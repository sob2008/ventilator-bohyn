Sem patří zkompilovaný `<sketch>.ino.bin` (z `arduino-cli compile --export-binaries`
nebo Arduino IDE `Sketch -> Export compiled Binary`) - `flash.py`/`flash.sh` v rodičovské
složce ho odsud vezmou a nahrají do zařízení.

`factory-sw.ino.bin` je v tomto repozitáři záměrně zacommitovaný (na rozdíl od
obecného doporučení šablony esp-ota) - tovární firmware se nemění často, takže
se vyplatí mít předkompilovaný .bin rovnou k dispozici pro flashnutí bez nutnosti
mít nainstalované Arduino IDE/arduino-cli. Pokud upravíte `factory-sw/factory-sw.ino`
nebo cokoliv v `factory-sw/Ota*`, `.bin` znovu zkompilujte
(`arduino-cli compile --fqbn esp8266:esp8266:d1_mini --export-binaries factory-sw`)
a nahraďte tento soubor, jinak bude flashovat zastaralou verzi.
