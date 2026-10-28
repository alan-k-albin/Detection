#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <WiFi.h>
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
#define DHT_TYPE DHT22

DHT dht(DHT_PIN, DHT_TYPE);

const int GAS_THRESHOLD = 1000;
const float TEMP_THRESHOLD = 35.0; // °C — flagged as dangerously high

// ================= SENSOR STATE =================
int gasValue = 0;
float temperature = NAN;
float humidity = NAN;
bool gasWarning = false;
bool tempWarning = false;

// Previous states — used to detect rising edge so we don't spam alerts
bool prevGasWarning = false;
bool prevTempWarning = false;

// ================= TIMING =================
unsigned long previousReadTime = 0;
const unsigned long READ_INTERVAL = 2000; // read & push every 2 seconds

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
  if (gasWarning || tempWarning) {
    display.fillRect(0, 51, 128, 13, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(5, 54);
    display.println(gasWarning ? "!! GAS WARNING !!" : "!! HIGH TEMP !!");
  } else {
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 54);
    display.println("Status: Monitoring");
  }

  display.display();
}

// ================= SETUP =================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(GAS_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
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
  unsigned long currentTime = millis();

  if (currentTime - previousReadTime >= READ_INTERVAL) {
    previousReadTime = currentTime;

    // 1. Read all sensors
    gasValue = analogRead(GAS_PIN);
    temperature = dht.readTemperature();
    humidity = dht.readHumidity();

    // 2. Evaluate warning conditions
    gasWarning = (gasValue >= GAS_THRESHOLD);
    tempWarning = (!isnan(temperature) && temperature >= TEMP_THRESHOLD);

    // 3. Buzzer on either warning
    digitalWrite(BUZZER_PIN, (gasWarning || tempWarning) ? HIGH : LOW);

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
  }
}
