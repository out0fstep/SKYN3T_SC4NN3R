/* Skyn3t Scann3r (ESP32-S3, headless)
 * AP: Skyn3t_Scann3r / scanme123  •  UI: http://192.168.4.1
 * ESP32 core 2.0.13+  •  Libs: WiFi (ESP32), BLEDevice, ArduinoJson
 */

#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <ArduinoJson.h>
#include "skull_png.h"   // PROGMEM PNG bytes + length

// ---------- CONFIG ----------
const char* apSSID = "Skyn3t_Scann3r";
const char* apPass = "scanme123";
const IPAddress apIP(192,168,4,1), apGateway(192,168,4,1), apNetmask(255,255,255,0);

WebServer server(80);

volatile bool wifiScanning=false, bleScanning=false;

struct WifiResult{ String ssid,bssid; int channel,rssi; String enc; };
struct BleResult { String addr,name;  int rssi; };

#define MAX_WIFI_RESULTS 256
#define MAX_BLE_RESULTS  256

std::vector<WifiResult> wifiResults;
std::vector<BleResult>  bleResults;

portMUX_TYPE resultsMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t wifiTaskHandle=nullptr, bleTaskHandle=nullptr;

// ---------- HELPERS ----------
String encType(uint8_t t, bool hidden){
  switch(t){
    case WIFI_AUTH_OPEN:            return hidden?"HIDDEN":"OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    default:                        return hidden?"HIDDEN":"UNKNOWN";
  }
}

// ---------- WIFI SCAN TASK (accumulate per session) ----------
static bool findWifiByBssid(const String& b, size_t& idx){
  for(size_t i=0;i<wifiResults.size();++i){ if(wifiResults[i].bssid==b){ idx=i; return true; } }
  return false;
}
void wifiScanTask(void*){
  while(true){
    if(!wifiScanning){ vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    int n = WiFi.scanNetworks(false,true,false,300);
    if(n<0){ vTaskDelay(pdMS_TO_TICKS(800)); continue; }

    portENTER_CRITICAL(&resultsMux);
    for(int i=0;i<n;++i){
      WifiResult r;
      r.ssid=WiFi.SSID(i); r.bssid=WiFi.BSSIDstr(i); r.channel=WiFi.channel(i); r.rssi=WiFi.RSSI(i);
      r.enc=encType(WiFi.encryptionType(i), r.ssid.length()==0);
      size_t idx;
      if(findWifiByBssid(r.bssid,idx)){
        wifiResults[idx].rssi=r.rssi; wifiResults[idx].channel=r.channel; wifiResults[idx].enc=r.enc;
        if(wifiResults[idx].ssid.isEmpty() && !r.ssid.isEmpty()) wifiResults[idx].ssid=r.ssid;
      }else if((int)wifiResults.size()<MAX_WIFI_RESULTS) wifiResults.push_back(r);
    }
    portEXIT_CRITICAL(&resultsMux);
    vTaskDelay(pdMS_TO_TICKS(1200));
  }
}

// ---------- BLE (accumulating) ----------
class BLECollector: public BLEAdvertisedDeviceCallbacks{
  void onResult(BLEAdvertisedDevice d) override{
    BleResult br; br.addr=d.getAddress().toString().c_str();
    String n=d.getName().c_str(); if(n.length()==0) n="<unknown>"; br.name=n; br.rssi=d.getRSSI();

    portENTER_CRITICAL(&resultsMux);
    bool found=false;
    for(auto &x:bleResults){
      if(x.addr==br.addr){ x.rssi=br.rssi; if(x.name=="<unknown>" && br.name!="<unknown>") x.name=br.name; found=true; break; }
    }
    if(!found && (int)bleResults.size()<MAX_BLE_RESULTS) bleResults.push_back(br);
    portEXIT_CRITICAL(&resultsMux);
  }
};
void bleScanTask(void*){
  BLEDevice::init("");
  BLEScan* scan=BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new BLECollector());
  scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99);
  while(true){
    if(!bleScanning){ vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    scan->start(3,false); scan->clearResults(); vTaskDelay(pdMS_TO_TICKS(400));
  }
}

