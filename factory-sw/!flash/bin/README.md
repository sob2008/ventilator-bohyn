Sem patří zkompilovaný `<sketch>.ino.bin` (z `arduino-cli compile --export-binaries`
nebo Arduino IDE `Sketch -> Export compiled Binary`) - `flash.py`/`flash.sh` v rodičovské
složce ho odsud vezmou a nahrají do zařízení.

Necommitujte sem výsledné `.bin` soubory - vždy se buildují čerstvě před flashnutím.
