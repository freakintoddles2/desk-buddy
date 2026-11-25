#include "net.h"
#include "weather.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include "clock.h"
#include "tz.h"
#include "store.h"

namespace net {
namespace {

constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
// After "could not connect": join again after 30 s, 1, 2, then every 5 minutes. The system's
// own reconnect gives up for good on some disconnects (router reboot, access point dropping
// idle devices), which would otherwise leave the device offline until it's unplugged.
constexpr uint32_t RETRY_MS[] = {30000, 60000, 120000, 300000};
uint32_t failedAt = 0;
int retries = 0;

WebServer server(80);
DNSServer dns;
bool setupOn = false;
bool serverStarted = false;
Status st = Status::Off;
uint32_t connectStarted = 0;
volatile bool synced = false;
bool savedFlag = false, savedWifi = false;

String esc(const String &s) {
  String o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

void startNtp() {
  if (!autoTime()) return;
  sntp_set_time_sync_notification_cb([](struct timeval *) {
    synced = true;
    clockx::markTrusted();
    Serial.println("[net] network time set");
  });
  configTzTime(tz::devicePosix(), "pool.ntp.org", "time.nist.gov");
}

void connect() {
  String s = store::getStr("ssid"), p = store::getStr("wifipw");
  if (!s.length()) { st = Status::Off; return; }
  WiFi.setAutoReconnect(true);
  WiFi.begin(s.c_str(), p.c_str());
  st = Status::Connecting;
  connectStarted = millis();
  Serial.printf("[net] connecting to %s\n", s.c_str());
}

// ---------- the setup page ----------

const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Desk buddy setup</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--text:#1d2330;--muted:#667085;--border:#d9dde5;--field:#f6f7f9;--accent:#3f7ae8}
@media (prefers-color-scheme:dark){:root{--bg:#101216;--card:#1a1d23;--text:#e6e9ef;--muted:#98a2b3;--border:#2c313b;--field:#13161b;--accent:#5b93ff}}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 -apple-system,system-ui,sans-serif}
main{max-width:420px;margin:0 auto;padding:18px 16px 32px}
.card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:16px}
h1{font-size:19px;margin:0 0 4px}.note{font-size:13px;color:var(--muted);margin-top:6px}
.sec{margin-top:18px}.sec-t{font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);margin-bottom:6px}
label.net{display:flex;align-items:center;gap:10px;padding:9px 8px;border-radius:9px}
label.net:active{background:var(--field)}.sig{margin-left:auto;color:var(--muted);font:12px ui-monospace,Menlo,monospace}
label.f{display:block;font-size:13px;color:var(--muted);margin-top:10px}
input[type=text],input[type=password],input[type=date],input[type=time],input[type=url],select{display:block;box-sizing:border-box;width:100%;margin-top:4px;padding:10px;border-radius:9px;border:1px solid var(--border);background:var(--field);color:var(--text);font:inherit}
button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:10px;background:var(--accent);color:#fff;font:600 16px -apple-system,system-ui,sans-serif}
a{color:var(--accent)}
</style></head><body><main><div class="card">)HTML";

const char PAGE_TAIL[] PROGMEM = R"HTML(</div></main><script>
const q=s=>document.querySelector(s);
function sync(){const n=(q('input[name=net]:checked')||{}).value||'';const other=n==='__other';
q('#other').style.display=other?'':'none';q('#pw').style.display=n?'':'none';
q('#time').style.display=n?'none':'';q('#wx').style.display=n?'':'none';}
document.addEventListener('change',sync);sync();
try{const z=Intl.DateTimeFormat().resolvedOptions().timeZone,o=[...q('#tz').options].find(o=>o.dataset.z===z);if(o)o.selected=true;}catch(e){}
</script></body></html>)HTML";

void sendPage() {
  String cur = store::getStr("ssid");
  String h;
  h.reserve(6000);
  h += FPSTR(PAGE_HEAD);
  h += "<h1>Desk buddy setup</h1><div class=note>Everything here is optional. Without Wi-Fi the device runs offline and you set the clock by hand.</div>";
  h += "<form method=post action=/save><div class=sec><div class=sec-t>Wi-Fi</div>";
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    h += "<div class=note>Looking for networks&hellip; <a href=/>Refresh</a></div>";
  } else {
    // Strongest first, one row per network name.
    std::vector<int> idx;
    for (int i = 0; i < max(0, n); i++) {
      bool dup = false;
      for (int j : idx) dup |= WiFi.SSID(j) == WiFi.SSID(i);
      if (!dup && WiFi.SSID(i).length()) idx.push_back(i);
    }
    std::sort(idx.begin(), idx.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
    for (int i : idx) {
      int r = WiFi.RSSI(i);
      const char *bars = r > -55 ? "&#9602;&#9604;&#9606;&#9608;" : r > -67 ? "&#9602;&#9604;&#9606;_" : r > -78 ? "&#9602;&#9604;__" : "&#9602;___";
      String s = esc(WiFi.SSID(i));
      h += "<label class=net><input type=radio name=net value=\"" + s + "\"" + (WiFi.SSID(i) == cur ? " checked" : "") +
           "> " + s + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " (open)" : "") + "<span class=sig>" + bars + "</span></label>";
    }
    h += "<div class=note><a href=/rescan>Scan again</a></div>";
  }
  h += "<label class=net><input type=radio name=net value=__other> Other network&hellip;</label>";
  h += "<label class=net><input type=radio name=net value=\"\"" + String(cur.length() ? "" : " checked") + "> No Wi-Fi</label>";
  h += "<div id=other><label class=f>Network name<input type=text name=othername autocapitalize=off autocorrect=off></label></div>";
  h += "<div id=pw><label class=f>Password<input type=password name=pw autocomplete=off></label>"
       "<div class=note>2.4 GHz networks only. Leave the password empty to keep the saved one.</div></div></div>";
  struct tm t = clockx::local();
  char d[16], tm_[8];
  strftime(d, sizeof(d), "%Y-%m-%d", &t);
  strftime(tm_, sizeof(tm_), "%H:%M", &t);
  h += String("<div class=sec id=time><div class=sec-t>Date and time</div><input type=date name=date value=") + d +
       "><input type=time name=time value=" + tm_ + ">";
  // The zone, by city: preselected from the phone's own zone when it has a preset.
  h += "<label class=f>Time zone<select name=tz id=tz>";
  if (tz::hasLocationZone())  // the weather location's own time (no preset for it)
    h += String("<option value=-1") + (tz::deviceCity() < 0 ? " selected" : "") + ">" + esc(tz::locationZoneName()) + "</option>";
  for (int i = 0; i < tz::CITY_COUNT; i++)
    h += String("<option value=") + i + " data-z=\"" + tz::CITIES[i].iana + "\"" + (i == tz::deviceCity() ? " selected" : "") +
         ">" + esc(tz::CITIES[i].name) + "</option>";
  h += "</select></label><div class=note>Pick the city that keeps the same time as you.</div></div>";
  h += "<div class=sec id=wx><div class=sec-t>Weather</div><label class=f>City<input type=text name=loc placeholder=\"e.g. Wellington, NZ\" value=\"" +
       esc(weather::locationLabel()) + "\"></label>"
       "<div class=note>City and state, province or country, e.g. Phoenix, AZ or Wellington, NZ or Toronto, ON. "
       "A US ZIP works too. The time zone and clock style follow the city. You can change it later on the device in "
       "Settings &gt; Weather.</div></div>";
  h += "<button type=submit>Save and finish</button></form>";
  h += FPSTR(PAGE_TAIL);
  server.send(200, "text/html", h);
}

