#!/bin/bash
#
# ESP Universal Flash Utility
# Podpora: ESP32 / ESP8266
#

set -e

VERSION="1.1"

# ==============================
# Nastavení
# ==============================

BIN_DIR="./bin"
BAUD=921600
MONITOR_BAUD=115200

# ==============================
# Barvy
# ==============================

RED="\033[31m"
GREEN="\033[32m"
YELLOW="\033[33m"
NC="\033[0m"

ok()
{
    echo -e "${GREEN}✔ $1${NC}"
}

err()
{
    echo -e "${RED}✘ $1${NC}"
}

info()
{
    echo -e "${YELLOW}➜ $1${NC}"
}


# ==============================
# Nápověda
# ==============================

help()
{
echo "
ESP Universal Flash Utility $VERSION

Použití:

./flash.sh
    Nahraje firmware

./flash.sh --info
    Zobrazí informace o čipu

./flash.sh --erase
    Smaže flash

./flash.sh --monitor
    Otevře sériový monitor

./flash.sh --help
    Tato nápověda

Poznámka:
    Skript při každém spuštění sám zkontroluje, zda jsou nainstalované
    python3, pip, esptool, přístupová práva k sériovému portu
    (skupina dialout/uucp) a u --monitor i picocom/screen.
    Chybějící závislosti nabídne doinstalovat (vyžaduje sudo).
"
exit 0
}


# ==============================
# Detekce balíčkovacího systému
# ==============================

PKG_MANAGER=""

detect_pkg_manager()
{

if command -v apt-get >/dev/null 2>&1
then
    PKG_MANAGER="apt"
elif command -v dnf >/dev/null 2>&1
then
    PKG_MANAGER="dnf"
elif command -v yum >/dev/null 2>&1
then
    PKG_MANAGER="yum"
elif command -v pacman >/dev/null 2>&1
then
    PKG_MANAGER="pacman"
elif command -v zypper >/dev/null 2>&1
then
    PKG_MANAGER="zypper"
else
    PKG_MANAGER=""
fi

}


# Nainstaluje balíček(y) přes zjištěný balíčkovací systém.
# Na Debianu/Ubuntu/Fedoře atd. může vyžadovat sudo heslo.
pkg_install()
{

local pkgs="$*"

case "$PKG_MANAGER" in

apt)
    sudo apt-get update -y
    sudo apt-get install -y $pkgs
;;

dnf)
    sudo dnf install -y $pkgs
;;

yum)
    sudo yum install -y $pkgs
;;

pacman)
    sudo pacman -Sy --noconfirm $pkgs
;;

zypper)
    sudo zypper install -y $pkgs
;;

*)
    err "Nepodařilo se zjistit balíčkovací systém, nainstalujte prosím ručně: $pkgs"
    return 1
;;

esac

}


# ==============================
# Kontrola základních závislostí (Python, pip)
# ==============================

check_python()
{

if command -v python3 >/dev/null 2>&1
then
    ok "python3 je nainstalovaný"
else

    err "python3 není nainstalovaný"
    read -p "Nainstalovat python3? [y/N] " answer

    if [[ "$answer" =~ ^[Yy]$ ]]
    then
        case "$PKG_MANAGER" in
        apt)    pkg_install python3 python3-venv ;;
        pacman) pkg_install python ;;
        *)      pkg_install python3 ;;
        esac
    else
        err "Bez python3 nelze pokračovat"
        exit 1
    fi

fi

}


check_pip()
{

if python3 -m pip --version >/dev/null 2>&1
then
    ok "pip je nainstalovaný"
else

    err "pip pro python3 není nainstalovaný"
    read -p "Nainstalovat python3-pip? [y/N] " answer

    if [[ "$answer" =~ ^[Yy]$ ]]
    then
        case "$PKG_MANAGER" in
        pacman) pkg_install python-pip ;;
        *)      pkg_install python3-pip ;;
        esac
    else
        err "Bez pip nelze doinstalovat esptool"
        exit 1
    fi

fi

}


