#include "pet_player.h"
#include "media.h"
#include <Preferences.h>
#include <cJSON.h>
#include <cmath>
#include <memory>
#include <mbedtls/sha256.h>

namespace {
constexpr size_t FRAME = 2560, LIMIT = 96 * FRAME + 4104;
const char *STATES[] = {"idle", "working", "waiting_input", "success", "error", "stale"};
struct Clip {
  uint32_t offset, count, ms;
  bool loop;
  String sound;
};
struct Pet {
  uint8_t *data = nullptr;
  size_t length = 0, payload = 0;
  String id, name, path;
  Clip clips[6];
  ~Pet() { free(data); }
};
std::unique_ptr<Pet> active;
Preferences prefs;
bool ready = false;
bool focusRequested = false;
String playbackState;
uint32_t started = 0;
int frameIndex = 0;
String statusError;
int indexOf(const String &state) {
  for (int i = 0; i < 6; ++i)
    if (state == STATES[i])
      return i;
  return 0;
}
bool assetPath(const String &p, const char *suffix) {
  if (!p.startsWith("/pet-") || !p.endsWith(suffix) || p.length() != 29 + strlen(suffix))
    return false;
  for (int i = 5; i < 29; ++i)
    if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
      return false;
  return true;
}
bool idOK(const char *v) {
  if (!v || !*v || strlen(v) > 32)
    return false;
  for (const char *p = v; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
          (p != v && (*p == '_' || *p == '-'))))
      return false;
  return true;
}
bool number(cJSON *v, int low, int high) {
  return cJSON_IsNumber(v) && std::isfinite(v->valuedouble) && v->valuedouble >= low &&
         v->valuedouble <= high && floor(v->valuedouble) == v->valuedouble;
}
String text(cJSON *j) {
  char *raw = cJSON_PrintUnformatted(j);
  String s = raw ? raw : "{}";
  cJSON_free(raw);
  cJSON_Delete(j);
  return s;
}
std::unique_ptr<Pet> load(const String &path) {
  if (!assetPath(path, ".rlp"))
    return nullptr;
  std::unique_ptr<Pet> p(new Pet());
  if (!mediaReadAsset(path, p->data, p->length, LIMIT) || p->length < 8 ||
      memcmp(p->data, "RLP1", 4))
    return nullptr;
  uint8_t digest[32];
  if (mbedtls_sha256(p->data, p->length, digest, 0) != 0)
    return nullptr;
  char hash[25];
  for (int i = 0; i < 12; ++i)
    snprintf(hash + i * 2, 3, "%02x", digest[i]);
  if (path.substring(5, 29) != hash)
    return nullptr;
  uint32_t size = uint32_t(p->data[4]) | uint32_t(p->data[5]) << 8 | uint32_t(p->data[6]) << 16 |
                  uint32_t(p->data[7]) << 24;
  if (!size || size > 4096 || size + 8 >= p->length)
    return nullptr;
  String header;
  header.reserve(size);
  for (uint32_t i = 0; i < size; ++i) {
    if (!p->data[8 + i])
      return nullptr;
    header += char(p->data[8 + i]);
  }
  if (header.indexOf("\\u0000") >= 0)
    return nullptr;
  cJSON *j = cJSON_ParseWithLengthOpts(header.c_str(), header.length() + 1, nullptr, true);
  auto *id = cJSON_GetObjectItemCaseSensitive(j, "id"),
       *name = cJSON_GetObjectItemCaseSensitive(j, "name"),
       *states = cJSON_GetObjectItemCaseSensitive(j, "states");
  bool ok = cJSON_IsObject(j) && cJSON_IsString(id) && idOK(id->valuestring) &&
            cJSON_IsString(name) && strlen(name->valuestring) > 0 &&
            strlen(name->valuestring) <= 96 && cJSON_IsArray(states) &&
            cJSON_GetArraySize(states) == 6;
  size_t expected = 0;
  if (ok)
    for (int i = 0; i < 6; ++i) {
      auto *clip = cJSON_GetArrayItem(states, i);
      auto *off = cJSON_GetObjectItemCaseSensitive(clip, "offset"),
           *count = cJSON_GetObjectItemCaseSensitive(clip, "count"),
           *ms = cJSON_GetObjectItemCaseSensitive(clip, "frame_ms"),
           *loop = cJSON_GetObjectItemCaseSensitive(clip, "loop"),
           *sound = cJSON_GetObjectItemCaseSensitive(clip, "sound");
      if (!number(off, 0, 96 * FRAME) || off->valueint != expected || !number(count, 1, 64) ||
          !number(ms, 125, 5000) || !cJSON_IsBool(loop) || !cJSON_IsString(sound)) {
        ok = false;
        break;
      }
      Clip &c = p->clips[i];
      c.offset = off->valueint;
      c.count = count->valueint;
      c.ms = ms->valueint;
      c.loop = cJSON_IsTrue(loop);
      c.sound = sound->valuestring;
      expected += c.count * FRAME;
      if (expected > 96 * FRAME ||
          (c.sound.length() && (!assetPath(c.sound, ".wav") || !mediaValidSound(c.sound)))) {
        ok = false;
        break;
      }
    }
  if (ok && size + 8 + expected != p->length)
    ok = false;
  if (ok) {
    p->id = id->valuestring;
    p->name = name->valuestring;
    p->path = path;
    p->payload = size + 8;
  }
  cJSON_Delete(j);
  if (!ok)
    return nullptr;
  return p;
}
cJSON *registry() {
  String raw = ready ? prefs.getString("registry", "") : "";
  cJSON *j = cJSON_Parse(raw.c_str());
  if (!cJSON_IsObject(j) || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(j, "pets"))) {
    cJSON_Delete(j);
    j = cJSON_CreateObject();
    cJSON_AddArrayToObject(j, "pets");
    cJSON_AddStringToObject(j, "active_path", "");
  }
  return j;
}
bool save(cJSON *j) {
  char *raw = cJSON_PrintUnformatted(j);
  bool ok = raw && ready && strlen(raw) <= 4096 && prefs.putString("registry", raw) == strlen(raw);
  cJSON_free(raw);
  return ok;
}
void reply(WebServer &s, int code, const char *value) {
  auto *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, code >= 400 ? "error" : "result", value);
  s.send(code, "application/json", text(j));
}
cJSON *body(WebServer &s) {
  String raw = s.arg("plain");
  if (raw.length() > 256 || raw.indexOf("\\u0000") >= 0)
    return nullptr;
  for (char c : raw)
    if (!c)
      return nullptr;
  return cJSON_ParseWithLengthOpts(raw.c_str(), raw.length() + 1, nullptr, true);
}
} // namespace

