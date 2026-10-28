#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <time.h>

// ================= FIREBASE CONFIG =================
// Fill these in after creating your Firebase project
const char *FIREBASE_HOST =
    "https://fxv-detection-default-rtdb.asia-southeast1.firebasedatabase.app/";
const char *FIREBASE_AUTH =
    "5ZwVmC5VkqUWeR2GEl5VMLd8flRv7DJo0qSiTa9i"; // Project Settings > Service
                                                // Accounts > Database Secrets

// ================= WIFI CREDENTIALS =================
const char *WIFI_SSID = "LAPTOP-S6L4UED5 7744";
const char *WIFI_PASSWORD = "ALAN1125";

WebServer server(80);

// ================= OLED CONFIGURATION =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ================= PIN & SENSOR CONFIGURATION =================
#define GAS_PIN 34
#define DHT_PIN 27
#define BUZZER_PIN 26
#define GREEN_LED_PIN 18
#define RED_LED1_PIN 19
#define RED_LED2_PIN 23
#define DHT_TYPE DHT22

DHT dht(DHT_PIN, DHT_TYPE);

const int GAS_WARNING_THRESHOLD = 950;
const int GAS_HIGH_THRESHOLD = 1800;
const float TEMP_THRESHOLD = 35.0; // °C — flagged as dangerously high

// ================= SENSOR STATE =================
int gasValue = 0;
float temperature = NAN;
float humidity = NAN;
bool gasWarning = false;
bool tempWarning = false;
int gasLevelState = 0;

// Previous states — used to detect rising edge so we don't spam alerts
bool prevGasWarning = false;
bool prevTempWarning = false;

// ================= TIMING =================
unsigned long previousReadTime = 0;
const unsigned long READ_INTERVAL = 2000; // read & push every 2 seconds
const unsigned long WARNING_CHIRP_DURATION = 150;

// ================= HOURLY AVERAGE ACCUMULATORS =================
double sumGas = 0;
double sumTemp = 0;
double sumHum = 0;
int sampleCount = 0;
String currentHourKey = ""; // e.g. "2026-10-28T10"

// ================= NTP TIME HELPERS =================

// Sync ESP32 clock from the internet (Indian Standard Time = UTC+5:30)
void syncTime() {
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time");
  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  Serial.println(retries < 20 ? " Done." : " Failed (no NTP).");
}

// Returns the current hour as a Firebase key, e.g. "2026-10-28T10"
String getHourKey() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "unknown";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H", &timeinfo);
  return String(buf);
}

// Human-readable label for the hourly table, e.g. "2026-10-28 10:00"
String getHourLabel() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "unknown";
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:00", &timeinfo);
  return String(buf);
}

// Unix timestamp for alert ordering
long getUnixTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return 0;
  return (long)mktime(&timeinfo);
}

// ================= FIREBASE REST API HELPERS =================

// PUT overwrites a path (used for current readings & hourly averages)
void firebasePut(String path, String jsonBody) {
  if (WiFi.status() != WL_CONNECTED)
    return;
  HTTPClient http;
  String url = String(FIREBASE_HOST) + path + "?auth=" + FIREBASE_AUTH;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(jsonBody);
  if (code < 0) {
    Serial.println("Firebase PUT error: " + http.errorToString(code));
  }
  http.end();
}

// POST creates a new child with a unique Firebase key (used for alerts)
void firebasePost(String path, String jsonBody) {
  if (WiFi.status() != WL_CONNECTED)
    return;
  HTTPClient http;
  String url = String(FIREBASE_HOST) + path + "?auth=" + FIREBASE_AUTH;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(jsonBody);
  if (code < 0) {
    Serial.println("Firebase POST error: " + http.errorToString(code));
  }
  http.end();
}

// ================= PUSH FUNCTIONS =================

// Sends the latest live readings to /sensors/current (overwrites each time)
void pushCurrentReadings() {
  String json = "{";
  json += "\"gas\":" + String(gasValue) + ",";
  json += "\"temp\":" + (isnan(temperature) ? "null" : String(temperature, 1)) +
          ",";
  json += "\"hum\":" + (isnan(humidity) ? "null" : String(humidity, 1)) + ",";
  json += "\"gas_warning\":" + String(gasWarning ? "true" : "false") + ",";
  json += "\"temp_warning\":" + String(tempWarning ? "true" : "false") + ",";
  json += "\"timestamp\":" + String(getUnixTimestamp());
  json += "}";
  firebasePut("/sensors/current.json", json);
}

// Computes averages from accumulators and stores at /sensors/hourly/<hour-key>
void pushHourlyAverage() {
  if (sampleCount == 0)
    return;

  float avgGas = sumGas / sampleCount;
  float avgTemp = sumTemp / sampleCount;
  float avgHum = sumHum / sampleCount;

  String json = "{";
  json += "\"avg_gas\":" + String(avgGas, 1) + ",";
  json += "\"avg_temp\":" + String(avgTemp, 1) + ",";
  json += "\"avg_hum\":" + String(avgHum, 1) + ",";
  json += "\"samples\":" + String(sampleCount) + ",";
  json += "\"hour_label\":\"" + getHourLabel() + "\"";
  json += "}";

  firebasePut("/sensors/hourly/" + currentHourKey + ".json", json);
  Serial.println("Hourly average pushed for: " + currentHourKey);
}

