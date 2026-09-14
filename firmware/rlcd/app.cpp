#include "ST7305_U8g2.h"
#include "config.h"
#include "media.h"
#include "agent_display.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <cJSON.h>
#include <atomic>

static ST7305_U8g2 lcd(11, 12, 5, 40, 41);
static U8G2 *display;
static WebServer http(80);
static Preferences prefs;
static bool storageOK, bleEnabled = false, bleReady = false;
static BLECharacteristic *bleStatus;
static BLEServer *bleServer;
static std::atomic<bool> restartAdvertising{false};
static std::atomic<int> lastWifiReason{0};
static std::atomic<bool> retryRequested{false};
struct Packet {
  char data[1024];
};
static QueueHandle_t commands;
static String ssid, password, source, phase = "starting", provisionResult = "idle";
static String message = "Send a message to POST /echo";
static bool connecting = false, candidate = false;
static uint32_t connectStarted, lastScreen, lastSensors;
static uint32_t lastConnectAttempt;
static std::atomic<unsigned> pageRequests{0};
static std::atomic<bool> provisionRequested{false};
static unsigned page = 0;
static float temperature, humidity, battery;
static bool shtOK, rtcOK, rtcValid;
static const char *shtError = "not_sampled";
static String rtcTime;

static String jsonText(cJSON *json) {
  char *raw = cJSON_PrintUnformatted(json);
  String result = raw ? raw : "{}";
  cJSON_free(raw);
  cJSON_Delete(json);
  return result;
}
static cJSON *parseObject(const String &body) {
  // cJSON strings are NUL terminated; reject embedded NUL instead of truncating input.
  for (size_t i = 0; i < body.length(); ++i) {
    if (body[i] == '\0')
      return nullptr;
    if (body[i] == '\\') {
      if (body.substring(i, i + 6) == "\\u0000")
        return nullptr;
      ++i;
    }
  }
  const char *end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(body.c_str(), body.length() + 1, &end, true);
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return nullptr;
  }
  return root;
}
static void updateBleStatus() {
  if (!bleReady)
    return;
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "state", phase.c_str());
  cJSON_AddStringToObject(j, "result", provisionResult.c_str());
  cJSON_AddStringToObject(j, "ip",
                          WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
  cJSON_AddBoolToObject(j, "provisioning", bleEnabled);
  bleStatus->setValue(jsonText(j).c_str());
}
class InputCallbacks : public BLECharacteristicCallbacks {
  String buffer;
  bool overflow = false;

public:
  void onWrite(BLECharacteristic *ch) override {
    String chunk = ch->getValue();
    for (size_t i = 0; i < chunk.length(); ++i) {
      char c = chunk[i];
      if (c == '\n') {
        Packet p = {};
        if (overflow)
          strcpy(p.data, "{}");
        else
          buffer.toCharArray(p.data, sizeof(p.data));
        if (buffer.length() || overflow)
          xQueueSend(commands, &p, 0);
        buffer = "";
        overflow = false;
      } else if (buffer.length() < 1023 && !overflow && c != '\0')
        buffer += c;
      else
        overflow = true;
    }
  }
};
class ServerCallbacks : public BLEServerCallbacks {
  void onDisconnect(BLEServer *) override { restartAdvertising.store(true); }
};
static void enableProvisioning() {
  if (!bleReady) {
    String name = "RLCD-" + WiFi.macAddress().substring(12);
    name.replace(":", "");
    BLEDevice::init(name.c_str());
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());
    BLEService *service = bleServer->createService(SERVICE_UUID);
    auto *input = service->createCharacteristic(WRITE_UUID, BLECharacteristic::PROPERTY_WRITE);
    input->setCallbacks(new InputCallbacks());
    bleStatus = service->createCharacteristic(STATUS_UUID, BLECharacteristic::PROPERTY_READ);
    service->start();
    BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
    BLEDevice::getAdvertising()->setScanResponse(true);
    bleReady = true;
  }
  bleEnabled = true;
  BLEDevice::startAdvertising();
  updateBleStatus();
}
static void connectWifi(const String &newSSID, const String &newPassword, bool isCandidate,
                        const String &newSource) {
  ssid = newSSID;
  password = newPassword;
  candidate = isCandidate;
  source = newSource;
  WiFi.STA.disconnect(false, 1000);
  // Arduino disconnect() is a no-op while connecting, so cancel at IDF level
  // before changing credentials (otherwise esp_wifi_set_config may fail).
  esp_wifi_disconnect();
  WiFi.begin(ssid.c_str(), password.c_str());
  connecting = true;
  connectStarted = millis();
  phase = "connecting";
  lastConnectAttempt = connectStarted;
  retryRequested.store(false);
  if (candidate)
    provisionResult = "connecting";
  updateBleStatus();
  Serial.printf("WIFI connecting source=%s\n", source.c_str());
}
static bool validCredentials(cJSON *j, String &s, String &p) {
  auto *a = cJSON_GetObjectItemCaseSensitive(j, "ssid");
  auto *b = cJSON_GetObjectItemCaseSensitive(j, "password");
  if (!cJSON_IsString(a) || !cJSON_IsString(b))
    return false;
  s = a->valuestring;
  p = b->valuestring;
  if (!s.length() || s.length() > 32)
    return false;
  if (!p.length())
    return true; // Open Wi-Fi
  if (p.length() >= 8 && p.length() <= 63)
    return true;
  if (p.length() != 64)
    return false;
  for (char c : p)
    if (!isxdigit(static_cast<unsigned char>(c)))
      return false;
  return true;
}
static void networkTick() {
  Packet packet;
  if (xQueueReceive(commands, &packet, 0) == pdTRUE) {
    cJSON *j = parseObject(packet.data);
    String s, p;
    if (!bleEnabled)
      provisionResult = "provisioning_closed";
    else if (connecting)
      provisionResult = "busy";
    else if (!j || !validCredentials(j, s, p))
      provisionResult = "invalid_credentials";
    else
      connectWifi(s, p, true, "provisioned");
    cJSON_Delete(j);
    updateBleStatus();
  }
  if (restartAdvertising.exchange(false) && bleEnabled)
    BLEDevice::startAdvertising();
  // Retry transient AP/auth failures inside the same bounded connection window.
  if (connecting && WiFi.status() != WL_CONNECTED && retryRequested.load() &&
      millis() - lastConnectAttempt >= 2000 && millis() - connectStarted < WIFI_TIMEOUT_MS) {
    retryRequested.store(false);
    lastConnectAttempt = millis();
    WiFi.STA.connect();
    Serial.println("WIFI retrying within connection deadline");
  }
  if (connecting && WiFi.status() == WL_CONNECTED) {
    connecting = false;
    phase = "connected";
    if (candidate) {
      // Single NVS value: the old pair remains intact until successful replacement.
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "ssid", ssid.c_str());
      cJSON_AddStringToObject(j, "password", password.c_str());
      String saved = jsonText(j);
      bool ok = storageOK && prefs.putString("credentials", saved) == saved.length();
      provisionResult = ok ? "saved" : "save_failed";
      if (ok) {
        source = "saved";
        bleEnabled = false;
        BLEDevice::getAdvertising()->stop();
      }
    } else
      provisionResult = "idle";
    candidate = false;
    Serial.printf("WIFI connected ip=%s source=%s result=%s\n", WiFi.localIP().toString().c_str(),
                  source.c_str(), provisionResult.c_str());
    updateBleStatus();
  } else if (connecting && millis() - connectStarted >= WIFI_TIMEOUT_MS) {
    connecting = false;
    esp_wifi_disconnect();
    phase = "waiting_for_ble";
    if (candidate)
      provisionResult = "connection_failed";
    candidate = false;
    enableProvisioning();
    Serial.println("WIFI timeout; waiting for BLE provisioning");
  } else if (!connecting && phase == "connected" && WiFi.status() != WL_CONNECTED) {
    connectWifi(ssid, password, false, source);
  }
}
static bool sensorCommand(uint16_t cmd) {
  Wire.beginTransmission(0x70);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 255);
  return Wire.endTransmission() == 0;
}
static uint8_t crc(const uint8_t *bytes) {
  uint8_t value = 0xff;
  for (int i = 0; i < 2; ++i) {
    value ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit)
      value = (value & 0x80) ? (value << 1) ^ 0x31 : value << 1;
  }
  return value;
}
static int bcd(uint8_t n) { return (n >> 4) * 10 + (n & 15); }
static void sampleSensors() {
  shtOK = false;
  shtError = "wakeup_failed";
  if (sensorCommand(0x3517)) {
    delay(2);
    shtError = "measurement_command_failed";
    if (sensorCommand(0x7866)) {
      delay(20);
      shtError = "measurement_read_failed";
      if (Wire.requestFrom(0x70, 6) == 6) {
        uint8_t v[6];
        for (auto &n : v)
          n = Wire.read();
        shtError = "crc_failed";
        if (crc(v) == v[2] && crc(v + 3) == v[5]) {
          temperature = -45 + 175.0f * ((v[0] << 8) | v[1]) / 65536;
          humidity = 100.0f * ((v[3] << 8) | v[4]) / 65536;
          shtOK = true;
          shtError = "none";
        }
      }
    }
    sensorCommand(0xb098);
  }
  uint32_t mv = 0;
  for (int i = 0; i < 8; ++i)
    mv += analogReadMilliVolts(4);
  battery = mv * 3.0f / 8000; // Official ADC1 channel 3, voltage divider 3:1.
  Wire.beginTransmission(0x51);
  Wire.write(0x04);
  rtcOK = Wire.endTransmission(false) == 0 && Wire.requestFrom(0x51, 7) == 7;
  rtcValid = false;
  rtcTime = "";
  if (rtcOK) {
    uint8_t r[7];
    for (auto &n : r)
      n = Wire.read();
    int sec = bcd(r[0] & 0x7f), min = bcd(r[1] & 0x7f), hour = bcd(r[2] & 0x3f);
    int day = bcd(r[3] & 0x3f), month = bcd(r[5] & 0x1f), year = 2000 + bcd(r[6]);
    rtcValid = !(r[0] & 0x80) && sec < 60 && min < 60 && hour < 24 && day >= 1 && day <= 31 &&
               month >= 1 && month <= 12;
    if (rtcValid) {
      char time[24];
      snprintf(time, sizeof(time), "%04d-%02d-%02dT%02d:%02d:%02d", year, month, day, hour, min,
               sec);
      rtcTime = time;
    }
  }
  lastSensors = millis();
}
static String statusJson() {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "firmware", "rlcd-0.4.0");
  cJSON_AddItemToObject(j, "audio", mediaAudioStatus());
  cJSON_AddItemToObject(j, "sd", mediaSDStatus());
  cJSON_AddItemToObject(j, "buttons", mediaButtonsStatus());
  cJSON_AddNumberToObject(j, "uptime_seconds", millis() / 1000);
  cJSON_AddNumberToObject(j, "free_heap_bytes", ESP.getFreeHeap());
  auto *w = cJSON_AddObjectToObject(j, "wifi");
  bool connected = WiFi.status() == WL_CONNECTED;
  cJSON_AddStringToObject(w, "state", phase.c_str());
  cJSON_AddStringToObject(w, "ssid", ssid.c_str());
  cJSON_AddStringToObject(w, "source", source.c_str());
  cJSON_AddNumberToObject(w, "last_disconnect_reason", lastWifiReason.load());
  cJSON_AddStringToObject(w, "ip", connected ? WiFi.localIP().toString().c_str() : "");
  if (connected)
    cJSON_AddNumberToObject(w, "rssi_dbm", WiFi.RSSI());
  else
    cJSON_AddNullToObject(w, "rssi_dbm");
  cJSON_AddBoolToObject(w, "provisioning", bleEnabled);
  cJSON_AddStringToObject(w, "provisioning_result", provisionResult.c_str());
  auto *s = cJSON_AddObjectToObject(j, "sensors");
  cJSON_AddNumberToObject(s, "sample_age_ms", millis() - lastSensors);
  auto *t = cJSON_AddObjectToObject(s, "shtc3");
  cJSON_AddStringToObject(t, "status", shtOK ? "ok" : "read_failed");
  cJSON_AddStringToObject(t, "error", shtError);
  if (shtOK) {
    cJSON_AddNumberToObject(t, "temperature_c", temperature);
    cJSON_AddNumberToObject(t, "humidity_percent", humidity);
  } else {
    cJSON_AddNullToObject(t, "temperature_c");
    cJSON_AddNullToObject(t, "humidity_percent");
  }
  auto *b = cJSON_AddObjectToObject(s, "battery");
  cJSON_AddNumberToObject(b, "voltage_v", battery);
  cJSON_AddStringToObject(b, "status", "measured");
  auto *r = cJSON_AddObjectToObject(s, "rtc");
  cJSON_AddStringToObject(r, "status", !rtcOK ? "read_failed" : rtcValid ? "ok" : "invalid_time");
  if (rtcValid)
    cJSON_AddStringToObject(r, "time", rtcTime.c_str());
  else
    cJSON_AddNullToObject(r, "time");
  return jsonText(j);
}
static void httpSetup() {
  mediaRoutes(http);
  agentRoutes(http);
  buttonsRoutes(http);
  http.on("/status", HTTP_GET, [] { http.send(200, "application/json", statusJson()); });
  http.on("/echo", HTTP_POST, [] {
    String body = http.arg("plain");
    if (body.length() > 2048) {
      http.send(413, "application/json", "{\"error\":\"body_too_large\"}");
      return;
    }
    cJSON *j = parseObject(body);
    auto *m = cJSON_GetObjectItemCaseSensitive(j, "message");
    bool valid = cJSON_IsString(m);
    String text = valid ? m->valuestring : "";
    cJSON_Delete(j);
    if (!valid || text.length() > 240) {
      http.send(400, "application/json", "{\"error\":\"message_must_be_ASCII_up_to_240_bytes\"}");
      return;
    }
    for (char c : text)
      if ((c < 32 || c > 126) && c != '\n') {
        http.send(400, "application/json", "{\"error\":\"ASCII_only\"}");
        return;
      }
    message = text;
    lastScreen = 0;
    auto *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "message", message.c_str());
    http.send(200, "application/json", jsonText(reply));
  });
  http.onNotFound([] { http.send(404, "application/json", "{\"error\":\"not_found\"}"); });
  http.begin();
}
static String ascii(const String &s) {
  String output;
  for (char c : s)
    output += (c >= 32 && c <= 126) ? c : '?';
  return output;
}
static void drawScreen() {
  if (page == 3) {
    agentDraw(*display, WiFi.localIP().toString(), bleEnabled);
    lastScreen = millis();
    return;
  }
  display->clearBuffer();
  display->setDrawColor(1);
  display->setFont(u8g2_font_helvB18_tr);
  display->drawStr(12, 28, page == 0 ? "RLCD Network" : page == 1 ? "RLCD Sensors" : "RLCD Audio");
  display->setFont(u8g2_font_6x13_tf);
  display->drawStr(12, 50, ("State: " + phase).c_str());
  display->drawStr(12, 68, ("SSID: " + ascii(ssid).substring(0, 50)).c_str());
  display->drawStr(
      12, 86,
      ("IP: " + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("--")) +
       "  HTTP:80")
          .c_str());
  char info[64];
  if (shtOK)
    snprintf(info, sizeof(info), "Temp: %.1f C  RH: %.1f%%  Bat: %.2f V", temperature, humidity,
             battery);
  else
    snprintf(info, sizeof(info), "Sensor unavailable  Bat: %.2f V", battery);
  display->drawStr(12, 108, info);
  display->drawHLine(12, 120, 376);
  if (page == 1) {
    display->drawStr(12, 145, ("RTC: " + (rtcValid ? rtcTime : String("unavailable"))).c_str());
    display->drawStr(12, 172, "SHTC3 / Battery ADC / PCF85063");
    display->drawStr(12, 199, "Press BOOT to switch pages");
  } else if (page == 2) {
    String state, file, detail;
    mediaScreen(state, file, detail);
    display->drawStr(12, 145, ("Audio: " + state).c_str());
    display->drawStr(12, 172, file.substring(0, 60).c_str());
    display->drawStr(12, 199, detail.substring(0, 60).c_str());
    display->drawStr(12, 226, "KEY: record/stop; double: replay");
  } else {
    int y = 141;
    String line;
    for (size_t i = 0; i < message.length(); ++i) {
      if (message[i] == '\n') {
        display->drawStr(12, y, line.c_str());
        line = "";
        y += 17;
      } else {
        line += message[i];
        if (line.length() == 60) {
          display->drawStr(12, y, line.c_str());
          line = "";
          y += 17;
        }
      }
      if (y > 243)
        break;
    }
    if (y <= 243)
      display->drawStr(12, y, line.c_str());
  }
  display->drawHLine(12, 260, 376);
  display->drawStr(12, 279,
                   bleEnabled ? "BLE setup available: open provision.html"
                              : "Hold BOOT 3s for Wi-Fi setup");
  display->sendBuffer();
  lastScreen = millis();
}
void appSetup() {
  Serial.begin(115200);
  delay(300);
  pinMode(0, INPUT_PULLUP);
  commands = xQueueCreate(2, sizeof(Packet));
  if (!commands) {
    Serial.println("Command queue allocation failed");
    while (true)
      delay(1000);
  }
  storageOK = prefs.begin("rlcd-wifi", false);
  lcd.begin(0, U8G2_R1);
  display = lcd.getU8g2();
  Wire.begin(13, 14);
  Wire.setTimeOut(50);
  analogSetPinAttenuation(4, ADC_11db);
  sensorCommand(0x3517);
  delay(2);
  sensorCommand(0x805d);
  delay(20);
  mediaSetup();
  mediaStartButtons([] { pageRequests.fetch_add(1); }, [] { provisionRequested.store(true); });
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        lastWifiReason.store(info.wifi_sta_disconnected.reason);
        if (info.wifi_sta_disconnected.reason != WIFI_REASON_ASSOC_LEAVE)
          retryRequested.store(true);
        Serial.printf("WIFI disconnected reason=%u\n", info.wifi_sta_disconnected.reason);
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  String saved = storageOK ? prefs.getString("credentials", "") : "";
  cJSON *j = parseObject(saved);
  String s, p;
  bool hasSaved = j && validCredentials(j, s, p);
  cJSON_Delete(j);
  if (hasSaved || DEFAULT_SSID[0]) {
    connectWifi(hasSaved ? s : DEFAULT_SSID, hasSaved ? p : DEFAULT_PASSWORD, false,
                hasSaved ? "saved" : "default");
  } else {
    source = "none";
    phase = "waiting_for_ble";
    enableProvisioning();
  }
  sampleSensors();
  httpSetup();
  drawScreen();
}
void appLoop() {
  // USB console offers the same provisioning action as the physical BOOT key.
  static String console;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (console == "provision") {
        enableProvisioning();
        Serial.println("BLE provisioning enabled");
      }
      if (console == "status")
        Serial.println(statusJson());
      if (console == "scan" && !connecting) {
        int count = WiFi.scanNetworks();
        int matches = 0;
        for (int i = 0; i < count; ++i)
          if (WiFi.SSID(i) == ssid) {
            ++matches;
            Serial.printf("WIFI target channel=%d rssi=%d auth=%d\n", WiFi.channel(i), WiFi.RSSI(i),
                          WiFi.encryptionType(i));
          }
        Serial.printf("WIFI scan result=%d target_matches=%d\n", count, matches);
        WiFi.scanDelete();
      }
      console = "";
    } else if (c != '\r') {
      if (console.length() < 32)
        console += c;
      else
        console = "";
    }
  }
  networkTick();
  http.handleClient();
  mediaAfterHttp();
  if (agentTakeFocus()) {
    page = 3;
    lastScreen = 0;
  }
  unsigned changes = pageRequests.exchange(0);
  if (changes) {
    page = (page + changes) % 4;
    lastScreen = 0;
  }
  if (provisionRequested.exchange(false))
    enableProvisioning();
  if (millis() - lastSensors >= 5000)
    sampleSensors();
  if (millis() - lastScreen >= (page == 3 ? 125u : 1000u))
    drawScreen();
  delay(2);
}
