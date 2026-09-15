#include "media.h"
#include "button_logic.h"
namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
ButtonLogic buttons[2];
struct Event {
  uint32_t sequence, ms;
  uint8_t button, type;
};
Event events[64];
uint32_t sequence = 0;
void (*clickCallback)();
void (*longCallback)();
void (*keyLongCallback)();
bool ready = false;
const char *names[] = {"KEY", "BOOT"};
const char *types[] = {"none", "single_click", "double_click", "long_press"};
String encode(cJSON *j) {
  char *raw = cJSON_PrintUnformatted(j);
  String s = raw ? raw : "{}";
  cJSON_free(raw);
  cJSON_Delete(j);
  return s;
}
void run(void *) {
  for (;;) {
    for (int i = 0; i < 2; ++i) {
      uint32_t now = millis();
      bool pressed = digitalRead(i == 0 ? 18 : 0) == LOW;
      portENTER_CRITICAL(&mux);
      int event = buttons[i].update(pressed, now);
      if (event) {
        ++sequence;
        events[(sequence - 1) % 64] = {sequence, now, (uint8_t)i, (uint8_t)event};
      }
      portEXIT_CRITICAL(&mux);
      if (event) {
        Serial.printf("BUTTON %s %s\n", names[i], types[event]);
        if (i == 0 && event == 1)
          mediaKeyClick();
        if (i == 0 && event == 2)
          mediaKeyDouble();
        if (i == 0 && event == 3 && keyLongCallback)
          keyLongCallback();
        if (i == 1 && event == 1 && clickCallback)
          clickCallback();
        if (i == 1 && event == 3 && longCallback)
          longCallback();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
} // namespace
void mediaStartButtons(void (*bootClick)(), void (*bootLong)(), void (*keyLong)()) {
  keyLongCallback = keyLong;
  clickCallback = bootClick;
  longCallback = bootLong;
  pinMode(18, INPUT_PULLUP);
  pinMode(0, INPUT_PULLUP);
  buttons[1].longMs = 3000;
  ready = xTaskCreate(run, "buttons", 4096, nullptr, 1, nullptr) == pdPASS;
}
cJSON *mediaButtonsStatus() {
  ButtonLogic copy[2];
  uint32_t seq;
  portENTER_CRITICAL(&mux);
  copy[0] = buttons[0];
  copy[1] = buttons[1];
  seq = sequence;
  portEXIT_CRITICAL(&mux);
  auto *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "ready", ready);
  cJSON_AddNumberToObject(j, "latest_sequence", seq);
  for (int i = 0; i < 2; ++i) {
    auto *b = cJSON_AddObjectToObject(j, names[i]);
    cJSON_AddBoolToObject(b, "pressed", copy[i].pressed);
    cJSON_AddNumberToObject(b, "held_ms", copy[i].pressed ? uint32_t(millis() - copy[i].down) : 0);
  }
  return j;
}
void buttonsRoutes(WebServer &s) {
  s.on("/buttons/status", HTTP_GET,
       [&s] { s.send(200, "application/json", encode(mediaButtonsStatus())); });
  s.on("/buttons/events", HTTP_GET, [&s] {
    String arg = s.hasArg("after") ? s.arg("after") : "0";
    if (arg.isEmpty() || arg.length() > 10) {
      s.send(400, "application/json", "{\"error\":\"invalid_sequence\"}");
      return;
    }
    for (char c : arg)
      if (c < '0' || c > '9') {
        s.send(400, "application/json", "{\"error\":\"invalid_sequence\"}");
        return;
      }
    uint64_t requested = strtoull(arg.c_str(), nullptr, 10);
    if (requested > UINT32_MAX) {
      s.send(400, "application/json", "{\"error\":\"invalid_sequence\"}");
      return;
    }
    uint32_t after = requested, last;
    Event snapshot[64];
    portENTER_CRITICAL(&mux);
    last = sequence;
    memcpy(snapshot, events, sizeof(snapshot));
    portEXIT_CRITICAL(&mux);
    uint32_t first = last >= 64 ? last - 63 : 1;
    auto *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "latest_sequence", last);
    cJSON_AddBoolToObject(j, "lost_events", after < first - 1);
    cJSON_AddBoolToObject(j, "cursor_reset", after > last);
    auto *list = cJSON_AddArrayToObject(j, "events");
    for (uint32_t n = first; n <= last && n > 0; ++n) {
      if (n <= after && after <= last)
        continue;
      const auto &e = snapshot[(n - 1) % 64];
      auto *item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "sequence", e.sequence);
      cJSON_AddNumberToObject(item, "timestamp_ms", e.ms);
      cJSON_AddStringToObject(item, "button", names[e.button]);
      cJSON_AddStringToObject(item, "type", types[e.type]);
      cJSON_AddItemToArray(list, item);
    }
    s.send(200, "application/json", encode(j));
  });
}
