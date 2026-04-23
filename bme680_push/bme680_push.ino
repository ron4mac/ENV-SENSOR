/**
 * ESP32-C6 + BME680  —  Low-Power Push Sensor (no HTTP server)
 * Arduino IDE version
 *
 * Board package : esp32 by Espressif  (≥ 3.0.0)
 *   Board Manager URL: https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
 *   Board: "ESP32C6 Dev Module"
 *
 * Libraries (install via Library Manager):
 *   • Adafruit BME680 Library  (by Adafruit)
 *   • Adafruit Unified Sensor  (dependency, also by Adafruit)
 *   • ArduinoJson              (by Benoit Blanchon)  ← for clean JSON building
 *
 * Wiring:
 *   BME680  SDI/SDA  →  GPIO 6
 *   BME680  SCK/SCL  →  GPIO 7
 *   BME680  SDO      →  GND   (sets I2C address to 0x76)
 *   BME680  CSB      →  3V3
 *   BME680  VCC      →  3V3
 *   BME680  GND      →  GND
 *
 * Wake / measure / push / sleep cycle:
 *   1. Wake from deep sleep timer
 *   2. Read BME680 (forced / one-shot mode + gas heater)
 *   3. Connect to Wi-Fi
 *   4. HTTP POST JSON payload to SERVER_URL
 *   5. Disconnect Wi-Fi, enter deep sleep for SLEEP_SEC seconds
 *
 * Typical average current  ≈  200–280 µA  at 60-second intervals
 *   Deep sleep           ~  15 µA   (~57 s)
 *   BME680 gas heater    ~  14 mA   (~200 ms)
 *   Wi-Fi connect+POST   ~ 100 mA   (~2–3 s)
 *
 * SERVER_URL can be any HTTP endpoint that accepts a POST with a JSON body:
 *   • The included Node.js collector  (server/collector.js)
 *   • Home Assistant REST sensor
 *   • InfluxDB v2 write API
 *   • Any webhook / cloud function
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
// for web-based updating
#include <WebServer.h>
#include <ESP2SOTA.h>


/* ═══════════════════════════════════════════════════════════════════
 *  USER CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════ */

const char* WIFI_SSID      = "RnP UBS";
const char* WIFI_PASSWORD  = "rnp13eliacirc";
const char* DEVICE_NAME    = "esp32c6-bme680-1";

// Full URL of the endpoint that receives the POST
// e.g. "http://192.168.1.50:3000/api/reading"
const char* SERVER_URL     = "http://192.168.2.250:3000/api/reading";

// Deep-sleep interval between readings (seconds)
static constexpr uint32_t SLEEP_SEC = 300;
// Listen for update interval
static constexpr uint32_t LSTN_MILLIS = 20000;

// Wi-Fi + HTTP timeout (milliseconds)
static constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 8000;

// Gas heater: 320 °C for 150 ms (Bosch recommended general purpose)
static constexpr uint16_t GAS_HEATER_TEMP_C = 320;
static constexpr uint16_t GAS_HEATER_DUR_MS = 150;

/* ═══════════════════════════════════════════════════════════════════
 *  RTC RAM  —  survives deep sleep
 * ═══════════════════════════════════════════════════════════════════ */

RTC_DATA_ATTR uint32_t rtc_boot_count     = 0;
RTC_DATA_ATTR float    rtc_gas_baseline   = 0.0f;
RTC_DATA_ATTR uint8_t  rtc_baseline_count = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  Globals
 * ═══════════════════════════════════════════════════════════════════ */

Adafruit_BME680 bme(&Wire);

struct Reading {
  float    temperature_c;
  float    humidity_pct;
  float    pressure_hpa;
  float    gas_kohm;
  bool     gas_valid;
  uint16_t iaq;
  const char* iaq_label;
};

/* ═══════════════════════════════════════════════════════════════════
 *  IAQ heuristic (rolling baseline stored in RTC RAM)
 * ═══════════════════════════════════════════════════════════════════ */

static const char* iaqLabel(uint16_t iaq) {
  if (iaq <=  50) return "Excellent";
  if (iaq <= 100) return "Good";
  if (iaq <= 150) return "Lightly polluted";
  if (iaq <= 200) return "Moderately polluted";
  if (iaq <= 300) return "Heavily polluted";
  return "Severely polluted";
}

static uint16_t calculateIAQ(float gas_kohm, float humidity) {
  if (rtc_gas_baseline == 0.0f) rtc_gas_baseline = gas_kohm;

  if (rtc_baseline_count < 30) {
    rtc_gas_baseline = rtc_gas_baseline * 0.9f + gas_kohm * 0.1f;
    rtc_baseline_count++;
  } else {
    rtc_gas_baseline = rtc_gas_baseline * 0.99f + gas_kohm * 0.01f;
  }

  float gas_score = constrain((gas_kohm / rtc_gas_baseline) * 75.0f, 0.0f, 100.0f);

  float hum_offset = humidity - 40.0f;
  float hum_score  = hum_offset > 0.0f
      ? constrain(25.0f - (hum_offset / 60.0f * 25.0f), 0.0f, 25.0f)
      : constrain(25.0f + (hum_offset / 40.0f * 25.0f), 0.0f, 25.0f);

  float aq  = gas_score + hum_score;
  float iaq = (1.0f - (aq / 100.0f)) * 500.0f;
  return (uint16_t)constrain(iaq, 0.0f, 500.0f);
}

/* ═══════════════════════════════════════════════════════════════════
 *  BME680
 * ═══════════════════════════════════════════════════════════════════ */