// ---------- WEB UI ----------
void handleIndex(){
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"text/html","");
  auto S=[&](const __FlashStringHelper* x){ server.sendContent(x); };

  S(F("<!doctype html><html><head><meta charset='utf-8'>"
     "<meta name='viewport' content='width=device-width,initial-scale=1'>"
     "<title>Skyn3t Scann3r</title><style>"
     ":root{--bg:#0a0a0a;--pink:#ff2fa6;--cyan:#20e6ff;--muted:#bdbdbd;--radius:22px;--border:2px;"
     "--stripeH:16px; --blockH:10px; --skullW:132px;"
     "--titleMax:64px; --titleMin:38px;}"  /* larger title */
     "@media (min-width:600px){:root{--skullW:148px; --titleMax:70px}}"
     "@media (min-width:900px){:root{--skullW:168px; --titleMax:78px}}"

     "html,body{margin:0;background:var(--bg);color:#fff;font-family:Inter,Segoe UI,Roboto,Arial}"
     ".wrap{max-width:820px;margin:0 auto;padding:14px}"

     /* ===== BANNER ===== */
     ".banner{position:relative}"
     ".stripe{height:var(--stripeH);border-radius:6px;opacity:.95;"
       "background:repeating-linear-gradient(45deg,#fff 0 16px,#111 16px 32px)}"
     ".between{position:relative}"
     ".titlePad{padding-right:calc(var(--skullW) + 12px)}" /* only title avoids skull */
     ".title{line-height:1.05;margin:8px 0}"
     ".title .l{display:block;font-weight:900;letter-spacing:.12em;color:var(--pink);"
       "font-size:clamp(var(--titleMin),8.6vw,var(--titleMax));}"  /* a bit bigger on mobile */
     ".blocks{height:var(--blockH);margin:8px 0;background:repeating-linear-gradient(90deg,transparent 0 10px,#2a2a2a 10px 20px)}"
     ".skull{position:absolute;right:0;top:0;bottom:0;width:var(--skullW);height:auto;object-fit:contain;pointer-events:none;z-index:2}"

     /* ===== GENERAL ===== */
     ".row{display:flex;align-items:center;justify-content:space-between}"
     ".statusrow{margin:10px 0 8px;border-bottom:3px solid #222;padding-bottom:6px}"
     ".statuslbl{color:var(--cyan);font-weight:800;font-size:26px;margin-right:8px}"
     ".statusval{display:inline-block;background:#121212;border:1px solid #2a2a2a;border-radius:8px;padding:4px 10px;"
       "font-family:ui-monospace,Consolas,monospace;font-size:22px}"

     ".card{background:linear-gradient(180deg,#0f0f0f,#0c0c0c);border-radius:var(--radius);padding:18px;"
       "border:var(--border) solid #1a1a1a;box-shadow:0 0 0 1px rgba(255,255,255,.02) inset}"
     ".pink{border-color:rgba(255,47,166,.6)} .cyan{border-color:rgba(32,230,255,.5)}"
     ".sectionTitle{display:inline-flex;align-items:center;gap:8px;padding:12px 18px;border-radius:18px;background:#0c0c0c;border:2px solid rgba(255,255,255,.08);font-size:22px}"
     ".sectionTitle.pink{background:#121015;border-color:rgba(255,47,166,.55)}"
     ".sectionTitle.cyan{background:#101416;border-color:rgba(32,230,255,.45)}"

     ".btnrow{display:flex;gap:12px;flex-wrap:nowrap;overflow-x:auto;padding-bottom:2px}"
     ".btn{display:inline-flex;align-items:center;justify-content:center;padding:14px 20px;border-radius:18px;background:#151515;border:1px solid #2a2a2a;color:#fff;cursor:pointer;font-weight:600;white-space:nowrap}"
     ".btn.primary{background:var(--pink);color:#0b0b0b;border:none}"
     ".btn.primary.cyanfill{background:var(--cyan);color:#0b0b0b;border:none}"
     ".btn.outline-pink{background:#151515;border:2px solid rgba(255,47,166,.55)}"
     ".btn.outline-cyan{background:#151515;border:2px solid rgba(32,230,255,.45)}"

     /* ===== Radar indicator ===== */
     "@keyframes pulse{0%{transform:scale(1);opacity:.35}50%{transform:scale(1.15);opacity:1}100%{transform:scale(1);opacity:.35}}"
     ".radar{display:inline-flex;align-items:center;justify-content:center;width:20px;height:20px;opacity:.0;transform-origin:center;}"
     ".radar svg{width:20px;height:20px;display:block}"
     ".radar.cyan{color:var(--cyan)} .radar.pink{color:var(--pink)}"
     ".radar.on{opacity:1;animation:pulse .8s ease-in-out infinite}"

     /* ===== Results (smaller) ===== */
     ".resultsHeader{margin-top:18px}"
     ".resultsTitle{font-size:20px;color:var(--pink);font-weight:900;letter-spacing:.03em}"
     ".floppy{width:52px;height:52px;border-radius:14px;border:2px solid rgba(32,230,255,.35);display:flex;align-items:center;justify-content:center;background:#0b0b0b;cursor:pointer}"
     ".legend{display:flex;gap:18px;color:#bdbdbd;margin:10px 10px;font-size:16px;align-items:center}"
     ".dot{width:14px;height:14px;border-radius:50%;display:inline-block;margin-right:8px}"
     ".dot.cyan{background:var(--cyan)} .dot.pink{background:var(--pink)}"
     ".legend .count{font-weight:900;color:#fff}"

     ".tableCard{background:#0c0c0c;border-radius:18px;border:2px solid #151515;padding:0;overflow:hidden;margin-top:8px}"
     "table{width:100%;border-collapse:collapse}"
     "thead th{background:#151515;color:#bdbdbd;font-weight:900;font-size:16px;padding:12px 10px;border-bottom:1px solid #1e1e1e;text-align:left}"
     "tbody td{padding:12px 10px;border-bottom:1px solid #141414;font-size:16px}"
     "tbody tr:last-child td{border-bottom:none}"
     ".pinktext{color:var(--pink)} .cyantext{color:var(--cyan)}"
     "</style></head><body><div class='wrap'>"));

  // ===== Banner =====
  S(F(
    "<div class='banner'>"
      "<div class='between'>"
        "<div class='stripe'></div>"
        "<div class='titlePad'>"
          "<div class='title'>"
            "<span class='l'>SKYN3T</span>"
            "<span class='l'>SC4NN3R</span>"
          "</div>"
        "</div>"
        "<div class='stripe'></div>"
        "<img src='/skull.png' alt='skull' class='skull'/>"
      "</div>"
      "<div class='blocks'></div>"
    "</div>"
  ));

  // Caption line
  S(F("<div style='color:#bdbdbd;font-weight:700;margin:10px 0 8px'>WiFi & Bluetooth スキャナー"
     "<span style='float:right;opacity:.7'>\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\</span></div>"));

  // Status
  S(F("<div class='statusrow'><span class='statuslbl'>Status:</span>"
     "<span class='statusval' id='statusText'>idle</span></div>"));

  // Wi-Fi card
  S(F("<div class='card pink'>"
     "<div class='sectionTitle pink'>"
       "Wi-Fi"
       "<span id='wifiRadar' class='radar cyan' title='Wi-Fi scanning'>"
       "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'>"
       "<circle cx='12' cy='12' r='1' fill='currentColor'/>"
       "<path d='M2 9a14 14 0 0 1 20 0'/>"
       "<path d='M5 12a10 10 0 0 1 14 0'/>"
       "<path d='M8 15a6 6 0 0 1 8 0'/>"
       "</svg></span>"
     "</div>"
     "<div style='height:12px'></div>"
     "<div class='btnrow'>"
       "<button class='btn primary' onclick='startWifi()'>Start Scan</button>"
       "<button class='btn outline-pink' onclick='stopWifi()'>Stop Scan</button>"
       "<button class='btn outline-pink' onclick='clearWifi()'>Clear</button>"
     "</div></div>"));

  // Bluetooth card
  S(F("<div style='height:14px'></div><div class='card cyan'>"
     "<div class='sectionTitle cyan'>"
       "Bluetooth"
       "<span id='bleRadar' class='radar pink' title='BT scanning'>"
       "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'>"
       "<circle cx='12' cy='12' r='1' fill='currentColor'/>"
       "<path d='M2 9a14 14 0 0 1 20 0'/>"
       "<path d='M5 12a10 10 0 0 1 14 0'/>"
       "<path d='M8 15a6 6 0 0 1 8 0'/>"
       "</svg></span>"
     "</div>"
     "<div style='height:12px'></div>"
     "<div class='btnrow'>"
       "<button class='btn primary cyanfill' onclick='startBle()'>Start Scan</button>"
       "<button class='btn outline-cyan' onclick='stopBle()'>Stop Scan</button>"
       "<button class='btn outline-cyan' onclick='clearBle()'>Clear</button>"
     "</div></div>"));

  // Separator
  S(F("<div style='height:18px'></div><div class='stripe'></div>"));

  // Results header + floppy
  S(F("<div class='resultsHeader row'>"
     "<div class='resultsTitle'>Scan Results &nbsp;&gt;&gt;&gt;&gt;&gt;&gt;&gt;</div>"
     "<div class='floppy' onclick='downloadCSV()' title='Save results'>"
     "<svg width='24' height='24' viewBox='0 0 24 24' fill='none' stroke='#20e6ff' stroke-width='2'>"
     "<rect x='3' y='3' width='18' height='18' rx='3' stroke='#20e6ff'/>"
     "<rect x='7' y='5' width='6' height='6' fill='#20e6ff'/>"
     "<rect x='7' y='14' width='10' height='5' stroke='#20e6ff'/></svg></div></div>"));

  // Counts
  S(F("<div class='legend'>"
     "<div><span class='dot cyan'></span> BT devices: <span class='count' id='btCount'>0</span></div>"
     "<div><span class='dot pink'></span> Wi-Fi devices: <span class='count' id='wifiCount'>0</span></div>"
     "</div>"));

  // Results table (trimmed columns)
  S(F("<div class='tableCard'><table id='resultsTable'>"
     "<thead><tr><th style='width:44%'>SSID / Name</th><th style='width:44%'>BSSID / Addr</th><th style='width:12%'>Ch</th></tr></thead>"
     "<tbody id='resultsBody'></tbody></table></div>"));

  // Bottom stripe
  S(F("<div style='height:16px'></div><div class='stripe'></div>"));

  // JS
  S(F("<script>"
     "function esc(s){if(!s)return'';return s.replace(/[&<>\"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;','\\'':'&#39;'}[m]));}"
     "function setRadar(id,on){const el=document.getElementById(id); if(!el) return; el.classList.toggle('on',!!on);} "
     "function render(j){"
       "document.getElementById('statusText').innerText=j.status;"
       "document.getElementById('wifiCount').innerText=j.wifi_count;"
       "document.getElementById('btCount').innerText=j.bt_count;"
       "setRadar('wifiRadar', j.wifi_active);"
       "setRadar('bleRadar',  j.bt_active);"
       "const tb=document.getElementById('resultsBody');"
       "let rows='';"
       "j.wifi.forEach(w=>{rows+=`<tr><td class='pinktext'>${esc(w.ssid)}</td><td>${esc(w.bssid)}</td><td style='text-align:center'>${w.channel}</td></tr>`;});"
       "j.ble.forEach(b=>{rows+=`<tr><td class='cyantext'>${esc(b.name)}</td><td>${esc(b.addr)}</td><td style='text-align:center'></td></tr>`;});"
       "tb.innerHTML=rows;"
     "}"
     "function fetchStatus(){fetch('/status').then(r=>r.json()).then(render).catch(()=>{});} "
     "function startWifi(){fetch('/startWifi').then(fetchStatus);} "
     "function stopWifi(){ fetch('/stopWifi').then(fetchStatus);} "
     "function clearWifi(){fetch('/clearWifi').then(fetchStatus);} "
     "function startBle(){ fetch('/startBle').then(fetchStatus);} "
     "function stopBle(){  fetch('/stopBle').then(fetchStatus);} "
     "function clearBle(){ fetch('/clearBle').then(fetchStatus);} "
     "function downloadCSV(){window.location='/export';} "
     "setInterval(fetchStatus,1200); fetchStatus();"
     "</script>"));

  S(F("</div></body></html>"));
  server.client().stop();
}