void handleSave() {
  String net = server.arg("net");
  if (net == "__other") net = server.arg("othername");
  net.trim();
  String msg;
  if (net.length()) {
    if (net != store::getStr("ssid") || server.arg("pw").length()) store::setStr("wifipw", server.arg("pw"));
    store::setStr("ssid", net);
    // Unchanged (still the picked place's label): keep the picked coordinates.
    if (server.arg("loc").length() && server.arg("loc") != weather::locationLabel()) store::setStr("loc", server.arg("loc"));
    setAutoTime(true);
    msg = "Joining <b>" + esc(net) + "</b>. Check the desk buddy: it shows whether it connected. "
          "If it can't, this page comes back so you can fix the password.";
    savedWifi = true;
  } else {
    store::remove("ssid");
    store::remove("wifipw");
    if (server.arg("tz").length()) {  // before the clock: it's local time
      int z = server.arg("tz").toInt();
      if (z < 0) tz::useLocationZone();
      else tz::setDeviceCity(z);
    }
    int y, mo, dd, hh, mm;
    if (sscanf(server.arg("date").c_str(), "%d-%d-%d", &y, &mo, &dd) == 3 &&
        sscanf(server.arg("time").c_str(), "%d:%d", &hh, &mm) == 2)
      clockx::setLocal(y, mo, dd, hh, mm);
    msg = "Saved. The desk buddy runs offline. You can close this page.";
    savedWifi = false;
  }
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Saved</h1><div class=note>" + msg + "</div>" + FPSTR(PAGE_TAIL);
  server.send(200, "text/html", h);
  savedFlag = true;
  Serial.printf("[net] setup saved: %s\n", net.length() ? net.c_str() : "(offline)");
}