# ==============================
# Kontrola přístupu k sériovému portu (skupina dialout/uucp)
# ==============================

check_serial_permissions()
{

# Relevantní pouze na Linuxu (na macOS neexistuje skupina dialout)
if [ "$(uname)" != "Linux" ]
then
    return 0
fi

# Zjištění správné skupiny podle distribuce (Debian/Ubuntu = dialout, Arch = uucp)
local group=""

if getent group dialout >/dev/null 2>&1
then
    group="dialout"
elif getent group uucp >/dev/null 2>&1
then
    group="uucp"
fi

if [ -z "$group" ]
then
    return 0
fi

if id -nG "$USER" | grep -qw "$group"
then
    ok "Uživatel $USER má přístup k sériovým portům (skupina $group)"
else

    err "Uživatel $USER není ve skupině $group (přístup k /dev/ttyUSB*, /dev/ttyACM* může selhat)"

    read -p "Přidat uživatele $USER do skupiny $group? [y/N] " answer

    if [[ "$answer" =~ ^[Yy]$ ]]
    then
        sudo usermod -aG "$group" "$USER"
        info "Hotovo. Pro projevení změny se je potřeba odhlásit a znovu přihlásit (nebo restartovat)."
    fi

fi

}


# ==============================
# Kontrola esptool
# ==============================

check_esptool()
{

if command -v esptool >/dev/null 2>&1
then
    ESPTOOL="esptool"
elif command -v esptool.py >/dev/null 2>&1
then
    ESPTOOL="esptool.py"
elif python3 -m esptool version >/dev/null 2>&1
then
    ESPTOOL="python3 -m esptool"
else

    err "esptool není nainstalovaný"

    read -p "Nainstalovat esptool? [y/N] " answer

    if [[ "$answer" =~ ^[Yy]$ ]]
    then
        python3 -m pip install --user --break-system-packages esptool 2>/dev/null \
            || python3 -m pip install --user esptool

        export PATH="$HOME/.local/bin:$PATH"

        if command -v esptool >/dev/null 2>&1
        then
            ESPTOOL="esptool"
        elif command -v esptool.py >/dev/null 2>&1
        then
            ESPTOOL="esptool.py"
        else
            ESPTOOL="python3 -m esptool"
        fi
    else
        exit 1
    fi

fi

ok "Používám $ESPTOOL"

}


# ==============================
# Kontrola sériového monitoru (picocom / screen)
# ==============================

check_monitor_tool()
{

if command -v picocom >/dev/null 2>&1 || command -v screen >/dev/null 2>&1
then
    ok "Nástroj pro sériový monitor je k dispozici"
else

    err "Není nainstalovaný picocom ani screen"

    read -p "Nainstalovat picocom? [y/N] " answer

    if [[ "$answer" =~ ^[Yy]$ ]]
    then
        pkg_install picocom
    else
        info "Bez picocom/screen nebude fungovat --monitor"
    fi

fi

}


# ==============================
# Hledání ESP
# ==============================