void petSetup() {
  ready = prefs.begin("rlcd-pets", false);
  auto *j = registry();
  auto *path = cJSON_GetObjectItemCaseSensitive(j, "active_path");
  if (cJSON_IsString(path) && path->valuestring[0]) {
    active = load(path->valuestring);
    if (!active)
      statusError = "saved_pet_unavailable";
    else
      focusRequested = true;
  }
  cJSON_Delete(j);
}
bool petTakeFocus() {
  bool result = focusRequested;
  focusRequested = false;
  return result;
}
void petTransition() {
  playbackState = "";
  started = millis();
  frameIndex = 0;
}
int petSound(const String &state) {
  if (!active)
    return 0;
  String path = active->clips[indexOf(state)].sound;
  return path.isEmpty() ? 0 : mediaPlaySound(path);
}
void petDraw(U8G2 &d, const String &state) {
  if (!active) {
    d.setFont(u8g2_font_6x13_tf);
    d.drawStr(130, 100, "Install a pet package");
    return;
  }
  if (playbackState != state) {
    playbackState = state;
    started = millis();
  }
  Clip &c = active->clips[indexOf(state)];
  uint32_t elapsed = uint32_t(millis() - started) / c.ms;
  frameIndex = c.loop ? elapsed % c.count : std::min(elapsed, c.count - 1);
  d.drawXBMP(120, 30, 160, 128, active->data + active->payload + c.offset + FRAME * frameIndex);
}
void petRoutes(WebServer &s) {
  s.on("/pets", HTTP_GET, [&s] {
    auto *j = registry();
    cJSON_AddStringToObject(j, "active_id", active ? active->id.c_str() : "");
    cJSON_AddStringToObject(j, "state", playbackState.c_str());
    cJSON_AddNumberToObject(j, "frame", frameIndex);
    cJSON_AddStringToObject(j, "error", statusError.c_str());
    s.send(200, "application/json", text(j));
  });
  s.on("/pets/register", HTTP_POST, [&s] {
    auto *input = body(s);
    auto *path = cJSON_GetObjectItemCaseSensitive(input, "path");
    auto pet = load(cJSON_IsString(path) ? path->valuestring : "");
    cJSON_Delete(input);
    if (!pet) {
      reply(s, 400, "invalid_pet_or_assets_unavailable");
      return;
    }
    auto *j = registry(), *pets = cJSON_GetObjectItemCaseSensitive(j, "pets");
    int found = -1;
    for (int i = 0; i < cJSON_GetArraySize(pets); ++i) {
      auto *id = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(pets, i), "id");
      if (cJSON_IsString(id) && pet->id == id->valuestring)
        found = i;
    }
    if (found < 0 && cJSON_GetArraySize(pets) >= 16) {
      cJSON_Delete(j);
      reply(s, 409, "pet_limit_reached");
      return;
    }
    auto *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "id", pet->id.c_str());
    cJSON_AddStringToObject(entry, "name", pet->name.c_str());
    cJSON_AddStringToObject(entry, "path", pet->path.c_str());
    if (found < 0)
      cJSON_AddItemToArray(pets, entry);
    else
      cJSON_ReplaceItemInArray(pets, found, entry);
    bool ok = save(j);
    cJSON_Delete(j);
    reply(s, ok ? 200 : 503, ok ? "registered" : "registry_save_failed");
  });
  s.on("/pets/use", HTTP_POST, [&s] {
    auto *input = body(s), *id = cJSON_GetObjectItemCaseSensitive(input, "id");
    String requested = cJSON_IsString(id) ? id->valuestring : "";
    cJSON_Delete(input);
    if (!idOK(requested.c_str())) {
      reply(s, 400, "invalid_id");
      return;
    }
    auto *j = registry(), *pets = cJSON_GetObjectItemCaseSensitive(j, "pets");
    String path;
    for (int i = 0; i < cJSON_GetArraySize(pets); ++i) {
      auto *entry = cJSON_GetArrayItem(pets, i),
           *key = cJSON_GetObjectItemCaseSensitive(entry, "id"),
           *file = cJSON_GetObjectItemCaseSensitive(entry, "path");
      if (cJSON_IsString(key) && requested == key->valuestring && cJSON_IsString(file))
        path = file->valuestring;
    }
    if (path.isEmpty()) {
      cJSON_Delete(j);
      reply(s, 404, "pet_not_registered");
      return;
    }
    auto candidate = load(path);
    if (!candidate) {
      cJSON_Delete(j);
      reply(s, 400, "pet_assets_unavailable");
      return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(j, "active_path");
    cJSON_AddStringToObject(j, "active_path", path.c_str());
    bool ok = save(j);
    cJSON_Delete(j);
    if (!ok) {
      reply(s, 503, "registry_save_failed");
      return;
    }
    active = std::move(candidate);
    focusRequested = true;
    petTransition();
    statusError = "";
    reply(s, 200, "selected");
  });
}