// ---------- CSV ----------
void handleExport(){
  String csv="type,ssid_or_name,bssid_or_addr,channel,rssi,encryption\n";
  portENTER_CRITICAL(&resultsMux);
  for(auto &w:wifiResults)
    csv+="wifi,\""+w.ssid+"\",\""+w.bssid+"\","+String(w.channel)+","+String(w.rssi)+",\""+w.enc+"\"\n";
  for(auto &b:bleResults)
    csv+="bt,\""+b.name+"\",\""+b.addr+"\",,"+String(b.rssi)+",\n";
  portEXIT_CRITICAL(&resultsMux);
  server.sendHeader("Cache-Control","no-cache");
  server.sendHeader("Content-Disposition","attachment; filename=\"scan_results.csv\"");
  server.send(200,"text/csv",csv);
}

// ---------- JSON + CONTROL ----------
void sendOK(){ server.send(200,"application/json","{\"ok\":true}"); }
void handleStartWifi(){ wifiScanning=true; if(!wifiTaskHandle) xTaskCreatePinnedToCore(wifiScanTask,"wifiScanTask",4096,nullptr,1,&wifiTaskHandle,1); sendOK(); }
void handleStopWifi(){ wifiScanning=false; sendOK(); }
void handleClearWifi(){ portENTER_CRITICAL(&resultsMux); wifiResults.clear(); portEXIT_CRITICAL(&resultsMux); sendOK(); }
void handleStartBle(){  bleScanning=true; if(!bleTaskHandle)  xTaskCreatePinnedToCore(bleScanTask,"bleScanTask",8192,nullptr,1,&bleTaskHandle,1);   sendOK(); }
void handleStopBle(){  bleScanning=false; sendOK(); }
void handleClearBle(){ portENTER_CRITICAL(&resultsMux); bleResults.clear();  portEXIT_CRITICAL(&resultsMux); sendOK(); }