bool sensorInit() {
  if (!bme.begin()) {
    Serial.println("[BME680] Not found — check wiring/address.");
    return false;
  }
  bme.setTemperatureOversampling(BME680_OS_2X);
  bme.setHumidityOversampling(BME680_OS_1X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(GAS_HEATER_TEMP_C, GAS_HEATER_DUR_MS);
  delay(5000);
  return true;
}

bool sensorRead(Reading &r) {
  if (!bme.performReading()) {
    Serial.println("[BME680] performReading() failed.");
    return false;
  }
  r.temperature_c = bme.temperature - 1;
  r.humidity_pct  = bme.humidity;
  r.pressure_hpa  = bme.pressure / 100.0f;
  r.gas_valid     = (bme.gas_resistance > 0);
  r.gas_kohm      = r.gas_valid ? bme.gas_resistance / 1000.0f : 0.0f;
  r.iaq           = r.gas_valid ? calculateIAQ(r.gas_kohm, r.humidity_pct) : 0;
  r.iaq_label     = r.gas_valid ? iaqLabel(r.iaq) : "Unavailable";

  Serial.printf("[BME680] T=%.2f°C  H=%.2f%%  P=%.2fhPa  Gas=%.1fkΩ  IAQ=%u (%s)\n",
    r.temperature_c, r.humidity_pct, r.pressure_hpa,
    r.gas_kohm, r.iaq, r.iaq_label);
  return true;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Wi-Fi
 * ═══════════════════════════════════════════════════════════════════ */

bool wifiConnect() {
  Serial.printf("[WiFi] Connecting to \"%s\" ...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t deadline = millis() + WIFI_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected — IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("[WiFi] Timed out.");
  return false;
}

/* ═══════════════════════════════════════════════════════════════════
 *  HTTP POST
 * ═══════════════════════════════════════════════════════════════════ */

bool httpPost(const Reading &r) {
  // Build JSON payload with ArduinoJson
  JsonDocument doc;
  doc["device"]        = DEVICE_NAME;
  doc["timestamp_ms"]  = millis();          // ms since this boot
  doc["boot_count"]    = rtc_boot_count;
  doc["temperature_c"] = serialized(String(r.temperature_c, 2));
  doc["temperature_f"] = serialized(String(r.temperature_c * 9.0f / 5.0f + 32.0f, 2));
  doc["humidity_pct"]  = serialized(String(r.humidity_pct, 2));
  doc["pressure_hpa"]  = serialized(String(r.pressure_hpa, 2));
  doc["gas_kohm"]      = serialized(String(r.gas_kohm, 2));
  doc["gas_valid"]     = r.gas_valid;
  doc["iaq"]           = r.iaq;
  doc["iaq_label"]     = r.iaq_label;
  doc["rssi_dbm"]      = WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  Serial.printf("[HTTP] POST %s\n", SERVER_URL);
  Serial.printf("[HTTP] Payload: %s\n", payload.c_str());

  int code = http.POST(payload);

  if (code > 0) {
    Serial.printf("[HTTP] Response: %d  %s\n", code, http.getString().c_str());
  } else {
    Serial.printf("[HTTP] Error: %s\n", http.errorToString(code).c_str());
  }

  http.end();

	if (code == 201) chk4update();

  return (code >= 200 && code < 300);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Deep sleep
 * ═══════════════════════════════════════════════════════════════════ */

void goToSleep() {
  Serial.printf("[SLEEP] Deep sleeping for %u s ...\n", SLEEP_SEC);
  Serial.flush();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SEC * 1000000ULL);
  esp_deep_sleep_start();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Entry point
 * ═══════════════════════════════════════════════════════════════════ */

void setup() {
  Serial.begin(115200);
  delay(100);

  rtc_boot_count++;
  Serial.printf("\n╔══════════════════════════════════════╗\n");
  Serial.printf("║  Boot #%-4lu  Wake: %d\n",
    (unsigned long)rtc_boot_count,
    (int)esp_sleep_get_wakeup_cause());
  Serial.printf("╚══════════════════════════════════════╝\n");

  // ── 1. Read sensor ──────────────────────────────────────────────
  Reading r = {};
  bool sensor_ok = sensorInit() && sensorRead(r);

  if (!sensor_ok) {
    Serial.println("[BOOT] Sensor failed — sleeping without posting.");
    goToSleep();
    return;
  }

  // ── 2. Connect Wi-Fi ────────────────────────────────────────────
  if (!wifiConnect()) {
    Serial.println("[BOOT] No Wi-Fi — sleeping without posting.");
    goToSleep();
    return;
  }

  // ── 3. POST data ─────────────────────────────────────────────────
  bool post_ok = httpPost(r);
  if (!post_ok) {
    Serial.println("[BOOT] POST failed.");
  }

  // ── 4. Check for update ──────────────────────────────────────────
//  Serial.println("Checking for update action.");
//  chk4update();

  // ── 5. Sleep ─────────────────────────────────────────────────────
  goToSleep();
}

void loop() {
  // Intentionally empty — device deep-sleeps; setup() re-runs each wake
}

void chk4update() {
  WebServer server(6680);
  ESP2SOTA.begin(&server);
  server.begin();
  Serial.printf("[LISTEN] Listening for %u millis ...\n", LSTN_MILLIS);
  uint32_t lstart = millis();
  while (millis() < (lstart + LSTN_MILLIS)) {
    server.handleClient();
  }
  server.close();
}