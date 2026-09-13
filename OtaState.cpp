#include "OtaState.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace OtaState {

const char* kDir = "/ota";
const char* kStatePath = "/ota/state.json";
const char* kStateTmpPath = "/ota/state.json.tmp";
const char* kCandidatePath = "/ota/candidate.bin";
const char* kLastGoodPath = "/ota/last_good.bin";

namespace {

struct State {
  bool pendingValidation = false;
  uint8_t bootAttempts = 0;
  String pendingVersion;
  String lastFailedVersion;
  String lastFailedReason;
};

State g_state;

void resetToDefault() {
  g_state = State();
}

bool writeStateToDisk() {
  JsonDocument doc;
  doc["pending_validation"] = g_state.pendingValidation;
  doc["boot_attempts"] = g_state.bootAttempts;
  doc["pending_version"] = g_state.pendingVersion;
  doc["last_failed_version"] = g_state.lastFailedVersion;
  doc["last_failed_reason"] = g_state.lastFailedReason;

  File f = LittleFS.open(kStateTmpPath, "w");
  if (!f) {
    Serial.println("[OTA] ERROR: cannot open state tmp file for write");
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.close();
  if (written == 0) {
    Serial.println("[OTA] ERROR: state serialization wrote 0 bytes");
    LittleFS.remove(kStateTmpPath);
    return false;
  }

  // Zapis nejdriv do docasneho souboru a az pak prejmenovat - vypadek
  // napajeni behem serializeJson() tak necha puvodni state.json netknuty.
  // LittleFS.rename() cilovy soubor pokud existuje atomicky prepise sam
  // (viz lfs_rename), takze se NEMAZE predem - kdybychom ho smazali a
  // rename pak selhal, zustali bychom bez jakehokoliv state.json.
  if (!LittleFS.rename(kStateTmpPath, kStatePath)) {
    Serial.println("[OTA] ERROR: state rename failed");
    return false;
  }
  return true;
}

void readStateFromDisk() {
  resetToDefault();
  if (!LittleFS.exists(kStatePath)) {
    return; // prvni spusteni - vychozi cisty stav je spravne chovani
  }
  File f = LittleFS.open(kStatePath, "r");
  if (!f) {
    Serial.println("[OTA] WARNING: state.json exists but cannot be opened, using defaults");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("[OTA] WARNING: state.json corrupt (");
    Serial.print(err.c_str());
    Serial.println("), using defaults");
    resetToDefault();
    return;
  }
  g_state.pendingValidation = doc["pending_validation"] | false;
  g_state.bootAttempts = doc["boot_attempts"] | 0;
  g_state.pendingVersion = String((const char*)(doc["pending_version"] | ""));
  g_state.lastFailedVersion = String((const char*)(doc["last_failed_version"] | ""));
  g_state.lastFailedReason = String((const char*)(doc["last_failed_reason"] | ""));
}

} // namespace

bool begin() {
  // LittleFS.begin() sama o sobe naformatuje FS pri prvnim/poskozenem
  // pripojeni (LittleFSConfig autoFormat je vychozi true).
  if (!LittleFS.begin()) {
    Serial.println("[OTA] ERROR: LittleFS unavailable, OTA state persistence disabled");
    resetToDefault();
    return false;
  }
  if (!LittleFS.exists(kDir)) {
    LittleFS.mkdir(kDir);
  }
  readStateFromDisk();
  return true;
}

bool pendingValidation() { return g_state.pendingValidation; }
uint8_t bootAttempts() { return g_state.bootAttempts; }
String pendingVersionValue() { return g_state.pendingVersion; }
String lastFailedVersion() { return g_state.lastFailedVersion; }

bool hasLastGoodBackup() {
  return LittleFS.exists(kLastGoodPath);
}

bool isVersionMarkedFailed(const String& version) {
  return g_state.lastFailedVersion.length() > 0 && g_state.lastFailedVersion == version;
}

uint8_t recordBootAttempt() {
  if (!g_state.pendingValidation) {
    return g_state.bootAttempts;
  }
  g_state.bootAttempts++;
  writeStateToDisk();
  return g_state.bootAttempts;
}

void beginPendingValidation(const String& newVersion) {
  g_state.pendingValidation = true;
  g_state.bootAttempts = 0;
  g_state.pendingVersion = newVersion;
  writeStateToDisk();
}

void markBootValidated() {
  if (LittleFS.exists(kCandidatePath)) {
    // rename() pripadny existujici last_good.bin sam atomicky prepise.
    if (!LittleFS.rename(kCandidatePath, kLastGoodPath)) {
      Serial.println("[OTA] WARNING: failed to promote candidate to last_good backup");
    }
  }
  g_state.pendingValidation = false;
  g_state.bootAttempts = 0;
  g_state.pendingVersion = "";
  writeStateToDisk();
}

void clearStalePending() {
  g_state.pendingValidation = false;
  g_state.bootAttempts = 0;
  g_state.pendingVersion = "";
  writeStateToDisk();
}

void markUpdateFailed(const String& version, const String& reason) {
  g_state.lastFailedVersion = version;
  g_state.lastFailedReason = reason;
  g_state.pendingValidation = false;
  g_state.bootAttempts = 0;
  g_state.pendingVersion = "";
  writeStateToDisk();

  if (LittleFS.exists(kCandidatePath)) {
    LittleFS.remove(kCandidatePath);
  }

  Serial.print("[OTA] Firmware marked as failed: ");
  Serial.print(version);
  Serial.print(" (");
  Serial.print(reason);
  Serial.println(")");
}

} // namespace OtaState
