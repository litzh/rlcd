#include "agent_display.h"
#include "media.h"
#include "pet_player.h"
#include <cmath>
#include <cstring>

namespace {
constexpr size_t TEXT_BYTES = 384 * 80 / 8;
uint8_t pixels[TEXT_BYTES] = {};
String owner, task, state = "idle";
uint32_t sequence = 0, updated = 0, ttl = 120000;
bool received = false, focus = false;
int progress = -1, soundCode = 0;
struct Watermark {
  String agent, task;
  uint32_t seq = 0;
};
Watermark history[64];
int historySize = 0;
int historyIndex(const String &a, const String &t) {
  for (int i = 0; i < historySize; ++i)
    if (history[i].agent == a && history[i].task == t)
      return i;
  return -1;
}
const char *states[] = {"idle", "working", "waiting_input", "success", "error"};
const char *labels[] = {"等待任务", "正在处理", "等待输入", "任务完成", "发生错误"};
int stateIndex(const String &s) {
  for (int i = 0; i < 5; ++i)
    if (s == states[i])
      return i;
  return -1;
}
bool expired() { return received && uint32_t(millis() - updated) >= ttl; }
String encode(cJSON *j) {
  char *raw = cJSON_PrintUnformatted(j);
  String result = raw ? raw : "{}";
  cJSON_free(raw);
  cJSON_Delete(j);
  return result;
}
int nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
bool idOK(cJSON *v) {
  if (!cJSON_IsString(v) || !v->valuestring[0] || strlen(v->valuestring) > 64)
    return false;
  for (const char *p = v->valuestring; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
          *p == '-' || *p == '_' || *p == '.'))
      return false;
  return true;
}
bool integer(cJSON *v, double low, double high) {
  return cJSON_IsNumber(v) && std::isfinite(v->valuedouble) && v->valuedouble >= low &&
         v->valuedouble <= high && floor(v->valuedouble) == v->valuedouble;
}
} // namespace

bool agentStandby() {
  return !received || expired() ||
         ((state == "idle" || state == "success") && uint32_t(millis() - updated) >= 30000);
}
cJSON *agentStatus() {
  auto *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "agent_id", owner.c_str());
  cJSON_AddStringToObject(j, "task_id", task.c_str());
  cJSON_AddNumberToObject(j, "seq", sequence);
  cJSON_AddBoolToObject(j, "expired", expired());
  cJSON_AddBoolToObject(j, "standby", agentStandby());
  cJSON_AddStringToObject(j, "pet_id", petCurrentId().c_str());
  cJSON_AddStringToObject(j, "state", expired() ? "stale" : state.c_str());
  if (progress < 0)
    cJSON_AddNullToObject(j, "progress");
  else
    cJSON_AddNumberToObject(j, "progress", progress);
  cJSON_AddNumberToObject(j, "age_ms", received ? uint32_t(millis() - updated) : 0);
  cJSON_AddNumberToObject(j, "ttl_seconds", ttl / 1000);
  cJSON_AddNumberToObject(j, "sound_http_code", soundCode);
  return j;
}