void handleStatus(){
  DynamicJsonDocument doc(4096);

  String st="idle";
  if(wifiScanning||bleScanning){
    if(wifiScanning&&bleScanning) st="scanning (wifi+bt)";
    else if(wifiScanning) st="scanning (wifi)";
    else st="scanning (bt)";
  }
  doc["status"]=st;
  doc["wifi_active"]=wifiScanning;   // <-- for radar
  doc["bt_active"]=bleScanning;      // <-- for radar

  portENTER_CRITICAL(&resultsMux);
  doc["wifi_count"]=(int)wifiResults.size();
  JsonArray wa=doc.createNestedArray("wifi");
  for(size_t i=0;i<wifiResults.size() && i<300;++i){
    JsonObject o=wa.createNestedObject();
    o["ssid"]=wifiResults[i].ssid; o["bssid"]=wifiResults[i].bssid;
    o["channel"]=wifiResults[i].channel; o["rssi"]=wifiResults[i].rssi; o["enc"]=wifiResults[i].enc;
  }
  doc["bt_count"]=(int)bleResults.size();
  JsonArray ba=doc.createNestedArray("ble");
  for(size_t i=0;i<bleResults.size() && i<300;++i){
    JsonObject o=ba.createNestedObject();
    o["addr"]=bleResults[i].addr; o["name"]=bleResults[i].name; o["rssi"]=bleResults[i].rssi;
  }
  portEXIT_CRITICAL(&resultsMux);

  String out; serializeJson(doc,out);
  server.send(200,"application/json",out);
}

