#pragma once
// Persistentni stav OTA systemu, ulozeny v LittleFS tak, aby prezil restart
// i vypadek napajeni. Drzi:
//   - /ota/state.json    maly JSON se stavem (viz nize)
//   - /ota/candidate.bin  prave stahovany/testovany firmware (pred validaci)
//   - /ota/last_good.bin  posledni overene funkcni firmware (zaloha pro rollback)
//
// Zapis state.json probiha pres docasny soubor + rename, aby vypadek napajeni
// uprostred zapisu nezanechal poskozeny/napulko zapsany stav.

#include <Arduino.h>

namespace OtaState {

// Adresar a soubory pouzivane OTA systemem.
extern const char* kDir;
extern const char* kStatePath;
extern const char* kStateTmpPath;
extern const char* kCandidatePath;
extern const char* kLastGoodPath;

// Pripoji LittleFS (naformatuje, pokud je prazdny/nenaformatovany) a nacte
// stav z /ota/state.json. Pokud soubor neexistuje nebo je poskozeny, vytvori
// vychozi (cisty) stav. Musi byt zavolano jednou v setup() pred pouzitim
// ostatnich funkci.
bool begin();

// --- Dotazy na aktualni stav ---
bool pendingValidation();
uint8_t bootAttempts();
String pendingVersionValue();
String lastFailedVersion();
bool hasLastGoodBackup();

// Vrati true, pokud "version" presne odpovida verzi, ktera jiz drive
// selhala pri validaci po OTA (ochrana proti nekonecne OTA smycce).
bool isVersionMarkedFailed(const String& version);

// --- Prechody stavu ---

// Zavolat co nejdrive v setup(), jeste pred prvnim pokusem o pripojeni site.
// Pokud je pendingValidation() true (tj. prave jsme nabootovali firmware
// z predchoziho OTA, ktery se jeste neprokazal jako funkcni), zvysi a
// persistentne ulozi pocitadlo pokusu o boot. Vraci aktualni pocet pokusu
// (vcetne tohoto).
uint8_t recordBootAttempt();

// Zavolat tesne pred ESP.restart() po uspesnem zapisu noveho firmware do
// OTA oblasti (Update.end() == true). Oznaci novy firmware jako "cekajici
// na validaci" a ulozi jeho verzi.
void beginPendingValidation(const String& newVersion);

// Zavolat po uspesnem overeni, ze nove nabootovany firmware funguje
// (WiFi + alespon jeden uspesny cyklus hlavni funkce, nebo grace timeout).
// Vycisti pending stav, vynuluje pocitadlo pokusu a povysi /ota/candidate.bin
// na /ota/last_good.bin (ten se stava novou zalohou pro pripadny budouci rollback).
void markBootValidated();

// Vycisti "pending" priznaky (pending_validation, boot_attempts, pending_version)
// BEZ povyseni candidate.bin na last_good.bin. Pouziva se, kdyz se zjisti, ze
// pending_validation byl "stale" - tj. verze bezici firmware (FIRMWARE_VERSION)
// neodpovida verzi, na kterou se OTA cekalo (viz OtaManager::begin) - typicky
// dusledek vypadku napajeni mezi zapisem tohoto stavu a skutecnym proveden'im
// eboot swapu. V takovem pripade nedoslo k realne aplikaci noveho firmware,
// takze neni co validovat ani promovat.
void clearStalePending();

// Zavolat, kdyz OTA/aktualizace selze - bud pred restartem (napr. spatny
// checksum, nekompatibilni target) nebo po opakovanych neuspesnych bootech
// (boot-loop detekce). Zaznamena verzi jako "failed" (aby se nezkousela
// znovu donekonecna) a vycisti pending/bootAttempts.
void markUpdateFailed(const String& version, const String& reason);

} // namespace OtaState
