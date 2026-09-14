#include "media.h"
#include "audio_hw.h"
#include <SD_MMC.h>
#include <Preferences.h>
#include <esp_system.h>
#include <atomic>

namespace {
SemaphoreHandle_t mutex;
struct Guard {
  Guard() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
  ~Guard() { xSemaphoreGiveRecursive(mutex); }
};
QueueHandle_t jobs;
bool mounted = false, codecReady = false;
enum Mode { Idle, Recording, Playing };
Mode mode = Idle;
bool stopRequested = false;
String activePath, lastPath, lastRecording, error;
String transferPath, uploadTemp;
uint32_t processed = 0, total = 0;
int volume = 35;
Preferences mediaPrefs;
void (*cleanupUpload)() = nullptr;
struct Job {
  bool record;
  char path[121];
  uint32_t seconds;
};
constexpr uint32_t RATE = 16000, BYTES_SECOND = 32000, MAX_UPLOAD = 64 * 1024 * 1024;

String encode(cJSON *j) {
  char *raw = cJSON_PrintUnformatted(j);
  String s = raw ? raw : "{}";
  cJSON_free(raw);
  cJSON_Delete(j);
  return s;
}
void reply(WebServer &s, int code, const char *msg) {
  auto *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, code >= 400 ? "error" : "result", msg);
  s.send(code, "application/json", encode(j));
}
cJSON *body(WebServer &s) {
  String b = s.arg("plain");
  if (b.length() > 1024)
    return nullptr;
  for (size_t i = 0; i < b.length(); ++i) {
    if (b[i] == '\0')
      return nullptr;
    if (b[i] == '\\') {
      if (b.substring(i, i + 6) == "\\u0000")
        return nullptr;
      ++i;
    }
  }
  auto *j = cJSON_ParseWithLengthOpts(b.c_str(), b.length() + 1, nullptr, true);
  if (!cJSON_IsObject(j)) {
    cJSON_Delete(j);
    return nullptr;
  }
  return j;
}
bool pathOK(const String &p, bool root = false) {
  if (!p.length() || p.length() > 120 || p[0] != '/' || (!root && p == "/") ||
      p.indexOf("//") >= 0 || p.endsWith("/.") || p.indexOf("/./") >= 0 || p.indexOf("..") >= 0)
    return false;
  String lower = p;
  lower.toLowerCase();
  if (p.endsWith(".") || p.endsWith(" ") || p.indexOf("./") >= 0 || p.indexOf(" /") >= 0)
    return false;
  if (lower.indexOf(".rlcd-") >= 0 || (p.length() > 1 && p.endsWith("/")))
    return false;
  for (char c : p)
    if (c < 32 || c > 126 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|')
      return false;
  return true;
}
bool lockedPath(const String &p) {
  return (activePath.length() && p.equalsIgnoreCase(activePath)) ||
         (transferPath.length() && p.equalsIgnoreCase(transferPath));
}
uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t u32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put16(uint8_t *p, uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
}
void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    p[i] = v >> (8 * i);
}
void wavHeader(uint8_t *p, uint32_t bytes) {
  memset(p, 0, 44);
  memcpy(p, "RIFF", 4);
  put32(p + 4, 36 + bytes);
  memcpy(p + 8, "WAVEfmt ", 8);
  put32(p + 16, 16);
  put16(p + 20, 1);
  put16(p + 22, 1);
  put32(p + 24, RATE);
  put32(p + 28, BYTES_SECOND);
  put16(p + 32, 2);
  put16(p + 34, 16);
  memcpy(p + 36, "data", 4);
  put32(p + 40, bytes);
}
bool wavInfo(File &f, uint32_t &offset, uint32_t &bytes) {
  uint8_t h[16];
  if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
    return false;
  uint64_t end = uint64_t(u32(h + 4)) + 8;
  if (end > f.size() || end < 12)
    return false;
  bool format = false;
  for (int chunks = 0; chunks < 128 && uint64_t(f.position()) + 8 <= end; ++chunks) {
    if (f.read(h, 8) != 8)
      return false;
    uint32_t len = u32(h + 4), pos = f.position();
    if (uint64_t(pos) + len > end)
      return false;
    if (!memcmp(h, "fmt ", 4)) {
      if (len < 16 || f.read(h, 16) != 16)
        return false;
      format = u16(h) == 1 && u16(h + 2) == 1 && u32(h + 4) == RATE && u32(h + 8) == BYTES_SECOND &&
               u16(h + 12) == 2 && u16(h + 14) == 16;
      if (!format)
        return false;
    } else if (!memcmp(h, "data", 4)) {
      if (!format || len == 0 || (len % 2))
        return false;
      offset = pos;
      bytes = len;
      return true;
    }
    if (uint64_t(pos) + len + (len & 1) > end || !f.seek(pos + len + (len & 1)))
      return false;
  }
  return false;
}
int start(bool rec, String path, uint32_t seconds = 60) {
  Guard guard;
  if (!codecReady || !mounted)
    return 503;
  if (mode != Idle)
    return 409;
  if (rec && path.isEmpty()) {
    char n[80];
    snprintf(n, sizeof(n), "/recordings/rec-%lu-%08lx.wav", (unsigned long)millis(),
             (unsigned long)esp_random());
    path = n;
  }
  if (!pathOK(path))
    return 400;
  if (rec) {
    String lower = path;
    lower.toLowerCase();
    if (!lower.startsWith("/recordings/") || lower.substring(12).indexOf('/') >= 0 ||
        !lower.endsWith(".wav") || seconds < 1 || seconds > 600)
      return 400;
  }
  if (lockedPath(path))
    return 409;
  if (rec) {
    if (SD_MMC.exists(path))
      return 409;
    uint64_t free = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    if (free < uint64_t(seconds) * BYTES_SECOND + 65536)
      return 507;
  } else {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory())
      return 404;
    uint32_t off, len;
    if (!wavInfo(f, off, len))
      return 415;
  }
  Job j = {};
  j.record = rec;
  j.seconds = seconds;
  path.toCharArray(j.path, sizeof(j.path));
  mode = rec ? Recording : Playing;
  activePath = path;
  lastPath = path;
  processed = 0;
  total = rec ? seconds * BYTES_SECOND : 0;
  error = "";
  stopRequested = false;
  if (xQueueSend(jobs, &j, 0) != pdTRUE) {
    mode = Idle;
    activePath = "";
    return 503;
  }
  return 202;
}
int stop(Mode expected) {
  Guard g;
  if (mode != Idle && mode != expected)
    return 409;
  stopRequested = true;
  return 200;
}
void worker(void *) {
  Job j;
  int16_t stereo[1024], mono[512];
  for (;;) {
    xQueueReceive(jobs, &j, portMAX_DELAY);
    String path = j.path, temp = path + ".rlcd-part", failure;
    File f;
    uint32_t bytes = 0, limit = 0, offset = 0;
    int appliedVolume;
    bool created = false;
    {
      Guard g;
      appliedVolume = volume;
      if (j.record) {
        if (SD_MMC.exists(temp)) {
          failure = "recording_temp_exists";
        } else {
          f = SD_MMC.open(temp, FILE_WRITE);
          created = bool(f);
          uint8_t h[44];
          wavHeader(h, 0);
          if (!f || f.write(h, 44) != 44)
            failure = "sd_write_failed";
          limit = j.seconds * BYTES_SECOND;
        }
      } else {
        f = SD_MMC.open(path, FILE_READ);
        if (!f || !wavInfo(f, offset, limit) || !f.seek(offset))
          failure = "invalid_wav";
        total = limit;
      }
    }
    bool opened = false;
    if (failure.isEmpty()) {
      opened = audioHardwareStart(j.record, appliedVolume);
      if (!opened)
        failure = "codec_start_failed";
    }
    while (failure.isEmpty() && bytes < limit) {
      int desired;
      bool stopping;
      {
        Guard g;
        stopping = stopRequested;
        desired = volume;
      }
      if (stopping)
        break;
      size_t samples = min(size_t(512), size_t((limit - bytes) / 2));
      if (j.record) {
        if (!audioHardwareRead(stereo, samples * 4)) {
          failure = "audio_read_failed";
          break;
        }
        for (size_t i = 0; i < samples; ++i)
          mono[i] = stereo[2 * i]; // MIC1; preserve mono PCM on disk.
        Guard g;
        if (f.write((uint8_t *)mono, samples * 2) != samples * 2) {
          failure = "sd_write_failed";
          break;
        }
      } else {
        if (desired != appliedVolume) {
          if (!audioHardwareVolume(desired)) {
            failure = "volume_failed";
            break;
          }
          appliedVolume = desired;
        }
        {
          Guard g;
          if (f.read((uint8_t *)mono, samples * 2) != samples * 2) {
            failure = "sd_read_failed";
            break;
          }
        }
        for (size_t i = 0; i < samples; ++i)
          stereo[2 * i] = stereo[2 * i + 1] = mono[i];
        if (!audioHardwareWrite(stereo, samples * 4)) {
          failure = "audio_write_failed";
          break;
        }
      }
      bytes += samples * 2;
      {
        Guard g;
        processed = bytes;
      }
      taskYIELD();
    }
    // Let queued DMA audio finish before disabling the amplifier.
    if (opened && !j.record) {
      bool stopping;
      {
        Guard g;
        stopping = stopRequested;
      }
      if (!stopping && failure.isEmpty())
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (opened)
      audioHardwareStop(j.record);
    {
      Guard g;
      if (j.record && f && failure.isEmpty()) {
        uint8_t h[44];
        wavHeader(h, bytes);
        if (!f.seek(0) || f.write(h, 44) != 44)
          failure = "wav_finalize_failed";
        f.flush();
      }
      f.close();
      if (j.record) {
        if (failure.isEmpty() && bytes > 0) {
          if (SD_MMC.exists(path) || !SD_MMC.rename(temp, path))
            failure = "recording_rename_failed";
          else {
            lastRecording = path;
            mediaPrefs.putString("latest", path);
          }
        } else if (failure.isEmpty())
          failure = "empty_recording";
        if (failure.length() && created)
          SD_MMC.remove(temp);
      }
      error = failure;
      mode = Idle;
      activePath = "";
      stopRequested = false;
      Serial.printf("AUDIO finished bytes=%lu error=%s\n", (unsigned long)bytes, error.c_str());
    }
  }
}
} // namespace

