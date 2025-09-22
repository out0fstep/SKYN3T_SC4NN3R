/*
  Skyn3t Sc4nn3r - Complete sketch
  - WiFi scanning
  - BLE scanning
  - Hosts a small web UI on a private AP (headless mode)
  - Configure via top-of-file defines

  Notes:
  - Tested conceptually against ESP32 Arduino core APIs.
  - For BLE scanning this sketch uses the built-in BLE APIs (BLEDevice).
  - If you add a display (TFT/OLED), set ENABLE_DISPLAY true and add your display init + draw code in the placeholders.
  - Keep this for educational and authorized testing only.
*/

#include <WiFi.h>
#include <WebServer.h>
#include "BLEDevice.h"

// ---------------------- CONFIG ----------------------
#define AP_SSID        "Skyn3t-AP"
#define AP_PASSWORD    "skyn3tpass"   // make stronger in real use
#define WIFI_SCAN_INTERVAL_MS  15000  // every 15s
#define BLE_SCAN_INTERVAL_MS   20000  // every 20s
#define BLE_SCAN_DURATION_SEC  6      // how long each BLE scan runs (sec)
#define ENABLE_DISPLAY false          // set true if you implement display code
#define SERIAL_BAUD 115200
// ----------------------------------------------------

// Global web server
WebServer server(80);

unsigned long lastWifiScanMs = 0;
unsigned long lastBleScanMs  = 0;

// Structures to hold scan results
struct WiFiResult {
  String ssid;
  int rssi;
  int channel;
  bool encrypted;
};

struct BleResult {
  String addr;
  String name;
  int rssi;
};

// Circular buffers / vectors for last results
std::vector<WiFiResult> lastWifiResults;
std::vector<BleResult>  lastBleResults;

// Forward declarations
void startAP();
void setupWebServer();
void handleRoot();
void handleJsonWifi();
void handleJsonBle();
void doWifiScan();
void doBleScan();
String htmlHeader(const String &title);
String htmlFooter();
void safeTrimResults(); // keep results list limited

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);
  Serial.println();
  Serial.println("=== Skyn3t Sc4nn3r starting ===");

  // Init BLE (required before doing scans)
  BLEDevice::init("");

  // Optional: initialize display here (placeholder)
  if (ENABLE_DISPLAY) {
    // initDisplay();
    // drawBootScreen();
  }

  // Start the headless access point
  startAP();

  // Setup web server routes
  setupWebServer();

  // Kick off initial scans immediately
  lastWifiScanMs = 0;
  lastBleScanMs  = 0;
}

void loop() {
  // handle web server
  server.handleClient();

  unsigned long now = millis();
  if (now - lastWifiScanMs >= WIFI_SCAN_INTERVAL_MS) {
    lastWifiScanMs = now;
    doWifiScan();
  }

  if (now - lastBleScanMs >= BLE_SCAN_INTERVAL_MS) {
    lastBleScanMs = now;
    doBleScan();
  }

  // small sleep to yield
  delay(10);
}

// ------------------ AP & Web Server ------------------

void startAP() {
  Serial.printf("Starting softAP: %s\n", AP_SSID);
  bool ok;
  if (strlen(AP_PASSWORD) == 0) {
    ok = WiFi.softAP(AP_SSID); // open AP
  } else {
    ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
  }

  if (!ok) {
    Serial.println("Failed to start AP!");
  }

  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/wifi.json", handleJsonWifi);
  server.on("/ble.json", handleJsonBle);

  // Simple endpoint to trigger on-demand scans
  server.on("/scan/wifi", []() {
    doWifiScan();
    server.send(200, "text/plain", "WiFi scan started");
  });

  server.on("/scan/ble", []() {
    doBleScan();
    server.send(200, "text/plain", "BLE scan started");
  });

  server.begin();
  Serial.println("Web server started on port 80");
}