void startServer() {
  if (serverStarted) return;
  server.on("/", sendPage);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/rescan", []() {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  // Captive portal: any other URL (phones probe /generate_204, /hotspot-detect.html, ...)
  // redirects to the setup page, which makes the phone open it by itself.
  server.onNotFound([]() {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
  serverStarted = true;
}

}  // namespace

void begin() {
  // Home Assistant was dropped (2026-11-09): clear an address/token saved by older firmware.
  store::remove("haurl");
  store::remove("hatok");
  WiFi.persistent(false);  // credentials live in our own NVS keys
  if (configured()) {
    WiFi.mode(WIFI_STA);
    connect();
    startNtp();
  } else {
    WiFi.mode(WIFI_OFF);
  }
}

void tick() {
  if (setupOn) {
    dns.processNextRequest();
    server.handleClient();
  }
  if (st == Status::Connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      st = Status::Connected;
      retries = 0;
      Serial.printf("[net] connected, %s\n", WiFi.localIP().toString().c_str());
      startNtp();
    } else if (millis() - connectStarted > CONNECT_TIMEOUT_MS) {
      st = Status::Failed;
      failedAt = millis();
      Serial.println("[net] could not connect");
    }
  } else if (st == Status::Connected && WiFi.status() != WL_CONNECTED) {
    st = Status::Connecting;  // auto-reconnect is on; show it as reconnecting
    connectStarted = millis();
  } else if (st == Status::Failed && WiFi.status() == WL_CONNECTED) {
    st = Status::Connected;
    retries = 0;
  } else if (st == Status::Failed && !setupOn && configured() &&
             millis() - failedAt >= RETRY_MS[min(retries, 3)]) {
    retries++;
    Serial.printf("[net] trying again (%d)\n", retries);
    WiFi.disconnect(false);
    connect();
  }
}

bool configured() { return store::wifi(); }
String ssid() { return store::getStr("ssid"); }
Status status() { return st; }
bool timeSynced() { return synced; }
bool autoTime() { return store::getInt("autotime", 1) != 0; }

void setAutoTime(bool on) {
  store::setInt("autotime", on ? 1 : 0);
  if (on && st == Status::Connected) startNtp();
  if (!on) esp_sntp_stop();
}

void testDrop() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  Serial.println("[net] test: dropped, auto-reconnect off");
}

void forget() {
  store::remove("ssid");
  store::remove("wifipw");
  WiFi.disconnect(true);
  WiFi.mode(setupOn ? WIFI_AP : WIFI_OFF);
  st = Status::Off;
  Serial.println("[net] forgot Wi-Fi");
}

String apName() {
  uint64_t mac = ESP.getEfuseMac();
  char b[24];
  snprintf(b, sizeof(b), "DeskBuddy-%02X%02X", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  return b;
}

void startSetup() {
  if (setupOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName().c_str());
  delay(100);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  startServer();
  WiFi.scanNetworks(true);
  setupOn = true;
  savedFlag = false;
  Serial.printf("[net] setup mode: join %s, page at %s\n", apName().c_str(), WiFi.softAPIP().toString().c_str());
}

void stopSetup() {
  if (!setupOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(configured() ? WIFI_STA : WIFI_OFF);
  setupOn = false;
  Serial.println("[net] setup mode off");
}

bool inSetup() { return setupOn; }

bool takeSaved(bool &wifi) {
  if (!savedFlag) return false;
  savedFlag = false;
  wifi = savedWifi;
  if (wifi) connect();
  return true;
}

String location() { return store::getStr("loc"); }  // "" until one is set
void setLocation(const String &loc) { store::setStr("loc", loc); }

}  // namespace net
