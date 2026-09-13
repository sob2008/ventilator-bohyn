//rele D1 spojit s D1 esp, gnd na gnd, 5v na 5v
//senzor + na 5v, gnd na gnd, data na D4 (NUTNY 4.7k pull-up rezistor mezi data a 5v!)
//prepinac pro rucni rezim jeden pin na gnd druhy na D2
//prepinac pro ventilator v rucnim rezimu jeden pin na gnd druhy na D5
//termistor pro teplotu CPU: 3V3 - 10k rezistor - A0 - termistor - GND
//ventilator-pro: historie teploty 24h, systemovy monitoring

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>

#include "OtaConfig.h"
#include "OtaState.h"
#include "OtaManager.h"

// Piny
#define RELAY_PIN       D1  // GPIO5  - rele
#define MODE_SWITCH_PIN D2  // GPIO4  - prepinac rucniho rezimu
#define ONE_WIRE_BUS    D4  // GPIO2  - DS18B20
#define FAN_SWITCH_PIN  D5  // GPIO14 - prepinac ventilatoru v rucnim rezimu

// Termistor pro teplotu CPU
#define THERMISTOR_PIN     A0
#define THERM_NOMINAL      10000   // odpor NTC pri 25°C [ohm]
#define THERM_SERIES       10000   // seriovy rezistor [ohm]
#define THERM_BCOEFF       3950    // B koeficient NTC
#define THERM_NOMINAL_TEMP 25.0    // nominalni teplota [°C]

// Debounce
#define DEBOUNCE_MS 50

// EEPROM
#define EEPROM_SIZE   16
#define EEPROM_MAGIC  0xAB
#define EEPROM_ADDR_MAGIC    0
#define EEPROM_ADDR_TEMP_ON  1  // float, 4 byty
#define EEPROM_ADDR_TEMP_OFF 5  // float, 4 byty

// Historie teploty - 24h po 2 minutach = 720 bodu
#define HIST_SIZE     720
#define HIST_INTERVAL 120000UL  // 2 minuty v ms
#define HIST_NO_DATA  -9990     // sentinel

// Rezimy
enum Mode { MODE_AUTO, MODE_MANUAL, MODE_HAND };

// Periferie
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);

// Stav
Mode currentMode = MODE_AUTO;
bool fanState = false;
float temperature = 0.0;
bool sensorError = false;
float tempMin = 999.0;
float tempMax = -999.0;
float thresholdOn  = 27.0;
float thresholdOff = 25.0;
unsigned long bootTime = 0;

// Systemove info
uint32_t minFreeHeap = 0xFFFFFFFF;
float cpuTemperature = 0.0;
bool cpuTempError = false;

// DS18B20 spolehlivost
uint8_t sensorErrorCount = 0;
#define SENSOR_REINIT_THRESHOLD 5  // po 5 chybach reinicializace senzoru

// Debounce stav
bool modeSwitchState = HIGH;
bool modeSwitchLastRaw = HIGH;
unsigned long modeSwitchLastChange = 0;

bool fanSwitchState = HIGH;
bool fanSwitchLastRaw = HIGH;
unsigned long fanSwitchLastChange = 0;

// Casovace
unsigned long lastTempRead = 0;
unsigned long lastHistRecord = 0;

// Async cteni teploty
bool tempRequested = false;
unsigned long tempRequestTime = 0;
#define TEMP_CONVERSION_MS 750  // 12-bit rozliseni

// Historie teploty - kruhovy buffer
int16_t histBuf[HIST_SIZE];
int histIdx = 0;
int histCount = 0;

// ---------- EEPROM ----------

void eepromLoadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EEPROM_ADDR_MAGIC) == EEPROM_MAGIC) {
    EEPROM.get(EEPROM_ADDR_TEMP_ON, thresholdOn);
    EEPROM.get(EEPROM_ADDR_TEMP_OFF, thresholdOff);
    if (isnan(thresholdOn) || isnan(thresholdOff) ||
        thresholdOn < 0 || thresholdOn > 80 ||
        thresholdOff < 0 || thresholdOff > 80 ||
        thresholdOff >= thresholdOn) {
      thresholdOn = 27.0;
      thresholdOff = 25.0;
    }
    Serial.print("EEPROM: prahy on=");
    Serial.print(thresholdOn);
    Serial.print(" off=");
    Serial.println(thresholdOff);
  } else {
    Serial.println("EEPROM: prvni spusteni, vychozi prahy");
    eepromSaveSettings();
  }
}

void eepromSaveSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.put(EEPROM_ADDR_TEMP_ON, thresholdOn);
  EEPROM.put(EEPROM_ADDR_TEMP_OFF, thresholdOff);
  EEPROM.commit();
  Serial.println("EEPROM: prahy ulozeny");
}

