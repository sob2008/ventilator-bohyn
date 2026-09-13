#include "OtaVersion.h"
#include <ctype.h>

namespace {

// Precte nezaporne cislo od *p, posune *p za posledni cislici.
// Vraci false, pokud na pozici *p neni zadna cislice.
bool readNumber(const char** p, uint32_t* out) {
  const char* s = *p;
  if (!isdigit(static_cast<unsigned char>(*s))) {
    return false;
  }
  uint32_t value = 0;
  while (isdigit(static_cast<unsigned char>(*s))) {
    value = value * 10 + uint32_t(*s - '0');
    s++;
  }
  *out = value;
  *p = s;
  return true;
}

} // namespace

bool parseOtaVersion(const char* str, OtaVersion& out) {
  if (str == nullptr) return false;

  const char* p = str;
  if (*p == 'v' || *p == 'V') p++;

  OtaVersion result;

  if (!readNumber(&p, &result.major)) {
    return false;
  }

  if (*p == '.') {
    p++;
    if (!readNumber(&p, &result.minor)) {
      return false;
    }
  }

  if (*p == '.') {
    p++;
    if (!readNumber(&p, &result.patch)) {
      return false;
    }
  }

  // Cokoliv za tim (napr. "-beta", "+build5") se pro porovnani ignoruje.
  out = result;
  return true;
}

int compareOtaVersion(const OtaVersion& a, const OtaVersion& b) {
  if (a.major != b.major) return (a.major < b.major) ? -1 : 1;
  if (a.minor != b.minor) return (a.minor < b.minor) ? -1 : 1;
  if (a.patch != b.patch) return (a.patch < b.patch) ? -1 : 1;
  return 0;
}

int compareOtaVersionStrings(const char* a, const char* b, bool* ok) {
  OtaVersion va, vb;
  bool okA = parseOtaVersion(a, va);
  bool okB = parseOtaVersion(b, vb);
  if (ok != nullptr) {
    *ok = okA && okB;
  }
  if (!okA || !okB) {
    return 0;
  }
  return compareOtaVersion(va, vb);
}