find_device()
{

info "Hledám ESP zařízení..."

DEVICES=()

# Rozšířeno o podporu macOS (*usbserial* / *wchusb*)
for p in /dev/ttyUSB* /dev/ttyACM* /dev/cu.usbserial* /dev/cu.wchusbserial*
do
    [ -e "$p" ] || continue

    DEVICES+=("$p")
done


if [ ${#DEVICES[@]} -eq 0 ]
then
    err "Žádné USB zařízení nenalezeno"
    exit 1
fi


if [ ${#DEVICES[@]} -eq 1 ]
then
    PORT=${DEVICES[0]}
else

    echo
    echo "Nalezeno více zařízení. Vyberte správné:"
    
    # Ošetření select smyčky proti neplatnému vstupu
    select d in "${DEVICES[@]}"
    do
        if [ -n "$d" ]; then
            PORT=$d
            break
        else
            err "Neplatná volba, zkuste to znovu."
        fi
    done

fi


ok "Port: $PORT"

}


# ==============================
# Informace o čipu
# ==============================

chip_info()
{

echo
# Použit read_mac, který spolehlivě funguje na ESP8266 i ESP32 v novém esptool
$ESPTOOL --port "$PORT" read_mac || true

echo
$ESPTOOL --port "$PORT" flash_id || true

}


# ==============================
# Najdi firmware
# ==============================

find_firmware()
{

# Bezpečné hledání souborů pomocí nullglob
shopt -s nullglob
FILES=("${BIN_DIR}"/*.ino.bin)
shopt -u nullglob

if [ ${#FILES[@]} -eq 0 ]
then
    err "Nenalezen žádný firmware v ${BIN_DIR}/"
    exit 1
fi


if [ ${#FILES[@]} -eq 1 ]
then
    APP=${FILES[0]}
else
    echo
    echo "Nalezeno více .bin souborů v ${BIN_DIR}/. Vyberte, který nahrát:"

    select f in "${FILES[@]}"
    do
        if [ -n "$f" ]; then
            APP=$f
            break
        else
            err "Neplatná volba, zkuste to znovu."
        fi
    done
fi

NAME=$(basename "$APP" .ino.bin)

BOOT="$BIN_DIR/$NAME.ino.bootloader.bin"
PART="$BIN_DIR/$NAME.ino.partitions.bin"


ok "Projekt: $NAME"

}


# ==============================
# Detekce typu
# ==============================

detect_chip()
{

# Zde přidáno || true, aby set -e nezhodil skript při chybě komunikace
# Používáme read_mac, protože vypíše typ čipu ("Chip is ESP32...") spolehlivěji
DATA=$($ESPTOOL --port "$PORT" read_mac 2>&1 || true)

if echo "$DATA" | grep -qi "ESP8266"
then
    CHIP="ESP8266"
elif echo "$DATA" | grep -qi "ESP32"
then
    CHIP="ESP32"
else
    # Failback, pokud se nepodaří čip detekovat (zkusí se ESP32 jako výchozí)
    info "Typ čipu nelze detekovat, zkouším ESP32..."
    CHIP="ESP32"
fi

ok "Detekovaný čip: $CHIP"

}


# ==============================
# Flash
# ==============================

flash()
{

info "Mazání flash..."

$ESPTOOL \
--chip "${CHIP,,}" \
--port "$PORT" \
erase_flash


if [ "$CHIP" = "ESP32" ]
then

    info "Nahrávám ESP32 firmware..."

    $ESPTOOL \
    --chip esp32 \
    --port "$PORT" \
    --baud "$BAUD" \
    write_flash -z \
    0x1000 "$BOOT" \
    0x8000 "$PART" \
    0x10000 "$APP"

else

    info "Nahrávám ESP8266 firmware..."

    $ESPTOOL \
    --chip esp8266 \
    --port "$PORT" \
    --baud "$BAUD" \
    write_flash -z \
    0x00000 "$APP"

fi


ok "Firmware nahrán"

}


# ==============================
# Monitor
# ==============================

monitor()
{

if command -v picocom >/dev/null
then
    picocom "$PORT" -b $MONITOR_BAUD
elif command -v screen >/dev/null
then
    screen "$PORT" $MONITOR_BAUD
else
    err "Není nainstalovaný picocom ani screen"
fi

}


# ==============================
# Program
# ==============================

detect_pkg_manager
check_python
check_pip
check_serial_permissions
check_esptool

find_device

case "$1" in

--help)
    help
;;

--info)
    chip_info
;;

--erase)

    $ESPTOOL \
    --port "$PORT" \
    erase_flash

    ok "Flash vymazána"

;;

--monitor)

    check_monitor_tool
    monitor

;;

*)

    find_firmware
    detect_chip

    echo
    read -p "Nahrát firmware? [Y/n] " answer

    if [[ "$answer" =~ ^[Nn]$ ]]
    then
        exit 0
    fi

    flash

;;

esac


echo
echo "=============================="
ok "HOTOVO"
echo "=============================="