void agentRoutes(WebServer &s) {
  s.on("/agent/state", HTTP_GET, [&s] {
    if (s.hasArg("agent_id") || s.hasArg("task_id")) {
      if (!s.hasArg("agent_id") || !s.hasArg("task_id")) {
        s.send(400, "application/json", "{\"error\":\"both_ids_required\"}");
        return;
      }
      int i = historyIndex(s.arg("agent_id"), s.arg("task_id"));
      auto *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "agent_id", s.arg("agent_id").c_str());
      cJSON_AddStringToObject(j, "task_id", s.arg("task_id").c_str());
      cJSON_AddNumberToObject(j, "seq", i < 0 ? 0 : history[i].seq);
      s.send(200, "application/json", encode(j));
    } else
      s.send(200, "application/json", encode(agentStatus()));
  });
  s.on("/agent/state", HTTP_POST, [&s] {
    String raw = s.arg("plain");
    if (raw.length() > 10000) {
      s.send(413, "application/json", "{\"error\":\"body_too_large\"}");
      return;
    }
    // Reject NUL, including JSON escapes, so identifiers cannot be silently truncated.
    bool nul = false;
    for (size_t i = 0; i < raw.length(); ++i) {
      if (!raw[i])
        nul = true;
      if (raw[i] == '\\') {
        if (raw.substring(i, i + 6) == "\\u0000")
          nul = true;
        ++i;
      }
    }
    cJSON *j =
        nul ? nullptr : cJSON_ParseWithLengthOpts(raw.c_str(), raw.length() + 1, nullptr, true);
    auto field = [j](const char *key) { return cJSON_GetObjectItemCaseSensitive(j, key); };
    auto *a = field("agent_id"), *t = field("task_id"), *q = field("seq"), *v = field("state");
    auto *p = field("progress"), *life = field("ttl_seconds"), *bitmap = field("text_hex"),
         *sound = field("sound");
    auto *pet = field("pet_id");
    bool ok =
        cJSON_IsObject(j) && (!pet || (cJSON_IsString(pet) && strlen(pet->valuestring) <= 32)) &&
        idOK(a) && idOK(t) && integer(q, 1, 4294967295.0) && cJSON_IsString(v) &&
        stateIndex(v->valuestring) >= 0 && integer(life, 5, 86400) &&
        (!p || cJSON_IsNull(p) || integer(p, 0, 100)) && (!sound || cJSON_IsBool(sound)) &&
        (!bitmap || (cJSON_IsString(bitmap) && strlen(bitmap->valuestring) == TEXT_BYTES * 2));
    if (ok && bitmap)
      for (size_t i = 0; i < TEXT_BYTES * 2; ++i)
        if (nibble(bitmap->valuestring[i]) < 0) {
          ok = false;
          break;
        }
    if (!ok) {
      cJSON_Delete(j);
      s.send(400, "application/json", "{\"error\":\"invalid_agent_state\"}");
      return;
    }
    bool same = received && owner == a->valuestring && task == t->valuestring;
    int slot = historyIndex(a->valuestring, t->valuestring);
    if (slot >= 0 && q->valuedouble <= history[slot].seq) {
      cJSON_Delete(j);
      s.send(409, "application/json", "{\"error\":\"stale_sequence\"}");
      return;
    }
    if (slot < 0 && historySize == 64) {
      cJSON_Delete(j);
      s.send(503, "application/json", "{\"error\":\"task_history_full\"}");
      return;
    }
    bool changedPet = pet && petCurrentId() != pet->valuestring;
    if (pet) {
      int code = petSelectTemporary(pet->valuestring);
      if (code != 200) {
        cJSON_Delete(j);
        s.send(code, "application/json", "{\"error\":\"pet_unavailable\"}");
        return;
      }
    }
    if (slot < 0) {
      slot = historySize++;
      history[slot].agent = a->valuestring;
      history[slot].task = t->valuestring;
    }
    history[slot].seq = uint32_t(q->valuedouble);
    bool transition = changedPet || !same || state != v->valuestring || expired();
    if (bitmap) {
      for (size_t i = 0; i < TEXT_BYTES; ++i)
        pixels[i] =
            (nibble(bitmap->valuestring[2 * i]) << 4) | nibble(bitmap->valuestring[2 * i + 1]);
    } else if (!same)
      memset(pixels, 0, sizeof(pixels));
    owner = a->valuestring;
    task = t->valuestring;
    sequence = uint32_t(q->valuedouble);
    state = v->valuestring;
    ttl = uint32_t(life->valuedouble) * 1000;
    progress = p && cJSON_IsNumber(p) ? p->valueint : -1;
    updated = millis();
    received = true;
    focus = true;
    soundCode = 0;
    if (transition) {
      petTransition();
      if (cJSON_IsTrue(sound))
        soundCode = petSound(state);
    }
    cJSON_Delete(j);
    s.send(200, "application/json", encode(agentStatus()));
  });
}

bool agentTakeFocus() {
  bool value = focus;
  focus = false;
  return value;
}

void agentDraw(U8G2 &d, const String &ip, bool provisioning) {
  const bool stale = expired();
  const int index = stateIndex(state);
  const uint32_t frame = millis() / 125;
  d.clearBuffer();
  d.setDrawColor(1);
  d.setFont(u8g2_font_unifont_t_gb2312);
  d.drawUTF8(12, 23, labels[index]);
  d.setFont(u8g2_font_6x13_tf);
  d.drawStr(268, 21, ip.c_str());
  petDraw(d, stale ? "stale" : state);
  d.drawXBMP(8, 159, 384, 80, pixels);
  d.drawFrame(12, 245, 376, 10);
  if (!stale && progress >= 0)
    d.drawBox(14, 247, 372 * progress / 100, 6);
  else if (!stale && state == "working")
    d.drawBox(14 + (frame * 12) % 332, 247, 40, 6);
  d.setFont(u8g2_font_6x13_tf);
  d.drawStr(12, 276, provisioning ? "BLE setup available" : "BOOT: pages   KEY: record / stop");
  if (progress >= 0) {
    char text[8];
    snprintf(text, sizeof(text), "%d%%", progress);
    d.drawStr(350, 276, text);
  }
  d.sendBuffer();
}
