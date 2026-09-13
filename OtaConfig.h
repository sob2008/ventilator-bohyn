#pragma once
// ============================================================
// OTA KONFIGURACE - jedine misto pro nastaveni OTA systemu.
// ============================================================
// Prevzato ze sablony https://github.com/sob2008/esp-ota (template/OtaConfig.example.h).
// Zadne tajne udaje (GitHub token, hesla, klice) sem NEPATRI - repozitar
// musi byt verejny, system pouziva anonymni GitHub REST API.

// --- Verze a identita tohoto firmware ---

// Zvysujte pri kazde zmene, kterou chcete distribuovat pres OTA.
// Pouziva se Semantic Versioning (MAJOR.MINOR.PATCH), viz OtaVersion.h.
// Pri vydani nove verze pres scripts/release.ps1 se tato hodnota nastavi
// automaticky - rucni uprava je potreba jen pri prvnim zavedeni OTA.
#define FIRMWARE_VERSION "1.1.0"

// Identifikuje HW/SW variantu tohoto firmware. OTA odmitne nainstalovat
// release, jehoz firmware.json obsahuje jiny "target" (chrani pred
// nahranim firmware urceneho pro jiny hardware). MUSI byt identicke
// s FIRMWARE_TARGET ve factory-sw/OtaConfig.h.
#define FIRMWARE_TARGET "esp8266-d1mini-ventilator-pro"

// --- GitHub repozitar s Releases ---
#define GITHUB_OWNER "sob2008"
#define GITHUB_REPOSITORY "ventilator-bohyn"

// Nazvy ocekavanych assetu v GitHub Release.
// firmware.json je volitelny, ale doporuceny - pokud je pritomen, pouzije
// se pro kontrolu "target" a SHA-256 bez nutnosti samostatneho .sha256 souboru.
#define OTA_ASSET_FIRMWARE "firmware.bin"
#define OTA_ASSET_METADATA "firmware.json"
#define OTA_ASSET_CHECKSUM "firmware.bin.sha256"

// --- Chovani OTA ---

// Globalni vypinac - pri false OtaManager::handle() nic nedela.
#define OTA_ENABLED true

// Jak casto (ms) se kontroluje GitHub Releases na novou verzi.
// 30 minut je rozumny vychozi interval pro zarizeni bezici bez dohledu.
#define OTA_CHECK_INTERVAL_MS (30UL * 60UL * 1000UL) // 30 minut

// Timeouty sitovych operaci (ms).
#define OTA_CONNECT_TIMEOUT_MS 10000UL
#define OTA_DOWNLOAD_TIMEOUT_MS 120000UL // cely stahovaci cyklus firmware.bin

// Vyzadovat platny SHA-256 checksum, jinak OTA zrusit (fail-closed).
// HTTPS spojeni pro OTA pouziva BearSSL::WiFiClientSecure::setInsecure()
// (zadne cert pinning), takze tento checksum je skutecnou, ne jen
// volitelnou pojistkou integrity stazeneho firmware.
#define OTA_REQUIRE_CHECKSUM true

// Kolik po sobe jdoucich neuspesnych bootu noveho (jeste nevalidovaneho)
// firmware je povoleno, nez OtaManager provede automaticky rollback na
// /ota/last_good.bin.
#define OTA_MAX_BOOT_ATTEMPTS 3
