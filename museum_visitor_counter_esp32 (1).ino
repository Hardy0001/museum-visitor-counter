/*
 ╔══════════════════════════════════════════════════════════╗
 ║   MUSEUM VISITOR COUNTER — ESP32 IoT Firmware            ║
 ║   Hardware: ESP32 + IR Sensor Pairs (HC-SR501 or TCRT5000)║
 ║   Features:                                               ║
 ║     • Entry / Exit detection via dual IR sensors          ║
 ║     • Wi-Fi connectivity + HTTP POST to dashboard server  ║
 ║     • Multi-room support (1 ESP32 per room)               ║
 ║     • OLED display (SSD1306) for local readout            ║
 ║     • NTP time sync                                       ║
 ╚══════════════════════════════════════════════════════════╝

  WIRING DIAGRAM:
  ──────────────────────────────────────────────────────
  IR Sensor 1 (ENTRY side)  → ESP32 GPIO 34  (INPUT_ONLY)
  IR Sensor 2 (EXIT side)   → ESP32 GPIO 35  (INPUT_ONLY)
  OLED SDA                  → ESP32 GPIO 21  (I2C SDA)
  OLED SCL                  → ESP32 GPIO 22  (I2C SCL)
  IR Sensors VCC            → 3.3V
  IR Sensors GND            → GND
  OLED VCC                  → 3.3V
  OLED GND                  → GND

  SENSOR PLACEMENT:
  ──────────────────────────────────────────────────────
  Place two IR sensors 15–20cm apart across the doorway.
  Direction is determined by WHICH sensor triggers FIRST.

  [ Door Frame ]
  |  IR_1 -----> IR_2  |  Entry if IR_1 triggers first
  |  IR_2 -----> IR_1  |  Exit  if IR_2 triggers first

  LIBRARIES REQUIRED (install via Arduino Library Manager):
  ──────────────────────────────────────────────────────
  • WiFi.h        (built-in ESP32)
  • HTTPClient.h  (built-in ESP32)
  • Wire.h        (built-in)
  • Adafruit_SSD1306
  • Adafruit_GFX
  • ArduinoJson   (v6+)
  • NTPClient
  • WiFiUdp
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ─── CONFIGURATION ───────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Your dashboard server endpoint (replace with your server IP or cloud URL)
const char* SERVER_URL    = "http://192.168.1.100:3000/api/visitor";

// Room identifier — change per ESP32 unit
const char* ROOM_ID       = "main_hall";
const char* ROOM_NAME     = "Main Hall";

// Maximum allowed people in this room (triggers alert)
const int   CAPACITY      = 150;

// ─── PIN DEFINITIONS ─────────────────────────────────────────────────
#define IR_SENSOR_1   34      // Entry-side IR sensor
#define IR_SENSOR_2   35      // Exit-side IR sensor
#define STATUS_LED    2       // Onboard LED (GPIO 2 on most ESP32 boards)
#define BUZZER_PIN    25      // Optional buzzer for capacity alert

// ─── OLED DISPLAY ────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1      // Reset pin (-1 if sharing reset with ESP32)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── NTP TIME ────────────────────────────────────────────────────────
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);  // UTC; adjust offset for your timezone

// ─── STATE VARIABLES ─────────────────────────────────────────────────
int  visitorCount   = 0;
int  totalEntries   = 0;
int  totalExits     = 0;
bool sensor1Active  = false;
bool sensor2Active  = false;
unsigned long sensor1Time = 0;
unsigned long sensor2Time = 0;
const int DETECTION_TIMEOUT = 2000;  // ms — max gap between paired sensor triggers
const int DEBOUNCE_MS       = 200;   // ms — debounce for sensor readings
unsigned long lastSend      = 0;
const int SEND_INTERVAL     = 5000;  // ms — how often to POST data to server
bool alertSent              = false;

// ─── SETUP ───────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n===  Museum Visitor Counter  ===");

  // Pin modes
  pinMode(IR_SENSOR_1, INPUT);
  pinMode(IR_SENSOR_2, INPUT);
  pinMode(STATUS_LED,  OUTPUT);
  pinMode(BUZZER_PIN,  OUTPUT);

  // OLED init
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed — continuing without display");
  } else {
    showSplash();
  }

  // Wi-Fi
  connectWiFi();

  // NTP
  timeClient.begin();
  timeClient.update();

  Serial.println("Setup complete. Monitoring...");
  updateDisplay();
}

// ─── MAIN LOOP ───────────────────────────────────────────────────────
void loop() {
  // Read sensors (LOW = object detected with most IR modules)
  bool s1 = (digitalRead(IR_SENSOR_1) == LOW);
  bool s2 = (digitalRead(IR_SENSOR_2) == LOW);

  unsigned long now = millis();

  // ── Sensor 1 triggered first → potential ENTRY ──
  if (s1 && !sensor1Active) {
    sensor1Active = true;
    sensor1Time   = now;
    Serial.println("[S1] Triggered");
  }
  if (s2 && !sensor2Active) {
    sensor2Active = true;
    sensor2Time   = now;
    Serial.println("[S2] Triggered");
  }

  // ── Determine direction ──
  if (sensor1Active && sensor2Active) {
    delay(DEBOUNCE_MS);
    if (sensor1Time < sensor2Time) {
      // S1 first → ENTRY
      handleEntry();
    } else {
      // S2 first → EXIT
      handleExit();
    }
    resetSensorState();
  }

  // ── Timeout stale triggers ──
  if (sensor1Active && (now - sensor1Time > DETECTION_TIMEOUT)) {
    Serial.println("[S1] Timed out — single trigger, ignoring");
    resetSensorState();
  }
  if (sensor2Active && (now - sensor2Time > DETECTION_TIMEOUT)) {
    Serial.println("[S2] Timed out — single trigger, ignoring");
    resetSensorState();
  }

  // ── Periodic data send ──
  if (now - lastSend >= SEND_INTERVAL) {
    sendData();
    lastSend = now;
  }

  // ── NTP update ──
  timeClient.update();

  delay(20);  // Small yield
}

// ─── EVENT HANDLERS ──────────────────────────────────────────────────
void handleEntry() {
  visitorCount++;
  totalEntries++;
  Serial.printf("[ENTRY] Count: %d  Total entries: %d\n", visitorCount, totalEntries);
  flashLED(1, 100, 0, 255, 0);   // Green flash
  updateDisplay();
  logEvent("ENTRY");

  // Capacity alert
  if (visitorCount >= CAPACITY && !alertSent) {
    Serial.println("!!! CAPACITY REACHED — sending alert !!!");
    buzzAlert();
    sendAlert("CAPACITY_FULL");
    alertSent = true;
  }
}

void handleExit() {
  if (visitorCount > 0) {
    visitorCount--;
    alertSent = false;  // Reset alert when count drops
  }
  totalExits++;
  Serial.printf("[EXIT]  Count: %d  Total exits: %d\n", visitorCount, totalExits);
  flashLED(1, 100, 255, 0, 0);   // Red flash
  updateDisplay();
  logEvent("EXIT");
}

void resetSensorState() {
  sensor1Active = false;
  sensor2Active = false;
  sensor1Time   = 0;
  sensor2Time   = 0;
}

// ─── DISPLAY ─────────────────────────────────────────────────────────
void showSplash() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(20, 10);
  display.print("Museum IoT Counter");
  display.setCursor(38, 25);
  display.print("Booting...");
  display.display();
  delay(1500);
}

void updateDisplay() {
  display.clearDisplay();

  // Title bar
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(ROOM_NAME);

  // Separator line
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  // Current count (large)
  display.setTextSize(3);
  String countStr = String(visitorCount);
  int x = (128 - countStr.length() * 18) / 2;
  display.setCursor(x, 16);
  display.print(countStr);

  // "INSIDE" label
  display.setTextSize(1);
  display.setCursor(44, 44);
  display.print("INSIDE");

  // Separator
  display.drawLine(0, 53, 127, 53, SSD1306_WHITE);

  // Footer stats
  display.setCursor(0, 56);
  display.print("IN:");
  display.print(totalEntries);
  display.setCursor(50, 56);
  display.print("OUT:");
  display.print(totalExits);

  // Time (top right)
  String t = timeClient.getFormattedTime().substring(0, 5);
  display.setCursor(128 - t.length() * 6, 0);
  display.print(t);

  display.display();
}

// ─── NETWORKING ──────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
    digitalWrite(STATUS_LED, HIGH);
  } else {
    Serial.println("\nWi-Fi failed — running in offline mode");
    digitalWrite(STATUS_LED, LOW);
  }
}

void sendData() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    return;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["room_id"]       = ROOM_ID;
  doc["room_name"]     = ROOM_NAME;
  doc["current_count"] = visitorCount;
  doc["total_entries"] = totalEntries;
  doc["total_exits"]   = totalExits;
  doc["capacity"]      = CAPACITY;
  doc["timestamp"]     = timeClient.getEpochTime();
  doc["uptime_ms"]     = millis();

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  Serial.printf("[HTTP] POST → %d\n", code);
  http.end();
}

void sendAlert(const char* alertType) {
  if (WiFi.status() != WL_CONNECTED) return;

  String alertURL = String(SERVER_URL) + "/alert";
  HTTPClient http;
  http.begin(alertURL);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> doc;
  doc["room_id"]    = ROOM_ID;
  doc["alert_type"] = alertType;
  doc["count"]      = visitorCount;
  doc["capacity"]   = CAPACITY;
  doc["timestamp"]  = timeClient.getEpochTime();

  String body;
  serializeJson(doc, body);
  http.POST(body);
  http.end();
}

void logEvent(const char* eventType) {
  Serial.printf("[LOG] %s | Room: %s | Count: %d | Time: %s\n",
    eventType, ROOM_ID, visitorCount,
    timeClient.getFormattedTime().c_str());
}

// ─── HELPERS ─────────────────────────────────────────────────────────
void flashLED(int times, int ms, int r, int g, int b) {
  for (int i = 0; i < times; i++) {
    digitalWrite(STATUS_LED, HIGH);
    delay(ms);
    digitalWrite(STATUS_LED, LOW);
    delay(ms);
  }
}

void buzzAlert() {
  // Three short beeps
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}
