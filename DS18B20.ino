#include <ESP8266WiFi.h>
#include <WiFiManager.h>   // https://github.com/tzapu/WiFiManager
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>  // For NTP time
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ==== MQTT ====
char mqtt_server[40] = "192.168.1.36";
char mqtt_port[6]    = "1883";
char mqtt_topic[50]  = "myds18b20/temp";


WiFiClient espClient;
PubSubClient client(espClient);

// ==== DS18B20 ====
#define ONE_WIRE_BUS 0
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ==== OLED ====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==== Previous reading storage ====
float prevTemp = NAN;
String prevTime = "--:--:--";
bool lastPublishFailed = false; // Track last publish status

// ==== Timezone (Jakarta UTC+7) ====
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

unsigned long lastBlink = 0;
bool invertState = false;

void reconnectMQTT() {
  while (!client.connected()) {
    if (client.connect("ESP01_DS18B20")) {
      display.invertDisplay(false); // Ensure normal when connected
    } else {
      unsigned long now = millis();
      if (now - lastBlink > 500) {
        invertState = !invertState;
        display.invertDisplay(invertState);
        lastBlink = now;
      }
      delay(100);
    }
  }
}

String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "--:--:--";
  }
  char timeStr[9];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  return String(timeStr);
}

void setup() {
  Serial.begin(115200);

  Wire.begin(2, 3);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 20);
  display.println("Starting...");
  display.display();

  // --- Load saved config from LittleFS ---
  loadConfig();

  WiFiManager wm;

  // --- Custom MQTT fields ---
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_topic("topic", "MQTT Topic", mqtt_topic, 50);

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_topic);

  // Try auto connect
  if (!wm.autoConnect("ESP-TempSetup")) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(5, 20);
    display.println("Failed WiFi!");
    display.display();
    delay(2000);
    ESP.restart();
  }

  // Optional: Allow config portal for 30s at boot
  wm.setConfigPortalTimeout(30);
  wm.startConfigPortal("ESP-TempSetup");

  // --- Save new values if updated ---
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port,   custom_mqtt_port.getValue());
  strcpy(mqtt_topic,  custom_mqtt_topic.getValue());

  saveConfig(); // 🔹 persist settings

  // --- Use new values ---
  client.setServer(mqtt_server, atoi(mqtt_port));

  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");
  sensors.begin();
}



void loop() {
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  String nowTime = getCurrentTime();

  // Prepare temp string
  char tempStr[8];
  dtostrf(tempC, 1, 2, tempStr);

  // Publish to MQTT
  if (!client.publish(mqtt_topic, tempStr)) {
    lastPublishFailed = true;
    unsigned long now = millis();
    if (now - lastBlink > 500) {
      invertState = !invertState;
      display.invertDisplay(invertState);
      lastBlink = now;
    }
  } else {
    lastPublishFailed = false;
    display.invertDisplay(false);
    // Only update prev values if publish successful
    prevTemp = tempC;
    prevTime = nowTime;
  }

  // Display current temp (big)
  display.clearDisplay();
  display.setTextSize(4);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(tempStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - h) / 2 - 10);
  display.print(tempStr);

  // Bottom line: previous temp & timestamp
  display.setTextSize(1);
  display.setCursor(0, SCREEN_HEIGHT - 8);
  if (!isnan(prevTemp)) {
    display.printf("%.2fC @ %s", prevTemp, prevTime.c_str());
  } else {
    display.print("--.--C @ --:--:--");
  }

  // Add "X" if last publish failed
  if (lastPublishFailed) {
    display.print(" X");
  }

  display.display();

  delay(5000);
}

bool loadConfig() {
  if (!LittleFS.exists("/config.json")) {
    Serial.println("No config file found");
    return false;
  }

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) return false;

  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  if (error) {
    Serial.println("Failed to read file");
    return false;
  }

  strlcpy(mqtt_server, doc["server"] | "192.168.1.36", sizeof(mqtt_server));
  strlcpy(mqtt_port,   doc["port"]   | "1883", sizeof(mqtt_port));
  strlcpy(mqtt_topic,  doc["topic"]  | "myds18b20/temp", sizeof(mqtt_topic));

  configFile.close();
  return true;
}

bool saveConfig() {
  StaticJsonDocument<200> doc;
  doc["server"] = mqtt_server;
  doc["port"]   = mqtt_port;
  doc["topic"]  = mqtt_topic;

  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) return false;

  if (serializeJson(doc, configFile) == 0) {
    Serial.println("Failed to write file");
    return false;
  }
  configFile.close();
  return true;
}
