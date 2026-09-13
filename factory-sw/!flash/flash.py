#!/usr/bin/env python3
#
# ESP Universal Flash Utility (Windows verze)
# Podpora: ESP32 / ESP8266
#

import os
import re
import sys
import glob
import subprocess

VERSION = "1.1"

# ==============================
# Nastavení
# ==============================

BIN_DIR = "./bin"
BAUD = "921600"
MONITOR_BAUD = "115200"

# ==============================
# Barvy (ANSI - povolíme na Windows 10+)
# ==============================

os.system("")  # trik pro povolení ANSI escape sekvencí v cmd.exe

RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
NC = "\033[0m"


def ok(msg):
    print(f"{GREEN}✔ {msg}{NC}")


def err(msg):
    print(f"{RED}✘ {msg}{NC}")


def info(msg):
    print(f"{YELLOW}➜ {msg}{NC}")


def ask(prompt):
    try:
        return input(prompt)
    except (EOFError, KeyboardInterrupt):
        print()
        sys.exit(1)


def yes(answer):
    return bool(re.match(r"^[Yy]$", answer or ""))


def no(answer):
    return bool(re.match(r"^[Nn]$", answer or ""))


# ==============================
# Nápověda
# ==============================

def show_help():
    print(f"""
ESP Universal Flash Utility {VERSION} (Windows)

Použití:

flash.py
    Nahraje firmware

flash.py --info
    Zobrazí informace o čipu

flash.py --erase
    Smaže flash

flash.py --monitor
    Otevře sériový monitor

flash.py --help
    Tato nápověda

Poznámka:
    Skript při každém spuštění sám zkontroluje, zda jsou nainstalované
    pip, esptool a pyserial (pro sériový monitor). Chybějící závislosti
    nabídne doinstalovat přes pip.
""")
    sys.exit(0)


# ==============================
# Kontrola pip
# ==============================

