#pragma once
// Jadro OTA systemu. Vse potrebne pro spravne pouziti:
//
//   setup():
//     OtaState::begin();       // pripojit LittleFS + nacist stav (viz OtaState.h)
//     OtaManager::begin();     // rozhodnout o pripadnem rollbacku z minuleho OTA
//     ... pripojit WiFi, provest prvni cyklus hlavni funkce zarizeni ...
//     OtaManager::notifyApplicationHealthy(); // potvrdit, ze firmware funguje
//
//   loop():
//     OtaManager::handle();    // neblokujici mimo aktivni OTA cyklus; kontroluje
//                               // GitHub Releases jednou za OTA_CHECK_INTERVAL_MS
//
// OtaManager pri aktivnim stahovani/flashovani BLOKUJE hlavni smycku - to je
// vedomy kompromis (jednoduchost > asynchronost), viz README.md v tomto
// balicku. Pokud vase zarizeni behem OTA nesmi na nic cekat (napr. rizeni
// v realnem case), zvazte spusteni OtaManager::handle() v samostatnem tasku/
// jinem vlaknu behu (na ESP32 s FreeRTOS) - tento balicek to primo neresi.

#include <Arduino.h>

namespace OtaManager {

// Volitelny callback pro zobrazeni stavu OTA na displeji behem stahovani/
// flashovani, aby uzivatel nevidel "zamrzly" displej. Volano prilezitostne
// (ne kazdy chunk) z prubehu OTA cyklu.
typedef void (*StatusCallback)(const String& line1, const String& line2);
void setStatusCallback(StatusCallback cb);

// Zavolat jednou v setup(), po OtaState::begin() a PRED pripojenim WiFi.
// Pokud predchozi OTA cekala na validaci, zvysi pocitadlo pokusu o boot a
// pri prekroceni OTA_MAX_BOOT_ATTEMPTS provede rollback (reflash
// /ota/last_good.bin + restart). Pokud rollback neni mozny (zadna zaloha),
// funkce se normalne vrati a firmware pokracuje (best-effort).
void begin();

// Zavolat jednou v setup(), jakmile hlavni aplikace prokaze, ze bezi
// (typicky: WiFi pripojeno + provedl se alespon jeden pokus o hlavni
// funkci zarizeni). Bezpecne volat i kdyz zadne OTA neprobehlo (no-op).
void notifyApplicationHealthy();

// Zavolat v kazdem loop(). Neblokujici mimo aktivni kontrolu/update cyklus.
void handle();

} // namespace OtaManager
