#pragma once
// Prenosna, na Arduino.h nezavisla implementace SHA-256 (FIPS 180-4).
// Pouziva se pro overeni integrity stahovaneho OTA firmware.
// Streamovaci API umoznuje pocitat hash po castech behem stahovani,
// aniz by bylo nutne drzet cely firmware v RAM.

#include <stdint.h>
#include <stddef.h>

class Sha256 {
public:
  Sha256();

  void reset();
  void update(const uint8_t* data, size_t len);
  void finish(uint8_t digest[32]);

  // Prevede 32bajtovy digest na 64znakovy hex retezec (+ '\0').
  static void toHex(const uint8_t digest[32], char out[65]);

  // Case-insensitive porovnani hex retezce s digestem.
  static bool hexEquals(const uint8_t digest[32], const char* hex);

private:
  void processBlock(const uint8_t block[64]);

  uint32_t state_[8];
  uint8_t buffer_[64];
  size_t bufferLen_;
  uint64_t totalLen_;
};