void mediaSetup() {
  mutex = xSemaphoreCreateRecursiveMutex();
  jobs = xQueueCreate(1, sizeof(Job));
  if (!mutex || !jobs) {
    Serial.println("MEDIA allocation failed");
    while (true)
      delay(1000);
  }
  mediaPrefs.begin("rlcd-media", false);
  lastRecording = mediaPrefs.getString("latest", "");
  SD_MMC.setPins(38, 21, 39);
  mounted = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 8);
  if (mounted && !SD_MMC.exists("/recordings"))
    SD_MMC.mkdir("/recordings");
  codecReady = audioHardwareInit();
  if (xTaskCreate(worker, "audio_worker", 8192, nullptr, 2, nullptr) != pdPASS)
    codecReady = false;
  Serial.printf("MEDIA sd=%d codec=%d\n", mounted, codecReady);
}
cJSON *mediaAudioStatus() {
  Guard g;
  auto *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "ready", codecReady);
  cJSON_AddStringToObject(j, "state",
                          mode == Idle        ? "idle"
                          : mode == Recording ? "recording"
                                              : "playing");
  cJSON_AddStringToObject(j, "path", lastPath.c_str());
  cJSON_AddStringToObject(j, "last_recording", lastRecording.c_str());
  cJSON_AddNumberToObject(j, "processed_bytes", processed);
  cJSON_AddNumberToObject(j, "total_bytes", total);
  cJSON_AddNumberToObject(j, "position_seconds", processed / double(BYTES_SECOND));
  cJSON_AddNumberToObject(j, "volume", volume);
  cJSON_AddBoolToObject(j, "stopping", stopRequested && mode != Idle);
  if (error.length())
    cJSON_AddStringToObject(j, "error", error.c_str());
  else
    cJSON_AddNullToObject(j, "error");
  return j;
}
cJSON *mediaSDStatus() {
  Guard g;
  auto *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "mounted", mounted);
  if (mounted) {
    uint64_t all = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
    cJSON_AddNumberToObject(j, "capacity_bytes", SD_MMC.cardSize());
    cJSON_AddNumberToObject(j, "total_bytes", all);
    cJSON_AddNumberToObject(j, "used_bytes", used);
    cJSON_AddNumberToObject(j, "free_bytes", all - used);
  }
  cJSON_AddBoolToObject(j, "transfer_active", transferPath.length() > 0);
  return j;
}
void mediaScreen(String &state, String &file, String &detail) {
  Guard g;
  state = mode == Idle ? "idle" : mode == Recording ? "recording" : "playing";
  file = lastPath;
  detail = "Vol: " + String(volume) + "  Time: " + String(processed / BYTES_SECOND) +
           "s  SD: " + (mounted ? "ready" : "missing");
  if (error.length())
    detail = error;
}
void mediaKeyClick() {
  Mode m;
  {
    Guard g;
    m = mode;
  }
  if (m == Recording)
    stop(Recording);
  else if (m == Playing)
    stop(Playing);
  else {
    int code = start(true, "");
    if (code != 202) {
      Guard g;
      error = "key_record_failed_" + String(code);
    }
  }
}
void mediaKeyDouble() {
  String p;
  {
    Guard g;
    p = lastRecording;
  }
  int code = start(false, p);
  if (code != 202) {
    Guard g;
    error = "key_play_failed_" + String(code);
  }
}
void mediaAfterHttp() {
  if (cleanupUpload)
    cleanupUpload();
}