def check_pip():
    result = subprocess.run(
        [sys.executable, "-m", "pip", "--version"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if result.returncode == 0:
        ok("pip je nainstalovaný")
    else:
        err("pip není nainstalovaný")
        err("Nainstalujte prosím pip ručně (např. přeinstalací Pythonu z python.org "
            "s zaškrtnutou volbou 'pip') a spusťte skript znovu.")
        sys.exit(1)


# ==============================
# Kontrola esptool
# ==============================

ESPTOOL = None


def check_esptool():
    global ESPTOOL

    result = subprocess.run(
        [sys.executable, "-m", "esptool", "version"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    if result.returncode == 0:
        ESPTOOL = [sys.executable, "-m", "esptool"]
    else:
        err("esptool není nainstalovaný")
        answer = ask("Nainstalovat esptool? [y/N] ")

        if yes(answer):
            install = subprocess.run(
                [sys.executable, "-m", "pip", "install", "--user", "esptool"]
            )
            if install.returncode != 0:
                err("Instalace esptool selhala")
                sys.exit(1)

            result = subprocess.run(
                [sys.executable, "-m", "esptool", "version"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            if result.returncode != 0:
                err("esptool se nepodařilo spustit ani po instalaci")
                sys.exit(1)

            ESPTOOL = [sys.executable, "-m", "esptool"]
        else:
            sys.exit(1)

    ok(f"Používám {' '.join(ESPTOOL)}")


# ==============================
# Kontrola pyserial (potřeba pro --monitor a hledání portů)
# ==============================

def check_pyserial():
    try:
        import serial  # noqa: F401
        import serial.tools.list_ports  # noqa: F401
        ok("pyserial je nainstalovaný")
        return True
    except ImportError:
        err("pyserial není nainstalovaný")
        answer = ask("Nainstalovat pyserial? [y/N] ")

        if yes(answer):
            install = subprocess.run(
                [sys.executable, "-m", "pip", "install", "--user", "pyserial"]
            )
            if install.returncode == 0:
                return True
            else:
                err("Instalace pyserial selhala")
                return False
        else:
            info("Bez pyserial nebude fungovat --monitor a automatické hledání portů")
            return False


# ==============================
# Hledání ESP (COM porty)
# ==============================

PORT = None


def find_device():
    global PORT

    info("Hledám ESP zařízení...")

    devices = []

    try:
        import serial.tools.list_ports as list_ports
        for p in list_ports.comports():
            devices.append(p.device)
    except ImportError:
        err("pyserial chybí, nelze automaticky vyhledat COM porty")
        err("Zadejte port ručně, např. COM3")
        manual = ask("Port: ").strip()
        if not manual:
            err("Žádný port nezadán")
            sys.exit(1)
        PORT = manual
        ok(f"Port: {PORT}")
        return

    if len(devices) == 0:
        err("Žádné USB zařízení nenalezeno")
        sys.exit(1)

    if len(devices) == 1:
        PORT = devices[0]
    else:
        print()
        print("Nalezeno více zařízení. Vyberte správné:")
        for idx, d in enumerate(devices, start=1):
            print(f"{idx}) {d}")

        while True:
            choice = ask("#? ").strip()
            if choice.isdigit() and 1 <= int(choice) <= len(devices):
                PORT = devices[int(choice) - 1]
                break
            else:
                err("Neplatná volba, zkuste to znovu.")

    ok(f"Port: {PORT}")


# ==============================
# Informace o čipu
# ==============================

def chip_info():
    print()
    subprocess.run(ESPTOOL + ["--port", PORT, "read-mac"])
    print()
    subprocess.run(ESPTOOL + ["--port", PORT, "flash_id"])


# ==============================
# Najdi firmware
# ==============================

APP = BOOT = PART = NAME = None


def find_firmware():
    global APP, BOOT, PART, NAME

    files = sorted(glob.glob(os.path.join(BIN_DIR, "*.ino.bin")))

    if len(files) == 0:
        err(f"Nenalezen žádný firmware v {BIN_DIR}/")
        sys.exit(1)

    if len(files) == 1:
        APP = files[0]
    else:
        print()
        print(f"Nalezeno více .bin souborů v {BIN_DIR}/. Vyberte, který nahrát:")
        for idx, f in enumerate(files, start=1):
            print(f"{idx}) {os.path.basename(f)}")

        while True:
            choice = ask("#? ").strip()
            if choice.isdigit() and 1 <= int(choice) <= len(files):
                APP = files[int(choice) - 1]
                break
            else:
                err("Neplatná volba, zkuste to znovu.")

    NAME = os.path.basename(APP)[: -len(".ino.bin")]

    BOOT = os.path.join(BIN_DIR, f"{NAME}.ino.bootloader.bin")
    PART = os.path.join(BIN_DIR, f"{NAME}.ino.partitions.bin")

    ok(f"Projekt: {NAME}")


# ==============================
# Detekce typu čipu
# ==============================

CHIP = None


def detect_chip():
    global CHIP

    result = subprocess.run(
        ESPTOOL + ["--port", PORT, "read-mac"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    data = result.stdout or ""

    if re.search("ESP8266", data, re.IGNORECASE):
        CHIP = "ESP8266"
    elif re.search("ESP32", data, re.IGNORECASE):
        CHIP = "ESP32"
    else:
        err("Typ čipu se nepodařilo automaticky detekovat.")
        print("--- Výstup esptool ---")
        print(data.strip())
        print("----------------------")

        if re.search("busy|PermissionError|Access is denied|Přístup byl odepřen", data, re.IGNORECASE):
            info(f"Port {PORT} je obsazený jiným programem. Zavřete Arduino IDE / "
                 "Serial Monitor / PuTTY / jiný běžící skript, který port používá, "
                 "případně zkuste zařízení odpojit a znovu připojit, nebo spusťte "
                 "terminál jako administrátor.")
        elif re.search("could not open|no such file|does not exist", data, re.IGNORECASE):
            info(f"Port {PORT} nejde otevřít. Zkontrolujte ve Správci zařízení, "
                 "že zařízení je skutečně na tomto portu a ovladač (CH340/CP210x) "
                 "je nainstalovaný.")
        else:
            info("Tip: u desek bez auto-reset obvodu podrž tlačítko FLASH/GPIO0 "
                 "při připojení/resetu, dokud se nespustí komunikace.")

        while True:
            choice = ask("Zadejte typ čipu ručně [1] ESP32  [2] ESP8266: ").strip()
            if choice == "1":
                CHIP = "ESP32"
                break
            elif choice == "2":
                CHIP = "ESP8266"
                break
            else:
                err("Neplatná volba, zadejte 1 nebo 2.")

    ok(f"Detekovaný čip: {CHIP}")


# ==============================
# Flash
# ==============================

def flash():
    info("Mazání flash...")

    subprocess.run(
        ESPTOOL + ["--chip", CHIP.lower(), "--port", PORT, "erase_flash"],
        check=True,
    )

    if CHIP == "ESP32":
        info("Nahrávám ESP32 firmware...")
        subprocess.run(
            ESPTOOL + [
                "--chip", "esp32",
                "--port", PORT,
                "--baud", BAUD,
                "write_flash", "-z",
                "0x1000", BOOT,
                "0x8000", PART,
                "0x10000", APP,
            ],
            check=True,
        )
    else:
        info("Nahrávám ESP8266 firmware...")
        subprocess.run(
            ESPTOOL + [
                "--chip", "esp8266",
                "--port", PORT,
                "--baud", BAUD,
                "write_flash", "-z",
                "0x00000", APP,
            ],
            check=True,
        )

    ok("Firmware nahrán")


# ==============================
# Monitor
# ==============================

def monitor():
    try:
        import serial.tools.miniterm  # noqa: F401
    except ImportError:
        err("pyserial (miniterm) není k dispozici, nelze spustit monitor")
        return

    subprocess.run(
        [sys.executable, "-m", "serial.tools.miniterm", PORT, MONITOR_BAUD]
    )


# ==============================
# Program
# ==============================

def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else ""

    if arg == "--help":
        show_help()

    check_pip()
    check_esptool()

    have_pyserial = check_pyserial()
    if not have_pyserial and arg == "--monitor":
        err("Bez pyserial nelze spustit --monitor")
        sys.exit(1)

    find_device()

    if arg == "--info":
        chip_info()

    elif arg == "--erase":
        subprocess.run(ESPTOOL + ["--port", PORT, "erase_flash"], check=True)
        ok("Flash vymazána")

    elif arg == "--monitor":
        monitor()

    else:
        find_firmware()
        detect_chip()

        print()
        answer = ask("Nahrát firmware? [Y/n] ")

        if no(answer):
            sys.exit(0)

        flash()

    print()
    print("==============================")
    ok("HOTOVO")
    print("==============================")


if __name__ == "__main__":
    main()