void handleRoot() {
  String html = htmlHeader("Skyn3t Sc4nn3r");
  html += "<h2>Skyn3t Sc4nn3r</h2>\n";
  html += "<p>Headless AP: <b>" + String(AP_SSID) + "</b></p>\n";
  html += "<p>Use the endpoints: <code>/wifi.json</code> and <code>/ble.json</code></p>\n";
  html += "<p><a href=\"/scan/wifi\">Trigger WiFi Scan</a> · <a href=\"/scan/ble\">Trigger BLE Scan</a></p>\n";

  // Small live rendering of results
  html += "<h3>Last Wi-Fi Results</h3>\n<ul>";
  for (auto &r : lastWifiResults) {
    html += "<li>" + r.ssid + " &middot; RSSI: " + String(r.rssi) + " dBm &middot; Ch:" + String(r.channel);
    if (r.encrypted) html += " &middot; 🔒";
    html += "</li>\n";
  }
  html += "</ul>\n";

  html += "<h3>Last BLE Results</h3>\n<ul>";
  for (auto &b : lastBleResults) {
    html += "<li>" + b.addr + " - " + (b.name.length() ? b.name : "—") + " &middot; RSSI:" + String(b.rssi) + " dBm</li>\n";
  }
  html += "</ul>\n";

  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleJsonWifi() {
  // Build JSON
  String json = "[";
  for (size_t i = 0; i < lastWifiResults.size(); ++i) {
    const WiFiResult &r = lastWifiResults[i];
    json += "{";
    json += "\"ssid\":\"" + r.ssid + "\",";
    json += "\"rssi\":" + String(r.rssi) + ",";
    json += "\"channel\":" + String(r.channel) + ",";
    json += "\"encrypted\":" + String(r.encrypted ? "true" : "false");
    json += "}";
    if (i + 1 < lastWifiResults.size()) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleJsonBle() {
  String json = "[";
  for (size_t i = 0; i < lastBleResults.size(); ++i) {
    const BleResult &b = lastBleResults[i];
    json += "{";
    json += "\"addr\":\"" + b.addr + "\",";
    json += "\"name\":\"" + b.name + "\",";
    json += "\"rssi\":" + String(b.rssi);
    json += "}";
    if (i + 1 < lastBleResults.size()) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// ------------------ Scanning ------------------

void doWifiScan() {
  Serial.println("Starting WiFi scan (blocking) ...");
  int n = WiFi.scanNetworks(false, true); // show_hidden=false, passive=false? (api variations exist)
  Serial.printf("Found %d networks\n", n);

  lastWifiResults.clear();
  for (int i = 0; i < n; ++i) {
    WiFiResult r;
    r.ssid = WiFi.SSID(i);
    r.rssi = WiFi.RSSI(i);
    r.channel = WiFi.channel(i);
    r.encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    lastWifiResults.push_back(r);

    Serial.printf("  %d: %s  ch:%d  rssi:%d  enc:%d\n", i, r.ssid.c_str(), r.channel, r.rssi, r.encrypted);
    // keep results to reasonable number
    if (lastWifiResults.size() >= 60) break;
  }

  // Optionally update display here
  if (ENABLE_DISPLAY) {
    // drawWifiResults(lastWifiResults);
  }
  safeTrimResults();
}

void doBleScan() {
  Serial.println("Starting BLE scan ...");
  BLEScan *pBLEScan = BLEDevice::getScan();
  // configure
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100); // ms
  pBLEScan->setWindow(99);    // ms

  // perform scan (blocking)
  BLEScanResults results = pBLEScan->start(BLE_SCAN_DURATION_SEC, false);
  int count = results.getCount();
  Serial.printf("BLE found %d devices\n", count);

  lastBleResults.clear();
  for (int i = 0; i < count; ++i) {
    BLEAdvertisedDevice dev = results.getDevice(i);
    BleResult b;
    b.addr = dev.getAddress().toString().c_str();
    b.name = dev.getName().c_str();
    b.rssi = dev.getRSSI();
    lastBleResults.push_back(b);

    Serial.printf("  %d: %s (%s) rssi:%d\n", i, b.addr.c_str(), b.name.c_str(), b.rssi);
    if (lastBleResults.size() >= 80) break;
  }

  // stop scanning (it already stops when start returns, but ensure)
  pBLEScan->clearResults(); // free memory used by results

  if (ENABLE_DISPLAY) {
    // drawBleResults(lastBleResults);
  }
  safeTrimResults();
}

// ------------------ Utilities ------------------

void safeTrimResults() {
  // limit to N entries to keep responses small
  const size_t MAX_WIFI = 60;
  const size_t MAX_BLE  = 80;
  while (lastWifiResults.size() > MAX_WIFI) lastWifiResults.erase(lastWifiResults.begin());
  while (lastBleResults.size() > MAX_BLE) lastBleResults.erase(lastBleResults.begin());
}

String htmlHeader(const String &title) {
  String s = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>" + title + "</title>";
  s += "<style>body{font-family:Helvetica,Arial,sans-serif;background:#0f1113;color:#d0d6db;margin:12px}a{color:#58a6ff}h2{margin-bottom:4px}ul{line-height:1.4}</style>";
  s += "</head><body>";
  s += "<div style='text-align:center;'>";
  s += "<h1>☠️ Skyn3t Sc4nn3r ☠️</h1>";
  s += "</div><hr/>";
  return s;
}

String htmlFooter() {
  String s;
  s += "<hr/><p style='font-size:0.85em;color:#9aa4ad'>Skyn3t Sc4nn3r - headless AP mode. For authorized testing only.</p>";
  s += "</body></html>";
  return s;
}

/* End of sketch */