void mediaRoutes(WebServer &s) {
  s.on("/audio/status", HTTP_GET,
       [&s] { s.send(200, "application/json", encode(mediaAudioStatus())); });
  s.on("/sd/status", HTTP_GET, [&s] { s.send(200, "application/json", encode(mediaSDStatus())); });
  s.on("/audio/record/start", HTTP_POST, [&s] {
    auto *j = body(s);
    if (!j) {
      reply(s, 400, "invalid_json");
      return;
    }
    auto *p = cJSON_GetObjectItemCaseSensitive(j, "path"),
         *d = cJSON_GetObjectItemCaseSensitive(j, "max_seconds");
    if ((p && !cJSON_IsString(p)) ||
        (d && (!cJSON_IsNumber(d) || d->valuedouble < 1 || d->valuedouble > 600 ||
               d->valuedouble != int(d->valuedouble)))) {
      cJSON_Delete(j);
      reply(s, 400, "invalid_recording_options");
      return;
    }
    String path = p ? p->valuestring : "";
    int seconds = d ? d->valueint : 60;
    cJSON_Delete(j);
    int code = start(true, path, seconds);
    if (code == 202)
      s.send(code, "application/json", encode(mediaAudioStatus()));
    else
      reply(s, code, "recording_rejected");
  });
  s.on("/audio/record/stop", HTTP_POST, [&s] {
    int code = stop(Recording);
    reply(s, code, code == 200 ? "stop_requested" : "audio_busy");
  });
  s.on("/audio/play", HTTP_POST, [&s] {
    auto *j = body(s);
    auto *p = cJSON_GetObjectItemCaseSensitive(j, "path");
    if (!cJSON_IsString(p)) {
      cJSON_Delete(j);
      reply(s, 400, "path_required");
      return;
    }
    String path = p->valuestring;
    cJSON_Delete(j);
    int code = start(false, path);
    if (code == 202)
      s.send(code, "application/json", encode(mediaAudioStatus()));
    else
      reply(s, code, "playback_rejected");
  });
  s.on("/audio/stop", HTTP_POST, [&s] {
    int code = stop(Playing);
    reply(s, code, code == 200 ? "stop_requested" : "audio_busy");
  });
  s.on("/audio/volume", HTTP_PUT, [&s] {
    auto *j = body(s);
    auto *v = cJSON_GetObjectItemCaseSensitive(j, "volume");
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 100 ||
        v->valuedouble != int(v->valuedouble)) {
      cJSON_Delete(j);
      reply(s, 400, "volume_must_be_integer_0_to_100");
      return;
    }
    {
      Guard g;
      volume = v->valueint;
    }
    cJSON_Delete(j);
    s.send(200, "application/json", encode(mediaAudioStatus()));
  });
  s.on("/sd/files", HTTP_GET, [&s] {
    String path = s.hasArg("path") ? s.arg("path") : "/";
    if (!pathOK(path, true)) {
      reply(s, 400, "invalid_path");
      return;
    }
    int offset = s.hasArg("offset") ? s.arg("offset").toInt() : 0,
        limit = s.hasArg("limit") ? s.arg("limit").toInt() : 100;
    if (offset < 0 || limit < 1 || limit > 100) {
      reply(s, 400, "invalid_pagination");
      return;
    }
    Guard g;
    if (!mounted) {
      reply(s, 503, "sd_unavailable");
      return;
    }
    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
      reply(s, 404, "directory_not_found");
      return;
    }
    auto *j = cJSON_CreateObject();
    auto *items = cJSON_AddArrayToObject(j, "files");
    int index = 0, count = 0;
    bool more = false;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String name = f.name();
      if (name.indexOf(".rlcd-") >= 0)
        continue;
      if (index++ < offset)
        continue;
      if (count >= limit) {
        more = true;
        break;
      }
      auto *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", name.c_str());
      cJSON_AddBoolToObject(item, "directory", f.isDirectory());
      cJSON_AddNumberToObject(item, "size", f.size());
      cJSON_AddItemToArray(items, item);
      ++count;
    }
    if (more)
      cJSON_AddNumberToObject(j, "next_offset", offset + count);
    else
      cJSON_AddNullToObject(j, "next_offset");
    s.send(200, "application/json", encode(j));
  });
  s.on("/sd/file", HTTP_GET, [&s] {
    String path = s.arg("path");
    File f;
    {
      Guard g;
      if (!pathOK(path)) {
        reply(s, 400, "invalid_path");
        return;
      }
      if (!mounted) {
        reply(s, 503, "sd_unavailable");
        return;
      }
      if (lockedPath(path)) {
        reply(s, 409, "file_busy");
        return;
      }
      f = SD_MMC.open(path, FILE_READ);
      if (!f || f.isDirectory()) {
        reply(s, 404, "file_not_found");
        return;
      }
      transferPath = path;
    }
    s.streamFile(f, path.endsWith(".wav") ? "audio/wav" : "application/octet-stream");
    f.close();
    {
      Guard g;
      transferPath = "";
    }
  });
  s.on("/sd/file", HTTP_DELETE, [&s] {
    Guard g;
    String path = s.arg("path");
    if (!pathOK(path)) {
      reply(s, 400, "invalid_path");
      return;
    }
    if (!mounted) {
      reply(s, 503, "sd_unavailable");
      return;
    }
    if (lockedPath(path)) {
      reply(s, 409, "file_busy");
      return;
    }
    File f = SD_MMC.open(path);
    if (!f) {
      reply(s, 404, "file_not_found");
      return;
    }
    if (f.isDirectory()) {
      reply(s, 400, "directory_delete_not_supported");
      return;
    }
    f.close();
    if (!SD_MMC.remove(path)) {
      reply(s, 500, "delete_failed");
      return;
    }
    if (path.equalsIgnoreCase(lastRecording)) {
      lastRecording = "";
      mediaPrefs.remove("latest");
    }
    reply(s, 200, "deleted");
  });
  // Multipart streaming. Commit only after the entire HTTP request is parsed.
  static File upload;
  static int uploadCode = 400;
  static bool complete = false;
  static size_t uploaded = 0;
  static int fileCount = 0;
  // WebServer parses uploads synchronously. If parsing returned without reaching
  // the request/abort callback, release the uncommitted file after handleClient.
  cleanupUpload = [] {
    Guard g;
    if (!fileCount)
      return;
    upload.close();
    if (uploadTemp.length())
      SD_MMC.remove(uploadTemp);
    transferPath = "";
    uploadTemp = "";
    complete = false;
    uploadCode = 400;
    fileCount = 0;
  };
  s.on(
      "/sd/file", HTTP_POST,
      [&s] {
        Guard g;
        if (complete && uploadCode == 200 && fileCount == 1) {
          if (SD_MMC.exists(transferPath) || !SD_MMC.rename(uploadTemp, transferPath))
            uploadCode = 409;
        } else if (uploadCode == 200)
          uploadCode = 400;
        upload.close();
        if (uploadTemp.length())
          SD_MMC.remove(uploadTemp);
        reply(s, uploadCode, uploadCode == 200 ? "uploaded" : "upload_rejected");
        transferPath = "";
        uploadTemp = "";
        complete = false;
        uploadCode = 400;
        fileCount = 0;
      },
      [&s] {
        Guard g;
        auto &u = s.upload();
        if (u.status == UPLOAD_FILE_START) {
          if (fileCount++ > 0) {
            uploadCode = 400;
            upload.close();
            return;
          }
          complete = false;
          uploaded = 0;
          uploadCode = 200;
          String path = s.arg("path");
          if (!pathOK(path))
            uploadCode = 400;
          else if (!mounted)
            uploadCode = 503;
          else if (lockedPath(path) || SD_MMC.exists(path))
            uploadCode = 409;
          if (uploadCode != 200)
            return;
          transferPath = path;
          uploadTemp = "/.rlcd-upload-" + String(esp_random(), HEX);
          if (SD_MMC.exists(uploadTemp)) {
            uploadCode = 409;
            uploadTemp = "";
            return;
          }
          upload = SD_MMC.open(uploadTemp, FILE_WRITE);
          if (!upload)
            uploadCode = 500;
        } else if (u.status == UPLOAD_FILE_WRITE && uploadCode == 200) {
          if (uploaded + u.currentSize > MAX_UPLOAD)
            uploadCode = 413;
          else if (upload.write(u.buf, u.currentSize) != u.currentSize)
            uploadCode = 507;
          else
            uploaded += u.currentSize;
        } else if (u.status == UPLOAD_FILE_END) {
          upload.flush();
          upload.close();
          complete = true;
        } else if (u.status == UPLOAD_FILE_ABORTED) {
          upload.close();
          if (uploadTemp.length())
            SD_MMC.remove(uploadTemp);
          transferPath = "";
          uploadTemp = "";
          uploadCode = 400;
          complete = false;
          fileCount = 0;
        }
      });
}

bool mediaReadAsset(const String &path, uint8_t *&data, size_t &length, size_t maximum) {
  Guard guard;
  data = nullptr;
  length = 0;
  if (!mounted || !pathOK(path) || lockedPath(path))
    return false;
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory() || !file.size() || file.size() > maximum)
    return false;
  length = file.size();
  data = (uint8_t *)ps_malloc(length);
  if (!data)
    return false;
  if (file.read(data, length) != length) {
    free(data);
    data = nullptr;
    return false;
  }
  return true;
}
bool mediaValidSound(const String &path) {
  Guard guard;
  if (!mounted || !pathOK(path) || lockedPath(path))
    return false;
  File file = SD_MMC.open(path, FILE_READ);
  uint32_t offset, length;
  return file && !file.isDirectory() && wavInfo(file, offset, length) && length <= 160000;
}
int mediaPlaySound(const String &path) { return start(false, path); }
