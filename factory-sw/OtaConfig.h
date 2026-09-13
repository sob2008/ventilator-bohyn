#pragma once
// ============================================================
// OTA KONFIGURACE PRO TOVARNI (PROVISIONING) FIRMWARE.
// ============================================================
// Prevzato ze sablony https://github.com/sob2008/esp-ota
// (factory-template/OtaConfig.example.h).
//
// DULEZITE: FIRMWARE_TARGET, GITHUB_OWNER, GITHUB_REPOSITORY a OTA_ASSET_*
// jsou IDENTICKE s ../OtaConfig.h ostreho firmware - jinak by factory-sw
// nenasel/neoveril spravny Release. FIRMWARE_VERSION a OTA_CHECK_INTERVAL_MS
// jsou zamerne jine, viz komentare nize.

// --- Identita "verze" tovarniho firmware ---

// Umyslne "0.0.0" - musi byt vzdy nizsi nez jakakoliv realne vydana verze,
// aby factory-sw pri prvnim pripojeni k WiFi okamzite nasel a nainstaloval
// nejnovejsi dostupny Release.
#define FIRMWARE_VERSION "0.0.0"

// Musi presne odpovidat FIRMWARE_TARGET ostreho firmware (../OtaConfig.h).
#define FIRMWARE_TARGET "esp8266-d1mini-ventilator-pro"

// --- GitHub repozitar s Releases (musi odpovidat ostremu firmware) ---
#define GITHUB_OWNER "sob2008"
#define GITHUB_REPOSITORY "ventilator-bohyn"

// Nazvy ocekavanych assetu v GitHub Release - stejne jako v ostrem firmware.
#define OTA_ASSET_FIRMWARE "firmware.bin"
#define OTA_ASSET_METADATA "firmware.json"
#define OTA_ASSET_CHECKSUM "firmware.bin.sha256"

// --- Chovani OTA ---

#define OTA_ENABLED true

// U tovarniho firmware umyslne KRATSI interval nez v ostrem firmware
// (tam 30 min) - technik/zakaznik ceka u zarizeni, chceme rychle
// opakovani pri docasnem vypadku WiFi/GitHubu, ne cekat 30 minut.
#define OTA_CHECK_INTERVAL_MS (20UL * 1000UL) // 20 sekund

#define OTA_CONNECT_TIMEOUT_MS 10000UL
#define OTA_DOWNLOAD_TIMEOUT_MS 120000UL

// Stejna bezpecnostni ocekavani jako ostry firmware.
#define OTA_REQUIRE_CHECKSUM true

// Tovarni firmware se sam nikdy nebootuje opakovane jako "pending
// validation" (po uspesnem OTA rovnou restartuje do ostreho firmware),
// takze tato hodnota zde nema prakticky vyznam - ponechana pro konzistenci.
#define OTA_MAX_BOOT_ATTEMPTS 3
