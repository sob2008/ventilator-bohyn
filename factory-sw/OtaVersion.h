#pragma once
// Prenosne (na Arduino.h nezavisle) parsovani a numericke porovnavani
// semantickych verzi ve tvaru "MAJOR.MINOR.PATCH", s volitelnym uvodnim 'v'/'V'
// (napr. GitHub Release tag "v1.10.0"). Porovnava se cislo po cisle, NIKOLI
// lexikograficky - "1.10.0" je vetsi nez "1.9.0".
//
// Volitelna pripona za patch verzi (napr. "-beta", "-rc1") se pro ucely
// porovnani ignoruje (odrizne se na prvnim znaku, ktery neni cislice ani tecka).

#include <stdint.h>

struct OtaVersion {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t patch = 0;
};

// Naparsuje retezec do OtaVersion. Prijima "1.2.3", "v1.2.3", "V1.2.3",
// i zkracene tvary "1.2" (-> patch=0) a "1" (-> minor=0, patch=0).
// Vraci false, pokud retezec neobsahuje na zacatku (po pripadnem 'v') zadne cislo.
bool parseOtaVersion(const char* str, OtaVersion& out);

// Vraci <0 pokud a<b, 0 pokud a==b, >0 pokud a>b. Cisty numericky vektorovy
// porovnavac (major, pak minor, pak patch).
int compareOtaVersion(const OtaVersion& a, const OtaVersion& b);

// Pohodlna varianta primo nad retezci. Pokud se nektery z retezcu neda
// naparsovat, nastavi *ok = false a vraci 0 (volajici musi ok zkontrolovat
// pred pouzitim vysledku porovnani).
int compareOtaVersionStrings(const char* a, const char* b, bool* ok);