// ---------- Ventilator ----------

void setFanState(bool state) {
  if (fanState == state) return;  // ochrana rele pred zbytecnym prepinanim
  fanState = state;
  digitalWrite(RELAY_PIN, state ? HIGH : LOW);
}

// ---------- Debounce + prepinace ----------

bool debounce(int pin, bool &lastRaw, bool &stableState, unsigned long &lastChange) {
  bool raw = digitalRead(pin);
  if (raw != lastRaw) {
    lastChange = millis();
    lastRaw = raw;
  }
  if ((millis() - lastChange) > DEBOUNCE_MS && stableState != lastRaw) {
    stableState = lastRaw;
    return true;
  }
  return false;
}

void readSwitches() {
  bool changed = debounce(MODE_SWITCH_PIN, modeSwitchLastRaw, modeSwitchState, modeSwitchLastChange);
  if (changed) {
    if (modeSwitchState == LOW) {
      currentMode = MODE_HAND;
      Serial.println("Prepinac: rucni rezim");
    } else {
      currentMode = MODE_AUTO;
      Serial.println("Prepinac: auto rezim");
    }
  }
  if (currentMode == MODE_HAND) {
    debounce(FAN_SWITCH_PIN, fanSwitchLastRaw, fanSwitchState, fanSwitchLastChange);
    setFanState(fanSwitchState == LOW);
  }
}

// ---------- Teplota ----------

// Cteni teploty je reseno neblokujicim zpusobem primo v loop()

// ---------- Historie teploty ----------

void histInit() {
  for (int i = 0; i < HIST_SIZE; i++) histBuf[i] = HIST_NO_DATA;
}

void histRecord() {
  if (sensorError) return;
  histBuf[histIdx] = (int16_t)(temperature * 10.0);
  histIdx = (histIdx + 1) % HIST_SIZE;
  if (histCount < HIST_SIZE) histCount++;
}

// ---------- Systemove info ----------

void trackHeap() {
  uint32_t free = ESP.getFreeHeap();
  if (free < minFreeHeap) minFreeHeap = free;
}

void updateCpuTemp() {
  int raw = analogRead(THERMISTOR_PIN);
  if (raw <= 0 || raw >= 1023) {
    cpuTempError = true;
    return;
  }
  float resistance = (float)THERM_SERIES * (float)raw / (1023.0 - (float)raw);
  float steinhart = log(resistance / (float)THERM_NOMINAL) / (float)THERM_BCOEFF;
  steinhart += 1.0 / ((float)THERM_NOMINAL_TEMP + 273.15);
  steinhart = 1.0 / steinhart - 273.15;
  cpuTemperature = steinhart;
  cpuTempError = false;
}

// ---------- Uptime ----------

String formatUptime() {
  unsigned long sec = (millis() - bootTime) / 1000;
  unsigned long d = sec / 86400; sec %= 86400;
  unsigned long h = sec / 3600;  sec %= 3600;
  unsigned long m = sec / 60;    sec %= 60;
  char buf[32];
  if (d > 0) sprintf(buf, "%lud %02lu:%02lu:%02lu", d, h, m, sec);
  else sprintf(buf, "%02lu:%02lu:%02lu", h, m, sec);
  return String(buf);
}

// ---------- Cas ----------

