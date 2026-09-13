#include "OtaManager.h"
#include "OtaConfig.h"
#include "OtaState.h"
#include "OtaVersion.h"
#include "Sha256.h"

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Updater.h>
#include <string.h>
#include <ctype.h>

namespace OtaManager {

namespace {

const size_t kChunkSize = 512;
const char* kUserAgent = "ESP8266-OTA-" FIRMWARE_TARGET;

StatusCallback g_statusCb = nullptr;
unsigned long g_lastCheckMs = 0;
bool g_firstCheckDone = false;
bool g_healthyNotified = false;

void reportStatus(const String& l1, const String& l2) {
  if (g_statusCb != nullptr) {
    g_statusCb(l1, l2);
  }
}

// Spolecny zacatek HTTP pozadavku - vsechny tri mista, ktera OTA pouziva
// (GitHub API, firmware.json, firmware.bin.sha256, firmware.bin), musi
// nasledovat presmerovani: "browser_download_url" GitHub Release assetu
// je VZDY 302 redirect na CDN (objects.githubusercontent.com), bez ohledu
// na to, jestli jde o firmware.bin nebo maly .json/.sha256 soubor. Puvodne
// to bylo nastavene jen pro stahovani firmware.bin, cimz stahovani
// firmware.json/firmware.bin.sha256 tise selhavalo na HTTP 302 - proto je
// to ted na jednom miste, aby se to nemohlo znovu rozjet jen pro cast pozadavku.
bool httpBeginCommon(HTTPClient& http, BearSSL::WiFiClientSecure& client, const String& url,
                      bool acceptJson) {
  if (!http.begin(client, url)) {
    Serial.println("[OTA] ERROR: HTTP begin failed");
    return false;
  }
  http.addHeader("User-Agent", kUserAgent);
  if (acceptJson) {
    http.addHeader("Accept", "application/vnd.github+json");
  }
  http.setTimeout(OTA_CONNECT_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  return true;
}

// --- GitHub Release JSON (filtrovane, aby se na ESP8266 nedeserializovaly
//     zbytecne velke JSON odpovedi do RAM) ---

bool fetchJson(BearSSL::WiFiClientSecure& client, HTTPClient& http, const String& url,
               JsonDocument& doc, const JsonDocument* filter) {
  if (!httpBeginCommon(http, client, url, true)) {
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.print("[OTA] HTTP GET failed, code=");
    Serial.println(code);
    http.end();
    return false;
  }

  DeserializationError err = (filter != nullptr)
      ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
      : deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    Serial.print("[OTA] JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }
  return true;
}

bool fetchLatestRelease(BearSSL::WiFiClientSecure& client, HTTPClient& http,
                         String& tagName, String& firmwareUrl, size_t& firmwareSize,
                         String& metadataUrl, String& checksumUrl) {
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;

  String url = String("https://api.github.com/repos/") + GITHUB_OWNER + "/" +
               GITHUB_REPOSITORY + "/releases/latest";

  JsonDocument doc;
  if (!fetchJson(client, http, url, doc, &filter)) {
    return false;
  }

  const char* tag = doc["tag_name"] | "";
  if (strlen(tag) == 0) {
    Serial.println("[OTA] Release not found");
    return false;
  }
  tagName = String(tag);
  firmwareUrl = "";
  metadataUrl = "";
  checksumUrl = "";
  firmwareSize = 0;

  if (doc["assets"].is<JsonArray>()) {
    for (JsonObject asset : doc["assets"].as<JsonArray>()) {
      const char* name = asset["name"] | "";
      if (strcmp(name, OTA_ASSET_FIRMWARE) == 0) {
        firmwareUrl = String((const char*)(asset["browser_download_url"] | ""));
        firmwareSize = asset["size"] | 0;
      } else if (strcmp(name, OTA_ASSET_METADATA) == 0) {
        metadataUrl = String((const char*)(asset["browser_download_url"] | ""));
      } else if (strcmp(name, OTA_ASSET_CHECKSUM) == 0) {
        checksumUrl = String((const char*)(asset["browser_download_url"] | ""));
      }
    }
  }
  return true;
}

struct Metadata {
  String target;
  size_t firmwareSize = 0;
  String sha256;
  bool valid = false;
};

Metadata fetchMetadata(BearSSL::WiFiClientSecure& client, HTTPClient& http, const String& url) {
  Metadata m;
  if (url.length() == 0) return m;

  JsonDocument doc;
  if (!fetchJson(client, http, url, doc, nullptr)) {
    return m;
  }
  m.target = String((const char*)(doc["target"] | ""));
  m.firmwareSize = doc["firmware_size"] | 0;
  m.sha256 = String((const char*)(doc["sha256"] | ""));
  m.valid = m.target.length() > 0;
  return m;
}

// Nacte a naparsuje "firmware.bin.sha256" - bezny format je bud samotny
// 64znakovy hex retezec, nebo "hex  nazev_souboru" (format sha256sum).
// Vraci prvnich 64 po sobe jdoucich hex znaku, nebo prazdny retezec.
String fetchChecksumFile(BearSSL::WiFiClientSecure& client, HTTPClient& http, const String& url) {
  if (url.length() == 0) return "";
  if (!httpBeginCommon(http, client, url, false)) return "";

  int code = http.GET();
  if (code != 200) {
    http.end();
    return "";
  }
  String payload = http.getString();
  http.end();

  String hex = "";
  for (size_t i = 0; i < payload.length() && hex.length() < 64; i++) {
    char c = payload[i];
    if (isxdigit(static_cast<unsigned char>(c))) {
      hex += c;
    } else if (hex.length() > 0) {
      break;
    }
  }
  return (hex.length() == 64) ? hex : String("");
}

enum class ApplyResult { Success, Cancelled, Failed };

// Stahne firmware.bin, streamuje ho soucasne do Update (OTA flash oblast) a
// do /ota/candidate.bin (zaloha pro pripad, ze se tato verze pozdeji ukaze
// jako spatna a bude potreba rollback), pocita SHA-256 za behu a na konci
// overi checksum. Update.end() se vola az PO uspesnem overeni checksumu -
// do te doby zustava soucasny (bezici) firmware zcela netknuty.
ApplyResult downloadAndApply(BearSSL::WiFiClientSecure& client, HTTPClient& http,
                              const String& url, size_t declaredSize,
                              const String& expectedSha256, const String& versionForPending) {
  size_t ceiling = ESP.getFreeSketchSpace();

  Serial.print("[OTA] Firmware size: ");
  Serial.print(declaredSize);
  Serial.println(" bytes");
  Serial.print("[OTA] OTA partition size: ");
  Serial.print(ceiling);
  Serial.println(" bytes");

  if (declaredSize > 0 && declaredSize > ceiling) {
    Serial.println("[OTA] ERROR: Firmware too large");
    Serial.println("[OTA] Update cancelled");
    return ApplyResult::Cancelled;
  }
  Serial.println("[OTA] Size check: OK");

  if (!httpBeginCommon(http, client, url, false)) {
    return ApplyResult::Failed;
  }

  Serial.println("[OTA] Downloading firmware...");
  int code = http.GET();
  if (code != 200) {
    Serial.print("[OTA] Download failed, HTTP code=");
    Serial.println(code);
    http.end();
    return ApplyResult::Failed;
  }

  int contentLength = http.getSize();
  if (contentLength > 0 && size_t(contentLength) > ceiling) {
    Serial.println("[OTA] ERROR: Firmware too large (Content-Length)");
    Serial.println("[OTA] Update cancelled");
    http.end();
    return ApplyResult::Cancelled;
  }

  // size predany do Update.begin() je zamerne konzervativni HORNI MEZ
  // (cela volna OTA oblast), ne presna ocekavana velikost - Content-Length
  // se nepovazuje za plne duveryhodny (viz sekce 4 zadani). Skutecna presnost
  // se overuje jinak: (1) Update.write() sam odmitne zapsat vic, nez kolik
  // zbyva mista - viz kontrola "written != n" nize, (2) SHA-256 na konci.
  if (!Update.begin(ceiling, U_FLASH)) {
    Serial.print("[OTA] ERROR: Update.begin failed: ");
    Serial.println(Update.getErrorString());
    http.end();
    return ApplyResult::Failed;
  }

  File candidate = LittleFS.open(OtaState::kCandidatePath, "w");
  bool backupOk = bool(candidate);
  if (!backupOk) {
    Serial.println("[OTA] WARNING: cannot open candidate.bin, rollback backup unavailable for this cycle");
  }

  Sha256 sha;
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[kChunkSize];
  size_t total = 0;
  unsigned long startMs = millis();
  bool sizeExceeded = false;
  bool writeFailed = false;
  bool timedOut = false;
  int lastLoggedPercent = -1;

  while (http.connected() && (contentLength <= 0 || total < size_t(contentLength))) {
    if (millis() - startMs > OTA_DOWNLOAD_TIMEOUT_MS) {
      timedOut = true;
      break;
    }
    int availInt = stream->available();
    if (availInt <= 0) {
      delay(1);
      continue;
    }
    size_t toRead = size_t(availInt) > sizeof(buf) ? sizeof(buf) : size_t(availInt);
    size_t n = stream->readBytes(buf, toRead);
    if (n == 0) {
      continue;
    }

    sha.update(buf, n);

    size_t written = Update.write(buf, n);
    if (written != n) {
      writeFailed = true;
      if (Update.getError() == UPDATE_ERROR_SPACE) {
        sizeExceeded = true;
      }
      break;
    }

    if (backupOk) {
      size_t bw = candidate.write(buf, n);
      if (bw != n) {
        Serial.println("[OTA] WARNING: candidate.bin backup write failed, continuing without backup");
        backupOk = false;
      }
    }

    total += n;
    if (contentLength > 0) {
      int percent = int((total * 100UL) / size_t(contentLength));
      if (percent >= lastLoggedPercent + 10) {
        Serial.print("[OTA] Download progress: ");
        Serial.print(percent);
        Serial.println("%");
        lastLoggedPercent = percent;
        char l2[24];
        snprintf(l2, sizeof(l2), "%d %%", percent);
        reportStatus("Stahuji firmware", String(l2));
      }
    }
    delay(0);
  }

  http.end();
  if (candidate) {
    candidate.close();
    if (!backupOk) {
      LittleFS.remove(OtaState::kCandidatePath);
    }
  }

  if (timedOut) {
    Serial.println("[OTA] ERROR: Download timed out");
    Serial.println("[OTA] Update cancelled");
    return ApplyResult::Failed;
  }
  if (writeFailed) {
    if (sizeExceeded) {
      Serial.println("[OTA] ERROR: Firmware too large (exceeded OTA partition during write)");
    } else {
      Serial.print("[OTA] ERROR: OTA write failed: ");
      Serial.println(Update.getErrorString());
    }
    Serial.println("[OTA] Update cancelled");
    return ApplyResult::Failed;
  }
  if (contentLength > 0 && total != size_t(contentLength)) {
    Serial.println("[OTA] ERROR: Download incomplete (connection closed early)");
    Serial.println("[OTA] Update cancelled");
    return ApplyResult::Failed;
  }
  if (declaredSize > 0 && total != declaredSize) {
    Serial.println("[OTA] ERROR: Downloaded size does not match declared firmware size");
    Serial.println("[OTA] Update cancelled");
    return ApplyResult::Failed;
  }

  uint8_t digest[32];
  sha.finish(digest);
  char hex[65];
  Sha256::toHex(digest, hex);

  if (expectedSha256.length() == 0) {
    if (OTA_REQUIRE_CHECKSUM) {
      Serial.println("[OTA] ERROR: No checksum available (required)");
      Serial.println("[OTA] Update cancelled");
      return ApplyResult::Cancelled;
    }
    Serial.println("[OTA] WARNING: proceeding without checksum verification (OTA_REQUIRE_CHECKSUM=false)");
  } else {
    Serial.print("[OTA] Verifying SHA-256... ");
    Serial.println(hex);
    if (!Sha256::hexEquals(digest, expectedSha256.c_str())) {
      Serial.println("[OTA] ERROR: Checksum mismatch");
      Serial.println("[OTA] Update cancelled");
      return ApplyResult::Failed;
    }
    Serial.println("[OTA] Verification successful");
  }

  reportStatus("Aktualizace FW", "Zapisuji...");
  Serial.println("[OTA] Starting OTA...");

  // Ulozit "pending validation" stav JESTE PRED Update.end(), ne az po nem.
  // Duvod: Update.end(true) primi eboot k provedeni swapu pri pristim
  // restartu - jakmile jednou uspesne vrati true, je (z pohledu eboot)
  // update jiz "nastaveny", i kdyby ESP.restart() nikdy nebyl zavolan kvuli
  // vypadku napajeni. Kdybychom pending stav zapsali az PO Update.end(),
  // existovalo by okno, kdy eboot pri dalsim bootu novy firmware nahraje,
  // ale nase sledovani o tom nebude vedet a rollback ochrana by se
  // neaktivovala. Timto poradim je pending stav na disku driv, nez muze
  // eboot swap vubec nastat.
  OtaState::beginPendingValidation(versionForPending);

  if (!Update.end(true)) {
    Serial.print("[OTA] ERROR: OTA finalize failed: ");
    Serial.println(Update.getErrorString());
    Serial.println("[OTA] Update cancelled");
    // Update.end() selhal => eboot NENI naveden na swap, soucasny firmware
    // zustava aktivni. Nas "pending" zapis byl tedy predcasny - vycistit ho
    // hned, misto cekani na sebeopravu az pri pristim bootu.
    OtaState::clearStalePending();
    return ApplyResult::Failed;
  }

  Serial.println("[OTA] OTA completed");
  return ApplyResult::Success;
}

// Reflashne /ota/last_good.bin (pouzije stejnou Update cestu jako bezne OTA,
// jen zdrojem bajtu je LittleFS soubor misto site) a restartuje. Vraci false,
// pokud rollback nelze provest (zadna zaloha nebo chyba pri zapisu) - v tom
// pripade zarizeni pokracuje v bootu se soucasnym (nevalidovanym) firmware.
bool performRollback(const String& reason) {
  String failingVersion = OtaState::pendingVersionValue();

  if (!OtaState::hasLastGoodBackup()) {
    Serial.println("[OTA] ERROR: Rollback unavailable (no backup) - continuing with current firmware");
    OtaState::markUpdateFailed(failingVersion, reason + "_no_backup");
    return false;
  }

  Serial.println("[OTA] Rollback detected: restoring last known-good firmware");
  reportStatus("Rollback FW", "Obnovuji...");

  File f = LittleFS.open(OtaState::kLastGoodPath, "r");
  if (!f) {
    Serial.println("[OTA] ERROR: cannot open last_good.bin for rollback");
    OtaState::markUpdateFailed(failingVersion, reason + "_backup_unreadable");
    return false;
  }

  size_t fileSize = f.size();
  size_t ceiling = ESP.getFreeSketchSpace();
  if (fileSize == 0 || fileSize > ceiling) {
    Serial.println("[OTA] ERROR: last_good.bin has invalid size");
    f.close();
    OtaState::markUpdateFailed(failingVersion, reason + "_backup_invalid_size");
    return false;
  }

  if (!Update.begin(ceiling, U_FLASH)) {
    Serial.print("[OTA] ERROR: Update.begin failed for rollback: ");
    Serial.println(Update.getErrorString());
    f.close();
    OtaState::markUpdateFailed(failingVersion, reason + "_update_begin_failed");
    return false;
  }

  uint8_t buf[kChunkSize];
  size_t total = 0;
  bool writeFailed = false;
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    size_t written = Update.write(buf, n);
    if (written != n) {
      writeFailed = true;
      break;
    }
    total += n;
    delay(0);
  }
  f.close();

  if (writeFailed || total != fileSize) {
    Serial.println("[OTA] ERROR: rollback write failed or incomplete");
    OtaState::markUpdateFailed(failingVersion, reason + "_rollback_write_failed");
    return false;
  }

  if (!Update.end(true)) {
    Serial.print("[OTA] ERROR: rollback finalize failed: ");
    Serial.println(Update.getErrorString());
    OtaState::markUpdateFailed(failingVersion, reason + "_rollback_end_failed");
    return false;
  }

  // Zaznamenat neuspesnou verzi AZ TED (po uspesnem naprogramovani zalohy) -
  // zabranuje tomu, aby ji pristi kontrola GitHub Releases zkusila znovu
  // (sekce 14 zadani - ochrana proti nekonecne OTA smycce).
  OtaState::markUpdateFailed(failingVersion, reason);

  Serial.println("[OTA] Rollback completed, rebooting into last known-good firmware...");
  delay(200);
  ESP.restart();
  return true; // nedosazitelne za normalnich okolnosti (ESP.restart() nevraci rizeni)
}

void checkAndUpdate() {
  Serial.println("\n[OTA] Checking for updates...");
  Serial.print("[OTA] Current version: ");
  Serial.println(FIRMWARE_VERSION);

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(OTA_CONNECT_TIMEOUT_MS);
  HTTPClient http;

  String tagName, firmwareUrl, metadataUrl, checksumUrl;
  size_t firmwareSizeFromRelease = 0;

  if (!fetchLatestRelease(client, http, tagName, firmwareUrl, firmwareSizeFromRelease, metadataUrl, checksumUrl)) {
    Serial.println("[OTA] GitHub unavailable");
    return;
  }

  Serial.print("[OTA] Latest version: ");
  Serial.println(tagName);
  Serial.print("[OTA] Target: ");
  Serial.println(FIRMWARE_TARGET);

  bool cmpOk = false;
  int cmp = compareOtaVersionStrings(tagName.c_str(), FIRMWARE_VERSION, &cmpOk);
  if (!cmpOk) {
    Serial.println("[OTA] ERROR: cannot parse release version tag, skipping");
    return;
  }
  if (cmp <= 0) {
    Serial.println("[OTA] No update available");
    return;
  }

  if (OtaState::isVersionMarkedFailed(tagName)) {
    Serial.println("[OTA] Firmware previously failed");
    return;
  }

  if (firmwareUrl.length() == 0) {
    Serial.println("[OTA] Firmware asset not found");
    return;
  }

  Metadata meta = fetchMetadata(client, http, metadataUrl);
  size_t declaredSize = firmwareSizeFromRelease;
  String expectedSha256 = "";

  if (meta.valid) {
    if (meta.target != FIRMWARE_TARGET) {
      Serial.print("[OTA] ERROR: Incompatible firmware (release target=");
      Serial.print(meta.target);
      Serial.println(")");
      Serial.println("[OTA] Update cancelled");
      return;
    }
    if (meta.firmwareSize > 0) declaredSize = meta.firmwareSize;
    if (meta.sha256.length() > 0) expectedSha256 = meta.sha256;
  } else {
    Serial.println("[OTA] WARNING: firmware.json not found, skipping target verification");
  }

  if (expectedSha256.length() == 0) {
    expectedSha256 = fetchChecksumFile(client, http, checksumUrl);
  }

  if (expectedSha256.length() == 0 && OTA_REQUIRE_CHECKSUM) {
    Serial.println("[OTA] ERROR: No checksum available (required)");
    Serial.println("[OTA] Update cancelled");
    return;
  }

  reportStatus("Aktualizace FW", "Zjistuji...");

  ApplyResult result = downloadAndApply(client, http, firmwareUrl, declaredSize, expectedSha256, tagName);

  if (result == ApplyResult::Success) {
    reportStatus("Aktualizace FW", "Restart...");
    Serial.println("[OTA] Rebooting...");
    delay(200);
    ESP.restart();
  } else {
    reportStatus("Aktualizace FW", "Chyba, pokracuji");
    delay(1000);
  }
}

} // namespace

void setStatusCallback(StatusCallback cb) {
  g_statusCb = cb;
}

void begin() {
  g_healthyNotified = false;
  if (!OTA_ENABLED) return;
  if (!OtaState::pendingValidation()) return;

  if (OtaState::pendingVersionValue() != String(FIRMWARE_VERSION)) {
    // Bud OTA nikdy skutecne neproblehlo (napr. vypadek napajeni mezi
    // zapisem pending stavu a proveden'im eboot swapu), nebo byl
    // FIRMWARE_VERSION zmenen rucne bez OTA. V obou pripadech neni co
    // validovat/rollbackovat.
    Serial.println("[OTA] Stale pending-validation state (firmware version mismatch), clearing");
    OtaState::clearStalePending();
    return;
  }

  uint8_t attempts = OtaState::recordBootAttempt();
  Serial.print("[OTA] Boot pending validation for version ");
  Serial.print(FIRMWARE_VERSION);
  Serial.print(", attempt ");
  Serial.print(attempts);
  Serial.print("/");
  Serial.println(OTA_MAX_BOOT_ATTEMPTS);

  if (attempts > OTA_MAX_BOOT_ATTEMPTS) {
    Serial.println("[OTA] Rollback detected");
    performRollback("boot_loop");
    // Pokud se performRollback vrati (misto restartu), zadna zaloha
    // neexistovala - pokracujeme v bootu se soucasnym firmware best-effort.
  }
}

void notifyApplicationHealthy() {
  if (g_healthyNotified) return;
  g_healthyNotified = true;

  if (OtaState::pendingValidation() && OtaState::pendingVersionValue() == String(FIRMWARE_VERSION)) {
    Serial.println("[OTA] Application healthy - marking current firmware as validated");
    OtaState::markBootValidated();
  }
}

void handle() {
  if (!OTA_ENABLED) return;

  unsigned long now = millis();
  if (g_firstCheckDone && (now - g_lastCheckMs) < OTA_CHECK_INTERVAL_MS) {
    return;
  }
  g_lastCheckMs = now;
  g_firstCheckDone = true;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] Skipping check: WiFi not connected");
    return;
  }

  checkAndUpdate();
}

} // namespace OtaManager