// Appends a new alert entry to /sensors/alerts (POST creates unique child key)
void logAlert(String type, String message) {
  String json = "{";
  json += "\"type\":\"" + type + "\",";
  json += "\"message\":\"" + message + "\",";
  json += "\"gas\":" + String(gasValue) + ",";
  json += "\"temp\":" + (isnan(temperature) ? "null" : String(temperature, 1)) +
          ",";
  json += "\"hum\":" + (isnan(humidity) ? "null" : String(humidity, 1)) + ",";
  json += "\"timestamp\":" + String(getUnixTimestamp());
  json += "}";

  firebasePost("/sensors/alerts.json", json);
  Serial.println("Alert logged: " + message);
}

// ================= LIVE WEBSITE HANDLERS =================

void handleSensorData() {
  String json = "{";
  json += "\"gas\":" + String(gasValue) + ",";
  json += "\"temp\":" + (isnan(temperature) ? "null" : String(temperature, 1)) +
          ",";
  json += "\"hum\":" + (isnan(humidity) ? "null" : String(humidity, 1)) + ",";
  json += "\"state\":" + String(gasLevelState);
  json += "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Home Gas &amp; Climate Monitor</title>
  <style>
    body { font-family: Arial, sans-serif; background: #0f172a; color: #f8fafc; text-align: center; margin: 0; padding: 20px; }
    h1 { font-size: 22px; margin-bottom: 15px; }
    .status-banner { padding: 15px; border-radius: 10px; font-weight: bold; font-size: 18px; max-width: 400px; margin: 0 auto 20px; }
    .safe { background: #16a34a; color: white; }
    .warn { background: #ea580c; color: white; }
    .danger { background: #dc2626; color: white; animation: blink 0.8s infinite; }
    @keyframes blink { 50% { opacity: 0.4; } }
    .card-grid { display: flex; flex-wrap: wrap; justify-content: center; gap: 15px; max-width: 420px; margin: 0 auto; }
    .card { background: #1e293b; padding: 20px; border-radius: 12px; width: 100%; box-sizing: border-box; box-shadow: 0 4px 10px rgba(0,0,0,0.3); }
    .label { font-size: 14px; color: #94a3b8; text-transform: uppercase; }
    .val { font-size: 34px; font-weight: bold; margin-top: 8px; }
  </style>
</head>
<body>
  <h1>Home Safety Monitor</h1>
  <div id="banner" class="status-banner safe">STATUS: NORMAL (GREEN LED)</div>
  <div class="card-grid">
    <div class="card">
      <div class="label">Gas Sensor Level</div>
      <div class="val" id="gas">--</div>
    </div>
    <div class="card">
      <div class="label">Temperature</div>
      <div class="val"><span id="temp">--</span> &deg;C</div>
    </div>
    <div class="card">
      <div class="label">Humidity</div>
      <div class="val"><span id="hum">--</span> %</div>
    </div>
  </div>
  <script>
    function fetchReadings() {
      fetch('/data').then(response => response.json()).then(data => {
        document.getElementById('gas').innerText = data.gas;
        document.getElementById('temp').innerText = data.temp === null ? '--' : data.temp;
        document.getElementById('hum').innerText = data.hum === null ? '--' : data.hum;
        const banner = document.getElementById('banner');
        if (data.state === 2) {
          banner.className = 'status-banner danger';
          banner.innerText = 'CRITICAL GAS LEVEL (2 RED LEDS)';
        } else if (data.state === 1) {
          banner.className = 'status-banner warn';
          banner.innerText = 'GAS WARNING (1 RED LED)';
        } else {
          banner.className = 'status-banner safe';
          banner.innerText = 'STATUS: NORMAL (GREEN LED)';
        }
      });
    }
    setInterval(fetchReadings, 2000);
    fetchReadings();
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ================= LED & BUZZER CONTROLLER =================

void updateIndicators() {
  if (gasValue >= GAS_HIGH_THRESHOLD) {
    gasLevelState = 2;
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(RED_LED1_PIN, HIGH);
    digitalWrite(RED_LED2_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
  } else if (gasValue >= GAS_WARNING_THRESHOLD) {
    gasLevelState = 1;
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(RED_LED1_PIN, HIGH);
    digitalWrite(RED_LED2_PIN, LOW);
    digitalWrite(BUZZER_PIN,
                 tempWarning ||
                         millis() - previousReadTime < WARNING_CHIRP_DURATION
                     ? HIGH
                     : LOW);
  } else {
    gasLevelState = 0;
    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(RED_LED1_PIN, LOW);
    digitalWrite(RED_LED2_PIN, LOW);
    digitalWrite(BUZZER_PIN, tempWarning ? HIGH : LOW);
  }
}

// ================= OLED DISPLAY =================

void showDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Top bar: IP address
  display.setCursor(0, 0);
  if (WiFi.status() == WL_CONNECTED) {
    display.print("IP:");
    display.println(WiFi.localIP());
  } else {
    display.println("WiFi: Disconnected");
  }
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  // Gas value + bar graph
  display.setCursor(0, 14);
  display.print("Gas: ");
  display.print(gasValue);
  int barWidth = map(constrain(gasValue, 0, 4095), 0, 4095, 0, 50);
  display.drawRect(75, 14, 52, 8, SSD1306_WHITE);
  display.fillRect(76, 15, barWidth, 6, SSD1306_WHITE);

  // Temperature
  display.setCursor(0, 26);
  display.print("Temp: ");
  if (isnan(temperature))
    display.println("Error");
  else {
    display.print(temperature, 1);
    display.println(" C");
  }

  // Humidity
  display.setCursor(0, 38);
  display.print("Hum:  ");
  if (isnan(humidity))
    display.println("Error");
  else {
    display.print(humidity, 1);
    display.println(" %");
  }

  // Bottom status bar
  if (gasLevelState == 2) {
    display.fillRect(0, 51, 128, 13, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(5, 54);
    display.println("!! CRITICAL GAS !!");
  } else if (gasWarning || tempWarning) {
    display.fillRect(0, 51, 128, 13, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(5, 54);
    display.println(gasWarning ? "!! GAS WARNING !!" : "!! HIGH TEMP !!");
  } else {
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 54);
    display.println("Status: SAFE (Green)");
  }

  display.display();
}

// ================= SETUP =================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(GAS_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED1_PIN, OUTPUT);
  pinMode(RED_LED2_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED1_PIN, LOW);
  digitalWrite(RED_LED2_PIN, LOW);
  analogReadResolution(12);
  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED init failed!");
    while (true)
      delay(1000);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 5);
  display.println("Connecting to WiFi...");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected! IP: " + WiFi.localIP().toString());
    server.on("/", handleRoot);
    server.on("/data", handleSensorData);
    server.begin();
    Serial.println("Live dashboard: http://" + WiFi.localIP().toString());
    syncTime();
    currentHourKey = getHourKey();

    display.clearDisplay();
    display.setCursor(0, 5);
    display.println("WiFi Connected!");
    display.println(WiFi.localIP());
    display.println("Firebase ready.");
    display.display();
    delay(3000);
  } else {
    Serial.println("WiFi Failed!");
    display.println("WiFi Failed!");
    display.display();
    delay(3000);
  }

  dht.begin();
}

// ================= MAIN LOOP =================

void loop() {
  if (WiFi.status() == WL_CONNECTED)
    server.handleClient();

  unsigned long currentTime = millis();

  if (currentTime - previousReadTime >= READ_INTERVAL) {
    previousReadTime = currentTime;

    // 1. Read all sensors
    gasValue = analogRead(GAS_PIN);
    temperature = dht.readTemperature();
    humidity = dht.readHumidity();

    // 2. Evaluate warning conditions
    gasWarning = (gasValue >= GAS_WARNING_THRESHOLD);
    tempWarning = (!isnan(temperature) && temperature >= TEMP_THRESHOLD);

    // 3. Update gas-stage LEDs and buzzer
    updateIndicators();

    // 4. Accumulate valid readings for hourly average
    if (!isnan(temperature) && !isnan(humidity)) {
      sumGas += gasValue;
      sumTemp += temperature;
      sumHum += humidity;
      sampleCount++;
    }

    // 5. Check if the hour has rolled over → push average & reset
    String newHourKey = getHourKey();
    if (newHourKey != "unknown" && newHourKey != currentHourKey &&
        currentHourKey != "") {
      pushHourlyAverage();
      sumGas = sumTemp = sumHum = 0;
      sampleCount = 0;
      currentHourKey = newHourKey;
    }

    // 6. Log alerts only on rising edge (transition from safe → warning)
    if (gasWarning && !prevGasWarning) {
      logAlert("gas",
               "Gas level exceeded threshold (" + String(gasValue) + ")");
    }
    if (tempWarning && !prevTempWarning) {
      logAlert("temperature",
               "High temperature detected (" + String(temperature, 1) + " C)");
    }
    prevGasWarning = gasWarning;
    prevTempWarning = tempWarning;

    // 7. Push live data to Firebase
    pushCurrentReadings();

    // 8. Update OLED
    showDisplay();

    Serial.print("Gas: ");
    Serial.print(gasValue);
    Serial.print(" | State: ");
    if (gasLevelState == 2)
      Serial.println("CRITICAL (2 Red LEDs)");
    else if (gasLevelState == 1)
      Serial.println("WARNING (1 Red LED)");
    else
      Serial.println("NORMAL (Green LED)");
  }

  updateIndicators();
}