String getFormattedTime() {
  unsigned long rawTime = timeClient.getEpochTime();
  unsigned long hours = (rawTime % 86400L) / 3600;
  unsigned long minutes = (rawTime % 3600) / 60;
  unsigned long seconds = rawTime % 60;
  char buf[9];
  sprintf(buf, "%02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(buf);
}

// ---------- Web: Dashboard HTML ----------

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="cs">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ventilator PRO</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',Tahoma,sans-serif;background:#1a1a2e;color:#e0e0e0;min-height:100vh;display:flex;flex-direction:column}
header{background:linear-gradient(135deg,#16213e,#0f3460);padding:16px 20px;text-align:center;box-shadow:0 2px 10px rgba(0,0,0,.3)}
header h1{font-size:1.4em;color:#e94560}
header .time{font-size:.85em;color:#a0a0b0;margin-top:4px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px;padding:14px;max-width:800px;margin:0 auto;width:100%}
.card{background:#16213e;border-radius:12px;padding:18px;box-shadow:0 4px 12px rgba(0,0,0,.3);display:flex;flex-direction:column;justify-content:center}
.card-wide{grid-column:1/-1}
.card h2{font-size:.8em;text-transform:uppercase;color:#a0a0b0;margin-bottom:10px;letter-spacing:1px}
.temp-value{font-size:2.8em;font-weight:700;text-align:center;line-height:1.1}
.temp-ok{color:#4ecca3}
.temp-warm{color:#f0a500}
.temp-hot{color:#e94560}
.temp-err{color:#666}
.temp-minmax{display:flex;justify-content:space-between;margin-top:10px;font-size:.8em;color:#a0a0b0}
.fan-wrap{text-align:center;padding:8px 0}
.fan-svg{width:80px;height:80px;margin:0 auto;display:block}
@keyframes spin{from{transform:rotate(0)}to{transform:rotate(360deg)}}
.fan-on .fan-svg{animation:spin .8s linear infinite}
.fan-on .fan-blade{fill:#4ecca3}
.fan-off .fan-blade{fill:#555}
.fan-on .fan-hub{fill:#3ba88a}
.fan-off .fan-hub{fill:#444}
.fan-label{text-align:center;margin-top:8px;font-size:1em}
.fan-label.on{color:#4ecca3}
.fan-label.off{color:#e94560}
.mode-btns{display:flex;gap:8px;margin-bottom:12px}
.mode-btns button{flex:1;padding:12px 8px;border:none;border-radius:8px;font-size:.95em;cursor:pointer;transition:background .2s,transform .1s;-webkit-tap-highlight-color:transparent}
.mode-btns button:active{transform:scale(.96)}
.btn-auto{background:#0f3460;color:#e0e0e0}
.btn-auto.active{background:#4ecca3;color:#1a1a2e;font-weight:700}
.btn-manual{background:#0f3460;color:#e0e0e0}
.btn-manual.active{background:#f0a500;color:#1a1a2e;font-weight:700}
.hand-indicator{background:#e94560;color:#fff;text-align:center;padding:10px;border-radius:8px;margin-bottom:12px;font-weight:700}
.fan-toggle{width:100%;padding:14px;border:none;border-radius:8px;font-size:1em;cursor:pointer;font-weight:700;-webkit-tap-highlight-color:transparent}
.fan-toggle.on{background:#e94560;color:#fff}
.fan-toggle.off{background:#4ecca3;color:#1a1a2e}
.fan-toggle:disabled{background:#333;color:#666;cursor:not-allowed}
.thresh-current{display:flex;justify-content:space-around;margin-bottom:14px;padding:10px;background:#0a0f24;border-radius:8px}
.thresh-current span{font-size:.85em;color:#a0a0b0}
.thresh-current b{color:#e0e0e0}
.thresh-divider{border:0;border-top:1px solid #333;margin-bottom:12px}
.settings-row{display:flex;align-items:center;gap:8px;margin-bottom:10px}
.settings-row label{flex:0 0 auto;font-size:.85em;color:#a0a0b0;white-space:nowrap}
.settings-row input{flex:1;min-width:0;padding:10px 6px;border:1px solid #333;border-radius:6px;background:#0f3460;color:#e0e0e0;font-size:1em;text-align:center;-webkit-appearance:none}
.btn-save{width:100%;padding:12px;border:none;border-radius:8px;background:#4ecca3;color:#1a1a2e;font-weight:700;font-size:.95em;cursor:pointer;margin-top:4px;-webkit-tap-highlight-color:transparent}
.btn-save:active{transform:scale(.96)}
.msg{text-align:center;font-size:.8em;margin-top:6px;min-height:1.2em}
.msg-ok{color:#4ecca3}
.msg-err{color:#e94560}
.chart-wrap{position:relative;width:100%;height:0;padding-bottom:35%;min-height:180px}
.chart-wrap canvas{position:absolute;top:0;left:0;width:100%;height:100%}
.sys-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.sys-item{background:#0a0f24;border-radius:8px;padding:10px;text-align:center}
.sys-val{font-size:1.3em;font-weight:700;color:#e0e0e0;margin-bottom:2px}
.sys-lbl{font-size:.7em;color:#a0a0b0;text-transform:uppercase;letter-spacing:.5px}
.sys-ok{color:#4ecca3}
.sys-warn{color:#f0a500}
.sys-bad{color:#e94560}
footer{margin-top:auto;background:#0f3460;padding:12px 16px;text-align:center;font-size:.75em;color:#a0a0b0;display:flex;justify-content:space-around;flex-wrap:wrap;gap:6px}
@media(max-width:500px){
.grid{grid-template-columns:1fr;gap:12px;padding:12px}
.card{padding:16px}
.temp-value{font-size:2.4em}
.fan-svg{width:70px;height:70px}
header h1{font-size:1.2em}
.chart-wrap{padding-bottom:50%;min-height:150px}
.sys-grid{grid-template-columns:repeat(2,1fr)}
}
@media(min-width:800px){
.grid{grid-template-columns:1fr 1fr 1fr 1fr;max-width:1100px}
.card-wide{grid-column:1/-1}
.temp-value{font-size:2.5em}
}
</style>
</head>
<body>
<header>
<h1>&#9881; Ventilator PRO</h1>
<div class="time" id="hTime">--:--:--</div>
</header>
<div class="grid">

<div class="card">
<h2>Teplota</h2>
<div class="temp-value" id="cTemp">--</div>
<div class="temp-minmax"><span>Min: <span id="cTMin">--</span></span><span>Max: <span id="cTMax">--</span></span></div>
</div>

<div class="card">
<h2>Ventilator</h2>
<div class="fan-wrap fan-off" id="cFanIcon">
<svg class="fan-svg" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
<path class="fan-blade" d="M50 46C50 46 30 10 20 8C10 6 8 20 14 30C20 40 46 50 46 50Z"/>
<path class="fan-blade" d="M54 50C54 50 90 30 92 20C94 10 80 8 70 14C60 20 50 46 50 46Z"/>
<path class="fan-blade" d="M50 54C50 54 70 90 80 92C90 94 92 80 86 70C80 60 54 50 54 50Z"/>
<path class="fan-blade" d="M46 50C46 50 10 70 8 80C6 90 20 92 30 86C40 80 50 54 50 54Z"/>
<circle class="fan-hub" cx="50" cy="50" r="7"/>
</svg>
</div>
<div class="fan-label off" id="cFanLabel">--</div>
</div>

<div class="card">
<h2>Rezim</h2>
<div id="handBanner" class="hand-indicator" style="display:none">Rucni rezim (prepinac)</div>
<div class="mode-btns" id="modeBtns">
<button class="btn-auto" id="btnAuto" onclick="setMode('auto')">Auto</button>
<button class="btn-manual" id="btnManual" onclick="setMode('manual')">Manualni</button>
</div>
<button class="fan-toggle off" id="btnFan" onclick="toggleFan()" disabled>Zapnout ventilator</button>
</div>

<div class="card">
<h2>Teplotni prahy</h2>
<div class="thresh-current"><span>Zapnuti: <b id="curOn">--</b>&deg;C</span><span>Vypnuti: <b id="curOff">--</b>&deg;C</span></div>
<hr class="thresh-divider">
<div class="settings-row"><label>Zapnout &ge;</label><input type="number" id="inOn" step="0.5" min="0" max="80"> <label>&deg;C</label></div>
<div class="settings-row"><label>Vypnout &le;</label><input type="number" id="inOff" step="0.5" min="0" max="80"> <label>&deg;C</label></div>
<button class="btn-save" onclick="saveSettings()">Ulozit</button>
<div class="msg" id="setMsg"></div>
</div>

<div class="card card-wide">
<h2>Teplota: <span id="chartSpan">--</span></h2>
<div class="chart-wrap"><canvas id="chart"></canvas></div>
</div>

<div class="card card-wide">
<h2>System</h2>
<div class="sys-grid">
<div class="sys-item"><div class="sys-val" id="sCpuTemp">--</div><div class="sys-lbl">Teplota CPU</div></div>
<div class="sys-item"><div class="sys-val" id="sHeap">--</div><div class="sys-lbl">Volna RAM</div></div>
<div class="sys-item"><div class="sys-val" id="sHeapMin">--</div><div class="sys-lbl">Min RAM</div></div>
<div class="sys-item"><div class="sys-val" id="sFrag">--</div><div class="sys-lbl">Fragmentace</div></div>
<div class="sys-item"><div class="sys-val" id="sRssi">--</div><div class="sys-lbl">WiFi signal</div></div>
<div class="sys-item"><div class="sys-val" id="sCpuFreq">--</div><div class="sys-lbl">CPU frekvence</div></div>
</div>
</div>

<a href="https://sobelectronic.endora.site" target="_blank" class="card card-wide" style="text-decoration:none;text-align:center;color:inherit">
<h2>Manual a podpora</h2>
<div style="color:#4ecca3;font-weight:700;font-size:1.1em;margin-bottom:8px">sobelectronic.endora.site</div>
<div style="font-size:.8em;color:#a0a0b0">Kod produktu: <span style="color:#f0a500;font-family:'JetBrains Mono',monospace;font-weight:700;letter-spacing:2px">VENTPRO2026</span></div>
</a>

</div>
<footer>
<span>IP: <span id="fIP">--</span></span>
<span>WiFi: <span id="fRSSI">--</span> dBm</span>
<span>Uptime: <span id="fUp">--</span></span>
</footer>
<script>
var inited=false;
var u=function(){
 fetch('/api/status').then(function(r){return r.json();}).then(function(d){
 if(!d)return;
 document.getElementById('hTime').textContent=d.time;
 var tv=document.getElementById('cTemp');
 if(d.sensorError){tv.textContent='ERR';tv.className='temp-value temp-err';}
 else{tv.textContent=d.temperature.toFixed(1)+'\u00B0C';
 tv.className='temp-value '+(d.temperature>=d.thresholdOn?'temp-hot':d.temperature>=d.thresholdOff?'temp-warm':'temp-ok');}
 document.getElementById('cTMin').textContent=d.tempMin>900?'--':d.tempMin.toFixed(1)+'\u00B0C';
 document.getElementById('cTMax').textContent=d.tempMax<-900?'--':d.tempMax.toFixed(1)+'\u00B0C';
 var fi=document.getElementById('cFanIcon');
 fi.className='fan-wrap '+(d.fanState?'fan-on':'fan-off');
 var fl=document.getElementById('cFanLabel');
 fl.textContent=d.fanState?'Zapnuty':'Vypnuty';
 fl.className='fan-label '+(d.fanState?'on':'off');
 var hb=document.getElementById('handBanner');
 var mb=document.getElementById('modeBtns');
 var bf=document.getElementById('btnFan');
 if(d.mode==='hand'){hb.style.display='block';mb.style.display='none';bf.disabled=true;bf.textContent='Rucni rezim';}
 else{hb.style.display='none';mb.style.display='flex';
 document.getElementById('btnAuto').className='btn-auto'+(d.mode==='auto'?' active':'');
 document.getElementById('btnManual').className='btn-manual'+(d.mode==='manual'?' active':'');
 if(d.mode==='manual'){bf.disabled=false;bf.textContent=d.fanState?'Vypnout ventilator':'Zapnout ventilator';bf.className='fan-toggle '+(d.fanState?'on':'off');}
 else{bf.disabled=true;bf.textContent='Automaticky rezim';bf.className='fan-toggle off';}}
 document.getElementById('curOn').textContent=d.thresholdOn;
 document.getElementById('curOff').textContent=d.thresholdOff;
 if(!inited){document.getElementById('inOn').value=d.thresholdOn;document.getElementById('inOff').value=d.thresholdOff;inited=true;}
 document.getElementById('fIP').textContent=d.ip;
 document.getElementById('fRSSI').textContent=d.rssi;
 document.getElementById('fUp').textContent=d.uptime;
 var cte=document.getElementById('sCpuTemp');
 if(d.cpuTempError){cte.textContent='ERR';cte.className='sys-val sys-bad';}
 else{cte.textContent=d.cpuTemp.toFixed(1)+'\u00B0C';cte.className='sys-val '+(d.cpuTemp<60?'sys-ok':d.cpuTemp<=75?'sys-warn':'sys-bad');}
 var hk=Math.round(d.freeHeap/1024);var he=document.getElementById('sHeap');he.textContent=hk+'KB';
 he.className='sys-val '+(hk>15?'sys-ok':hk>8?'sys-warn':'sys-bad');
 var hmk=Math.round(d.minFreeHeap/1024);var hme=document.getElementById('sHeapMin');hme.textContent=hmk+'KB';
 hme.className='sys-val '+(hmk>10?'sys-ok':hmk>5?'sys-warn':'sys-bad');
 var fe=document.getElementById('sFrag');fe.textContent=d.heapFrag+'%';
 fe.className='sys-val '+(d.heapFrag<30?'sys-ok':d.heapFrag<50?'sys-warn':'sys-bad');
 var re=document.getElementById('sRssi');re.textContent=d.rssi+'dBm';
 re.className='sys-val '+(d.rssi>-60?'sys-ok':d.rssi>-75?'sys-warn':'sys-bad');
 var cf=document.getElementById('sCpuFreq');cf.textContent=d.cpuFreq+'MHz';cf.className='sys-val sys-ok';
 }).catch(function(){});
};
var setMode=function(m){fetch('/api/mode?m='+m).then(function(){u();});};
var toggleFan=function(){fetch('/api/fan?state='+(document.getElementById('cFanIcon').classList.contains('fan-on')?'off':'on')).then(function(){u();});};
var saveSettings=function(){
 var on=parseFloat(document.getElementById('inOn').value);
 var off=parseFloat(document.getElementById('inOff').value);
 var msg=document.getElementById('setMsg');
 if(isNaN(on)||isNaN(off)){msg.textContent='Zadejte platna cisla';msg.className='msg msg-err';return;}
 if(off>=on){msg.textContent='Vypnuti musi byt nizsi nez zapnuti';msg.className='msg msg-err';return;}
 fetch('/api/settings?on='+on+'&off='+off).then(function(r){return r.json();}).then(function(d){
 if(d.ok){msg.textContent='Ulozeno';msg.className='msg msg-ok';u();}
 else{msg.textContent=d.error||'Chyba';msg.className='msg msg-err';}
 setTimeout(function(){msg.textContent='';},3000);
 });
};
var drawChart=function(data,cnt,intMin,tOn,tOff){
 var cv=document.getElementById('chart');
 var dpr=window.devicePixelRatio||1;
 var rw=cv.parentElement.clientWidth;
 var rh=cv.parentElement.clientHeight;
 cv.width=rw*dpr;cv.height=rh*dpr;
 cv.style.width=rw+'px';cv.style.height=rh+'px';
 var c=cv.getContext('2d');
 c.scale(dpr,dpr);
 var totalMin=cnt*intMin;
 var sp=document.getElementById('chartSpan');
 if(totalMin>=1440)sp.textContent='poslednich 24 hodin';
 else if(totalMin>=60)sp.textContent='poslednich '+Math.floor(totalMin/60)+'h '+(totalMin%60)+'min';
 else sp.textContent='poslednich '+totalMin+' min';
 var ml=45,mr=15,mt=15,mb=30;
 var w=rw-ml-mr,h=rh-mt-mb;
 var pts=[];var mn=999,mx=-999;
 for(var i=0;i<data.length;i++){if(data[i]>-990){var v=data[i]/10.0;pts.push({i:i,v:v});if(v<mn)mn=v;if(v>mx)mx=v;}}
 if(pts.length<2){c.fillStyle='#0f3460';c.fillRect(ml,mt,w,h);c.fillStyle='#a0a0b0';c.font='14px sans-serif';c.textAlign='center';c.fillText('Cekam na data...',rw/2,rh/2);return;}
 var pad=1;mn=Math.floor(mn-pad);mx=Math.ceil(mx+pad);if(mx-mn<4){mn-=2;mx+=2;}
 c.fillStyle='#0f3460';c.fillRect(ml,mt,w,h);
 c.strokeStyle='#1a1a2e';c.lineWidth=1;
 var steps=5;
 c.font='11px sans-serif';c.textAlign='right';c.fillStyle='#a0a0b0';
 for(var s=0;s<=steps;s++){var yv=mn+(mx-mn)*s/steps;var yy=mt+h-h*s/steps;c.beginPath();c.moveTo(ml,yy);c.lineTo(ml+w,yy);c.stroke();c.fillText(yv.toFixed(1),ml-5,yy+4);}
 c.textAlign='center';
 var tickMin;
 if(totalMin<=30)tickMin=5;
 else if(totalMin<=60)tickMin=10;
 else if(totalMin<=180)tickMin=30;
 else if(totalMin<=360)tickMin=60;
 else if(totalMin<=720)tickMin=120;
 else tickMin=360;
 for(var tm=tickMin;tm<=totalMin;tm+=tickMin){
 var xi=ml+w*(1-tm/totalMin);
 c.beginPath();c.moveTo(xi,mt);c.lineTo(xi,mt+h);c.strokeStyle='#1a1a2e';c.stroke();
 var lbl;if(tm<60)lbl=tm+'m';else if(tm%60===0)lbl=Math.floor(tm/60)+'h';else lbl=Math.floor(tm/60)+'h'+tm%60+'m';
 c.fillStyle='#a0a0b0';c.fillText('-'+lbl,xi,mt+h+15);}
 c.fillStyle='#a0a0b0';c.fillText('ted',ml+w,mt+h+15);
 if(tOn!==undefined){var yOn=mt+h-(tOn-mn)/(mx-mn)*h;var yOff=mt+h-(tOff-mn)/(mx-mn)*h;
 c.setLineDash([4,4]);
 if(yOn>=mt&&yOn<=mt+h){c.strokeStyle='#e94560';c.beginPath();c.moveTo(ml,yOn);c.lineTo(ml+w,yOn);c.stroke();}
 if(yOff>=mt&&yOff<=mt+h){c.strokeStyle='#4ecca3';c.beginPath();c.moveTo(ml,yOff);c.lineTo(ml+w,yOff);c.stroke();}
 c.setLineDash([]);c.strokeStyle='#1a1a2e';}
 var total=data.length;
 c.beginPath();c.strokeStyle='#f0a500';c.lineWidth=2;var first=true;
 for(var p=0;p<pts.length;p++){var px=ml+w*pts[p].i/(total-1);var py=mt+h-(pts[p].v-mn)/(mx-mn)*h;if(first){c.moveTo(px,py);first=false;}else c.lineTo(px,py);}
 c.stroke();
 c.beginPath();c.strokeStyle='transparent';c.fillStyle='rgba(240,165,0,0.1)';first=true;
 for(var p=0;p<pts.length;p++){var px=ml+w*pts[p].i/(total-1);var py=mt+h-(pts[p].v-mn)/(mx-mn)*h;if(first){c.moveTo(px,mt+h);c.lineTo(px,py);first=false;}else c.lineTo(px,py);}
 c.lineTo(ml+w*pts[pts.length-1].i/(total-1),mt+h);c.closePath();c.fill();
};
var loadHist=function(){
 fetch('/api/history').then(function(r){return r.json();}).then(function(d){
 if(d&&d.data)drawChart(d.data,d.count,d.intervalMin,d.thresholdOn,d.thresholdOff);
 }).catch(function(){});
};
u();loadHist();setInterval(u,2000);setInterval(loadHist,30000);
window.addEventListener('resize',function(){loadHist();});
</script>
</body>
</html>)rawliteral";

// ---------- Web: API handlery ----------

void handleDashboard() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleApiStatus() {
  String modeStr;
  switch (currentMode) {
    case MODE_HAND:   modeStr = "hand";   break;
    case MODE_MANUAL: modeStr = "manual"; break;
    default:          modeStr = "auto";   break;
  }

  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"mode\":\"%s\",\"fanState\":%s,\"temperature\":%.1f,"
    "\"sensorError\":%s,\"tempMin\":%.1f,\"tempMax\":%.1f,"
    "\"thresholdOn\":%.1f,\"thresholdOff\":%.1f,"
    "\"ip\":\"%s\",\"rssi\":%d,"
    "\"cpuTemp\":%.1f,\"cpuTempError\":%s,\"freeHeap\":%u,"
    "\"minFreeHeap\":%u,\"heapFrag\":%u,\"cpuFreq\":%u,"
    "\"uptime\":\"%s\",\"time\":\"%s\"}",
    modeStr.c_str(),
    fanState ? "true" : "false",
    temperature,
    sensorError ? "true" : "false",
    tempMin, tempMax,
    thresholdOn, thresholdOff,
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI(),
    cpuTemperature,
    cpuTempError ? "true" : "false",
    ESP.getFreeHeap(),
    minFreeHeap,
    ESP.getHeapFragmentation(),
    ESP.getCpuFreqMHz(),
    formatUptime().c_str(),
    getFormattedTime().c_str()
  );
  server.send(200, "application/json", buf);
}

void handleApiMode() {
  if (currentMode == MODE_HAND) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Rucni rezim je aktivni (prepinac)\"}");
    return;
  }
  String m = server.arg("m");
  if (m == "auto") {
    currentMode = MODE_AUTO;
    Serial.println("Web: auto rezim");
  } else if (m == "manual") {
    currentMode = MODE_MANUAL;
    Serial.println("Web: manualni rezim");
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiFan() {
  if (currentMode != MODE_MANUAL) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Ventilator lze ovladat jen v manualnim rezimu\"}");
    return;
  }
  String s = server.arg("state");
  if (s == "on") {
    setFanState(true);
    Serial.println("Web: ventilator zapnut");
  } else if (s == "off") {
    setFanState(false);
    Serial.println("Web: ventilator vypnut");
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiSettings() {
  float newOn = server.arg("on").toFloat();
  float newOff = server.arg("off").toFloat();

  if (newOn < 0 || newOn > 80 || newOff < 0 || newOff > 80) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Hodnoty mimo rozsah 0-80\"}");
    return;
  }
  if (newOff >= newOn) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Vypnuti musi byt nizsi nez zapnuti\"}");
    return;
  }

  thresholdOn = newOn;
  thresholdOff = newOff;
  eepromSaveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiHistory() {
  // Chunked odpoved - nestavime cely string v pameti
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  char hdr[96];
  snprintf(hdr, sizeof(hdr),
    "{\"thresholdOn\":%.1f,\"thresholdOff\":%.1f,\"intervalMin\":%lu,\"count\":%d,\"data\":[",
    thresholdOn, thresholdOff, HIST_INTERVAL / 60000UL, histCount);
  server.sendContent(hdr);

  int start = (histCount < HIST_SIZE) ? 0 : histIdx;
  int count = (histCount < HIST_SIZE) ? histCount : HIST_SIZE;

  // Posilame po davkach ~60 hodnot aby se nefragmentovala pamet
  String chunk;
  chunk.reserve(400);
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % HIST_SIZE;
    if (i > 0) chunk += ',';
    chunk += String(histBuf[idx]);
    if (chunk.length() > 300) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  if (chunk.length() > 0) server.sendContent(chunk);

  server.sendContent("]}");
  server.sendContent("");  // ukonci chunked prenos
}

// ---------- OTA ----------

void otaStatusCallback(const String& line1, const String& line2) {
  Serial.print("[OTA-STATUS] ");
  Serial.print(line1);
  Serial.print(" ");
  Serial.println(line2);
}

// ---------- Setup ----------

void setup() {
  Serial.begin(115200);
  bootTime = millis();

  // Historie
  histInit();

  // EEPROM
  eepromLoadSettings();

  // OTA stav (LittleFS) + pripadny rollback z minuleho OTA cyklu -
  // PRED pripojenim WiFi, aby rollback nezavisel na siti.
  OtaState::begin();
  OtaManager::setStatusCallback(otaStatusCallback);
  OtaManager::begin();

  // WiFi
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);  // portal se zavre po 3 min
  if (!wifiManager.autoConnect("ESP_Config")) {
    Serial.println("WiFi selhalo. Restart...");
    ESP.restart();
  }
  Serial.println("Pripojeno k WiFi!");
  Serial.print("IP adresa: ");
  Serial.println(WiFi.localIP());
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  // mDNS
  if (MDNS.begin("ventilator")) {
    Serial.println("mDNS: ventilator.local");
  }

  // Senzor + rele
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  sensors.begin();
  delay(100);  // cas na inicializaci DS18B20
  sensors.setWaitForConversion(false);
  sensors.setResolution(12);
  Serial.print("DS18B20 nalezeno senzoru: ");
  Serial.println(sensors.getDeviceCount());
  if (sensors.getDeviceCount() == 0) {
    Serial.println("VAROVANI: DS18B20 nedetekovan! Zkontrolujte zapojeni a 4.7k pull-up rezistor na data pin.");
  }

  // Prepinace
  pinMode(MODE_SWITCH_PIN, INPUT_PULLUP);
  pinMode(FAN_SWITCH_PIN, INPUT_PULLUP);

  modeSwitchState = digitalRead(MODE_SWITCH_PIN);
  modeSwitchLastRaw = modeSwitchState;
  if (modeSwitchState == LOW) {
    currentMode = MODE_HAND;
    Serial.println("Start: rucni rezim (prepinac)");
  }

  // NTP
  timeClient.begin();

  // Web server
  server.on("/", handleDashboard);
  server.on("/api/status", handleApiStatus);
  server.on("/api/mode", handleApiMode);
  server.on("/api/fan", handleApiFan);
  server.on("/api/settings", handleApiSettings);
  server.on("/api/history", handleApiHistory);
  server.begin();
  Serial.println("HTTP server spusten");

  minFreeHeap = ESP.getFreeHeap();

  // Potvrdit, ze firmware po WiFi pripojeni a startu serveru funguje -
  // jinak by po par neuspesnych bootech dosel k automatickemu rollbacku.
  OtaManager::notifyApplicationHealthy();
}

// ---------- Loop ----------

void loop() {
  server.handleClient();
  MDNS.update();
  timeClient.update();

  // OTA - neblokujici mimo aktivni kontrolu/update cyklus (viz
  // OTA_CHECK_INTERVAL_MS v OtaConfig.h). Pri stahovani/flashovani nove
  // verze tato funkce na chvili zablokuje loop() - rele mezitim zustava
  // v aktualnim stavu (nemeni se), takze to neni nebezpecne, jen se po
  // tu dobu neaktualizuje teplota a prepinace nezareaguji.
  OtaManager::handle();

  // Prepinace - kazdy pruchod
  readSwitches();

  // Teplota - kazde 2s, neblokujici dvoufa­zove cteni
  if (!tempRequested && (millis() - lastTempRead > 2000)) {
    sensors.requestTemperatures();
    tempRequested = true;
    tempRequestTime = millis();
  }
  if (tempRequested && (millis() - tempRequestTime >= TEMP_CONVERSION_MS)) {
    tempRequested = false;
    lastTempRead = millis();
    float t = sensors.getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C || t < -50.0 || t > 125.0) {
      sensorError = true;
      sensorErrorCount++;
      Serial.print("Chyba DS18B20! (");
      Serial.print(sensorErrorCount);
      Serial.println("x za sebou)");
      if (sensorErrorCount >= SENSOR_REINIT_THRESHOLD) {
        Serial.println("Reinicializace OneWire sbernice...");
        sensors.begin();
        delay(100);
        sensors.setWaitForConversion(false);
        sensors.setResolution(12);
        sensorErrorCount = 0;
        Serial.print("DS18B20 nalezeno senzoru: ");
        Serial.println(sensors.getDeviceCount());
      }
    } else {
      sensorError = false;
      sensorErrorCount = 0;
      temperature = t;
      if (t < tempMin) tempMin = t;
      if (t > tempMax) tempMax = t;
      if (currentMode == MODE_AUTO) {
        if (temperature >= thresholdOn) setFanState(true);
        else if (temperature <= thresholdOff) setFanState(false);
      }
    }
    updateCpuTemp();
    trackHeap();
  }

  // Historie - kazde 2 minuty
  if (millis() - lastHistRecord >= HIST_INTERVAL) {
    histRecord();
    lastHistRecord = millis();
  }

  yield();  // cas pro WiFi stack a WDT
}