// ---------- SETUP / LOOP ----------
void setup(){
  Serial.begin(115200); delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP,apGateway,apNetmask);
  WiFi.softAP(apSSID,apPass,1,false);

  server.on("/",HTTP_GET,handleIndex);
  server.on("/export",HTTP_GET,handleExport);
  server.on("/status",HTTP_GET,handleStatus);

  // Serve skull from PROGMEM (chunked)
  server.on("/skull.png",HTTP_GET,[](){
    server.sendHeader("Cache-Control","public, max-age=86400");
    server.setContentLength(SKULL_PNG_LEN);
    server.send(200,"image/png","");
    const uint8_t* p=SKULL_PNG; size_t left=SKULL_PNG_LEN; const size_t CH=1024; uint8_t buf[CH];
    while(left){ size_t n=left>CH?CH:left; memcpy_P(buf,p,n); server.sendContent((const char*)buf,n); p+=n; left-=n; }
  });

  server.on("/startWifi",HTTP_GET,handleStartWifi);
  server.on("/stopWifi", HTTP_GET,handleStopWifi);
  server.on("/clearWifi",HTTP_GET,handleClearWifi);
  server.on("/startBle", HTTP_GET,handleStartBle);
  server.on("/stopBle",  HTTP_GET,handleStopBle);
  server.on("/clearBle", HTTP_GET,handleClearBle);

  server.onNotFound([](){ server.send(404,"text/plain","Not found"); });
  server.begin();

  if(!wifiTaskHandle) xTaskCreatePinnedToCore(wifiScanTask,"wifiScanTask",4096,nullptr,1,&wifiTaskHandle,1);
  if(!bleTaskHandle)  xTaskCreatePinnedToCore(bleScanTask, "bleScanTask", 8192,nullptr,1,&bleTaskHandle,1);

  Serial.println("AP up at 192.168.4.1; UI ready.");
}
void loop(){ server.handleClient(); delay(2); }
