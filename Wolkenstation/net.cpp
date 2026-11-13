// Wolkenstation – WLAN, Einrichtung übers Handy (eigenes WLAN + Webseite), Uhrzeit, Handy-Nachrichten (ntfy).
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_wifi.h>
#include <time.h>
#include "app.h"
#include "platform.h"
#include "ca_certs.h"

static const char* TZ_DE = "CET-1CEST,M3.5.0,M10.5.0/3";

// ------------------------------------------------------------------ Zustand WLAN
static char credSsid[33] = "", credPass[65] = "";      // gespeichert
static char trySsid[33] = "", tryPass[65] = "";        // aktueller Versuch
static volatile NetState state = NET_IDLE;
static bool viaPortal = false;
static bool tryingNew = false;                          // neue Zugangsdaten (noch nicht gespeichert)
static uint32_t connectStart = 0;
static uint8_t attempt = 0, failPass = 0, failNotFound = 0;
static uint32_t nextRetryMs = 0;
static uint8_t retryCount = 0;
static uint8_t lastChannel = 0;
static char ipStr[16] = "-";

static volatile bool evGotIp = false, evDisc = false;
static volatile uint8_t evReason = 0;

static ScanNet nets[16];
static int nNets = 0;
static bool scanning = false;

// Einrichtungs-WLAN
static WebServer* server = nullptr;
static DNSServer* dns = nullptr;
static bool portalOn = false;
static char apSsid[33] = "", apPass[16] = "";
static uint32_t portalStopAt = 0;

// ------------------------------------------------------------------ Zeit
bool timeValid() { return time(nullptr) > 1700000000; }
uint32_t epochNow() { return (uint32_t)time(nullptr); }
static bool localNow(struct tm& tm) {
  if (!timeValid()) return false;
  time_t t = time(nullptr);
  localtime_r(&t, &tm);
  return true;
}
int minuteNow() { struct tm tm; if (!localNow(tm)) return -1; return tm.tm_hour * 60 + tm.tm_min; }
uint32_t dayKeyNow() { struct tm tm; if (!localNow(tm)) return 0; return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday; }
int weekdayNow() { struct tm tm; if (!localNow(tm)) return -1; return tm.tm_wday; }
uint32_t msNow() { return millis(); }
uint32_t randomU32() { return esp_random(); }

// ------------------------------------------------------------------ Abfragen
NetState netState() { return state; }
bool netOnline() { return state == NET_CONNECTED; }
const char* netSsid() { return credSsid[0] ? credSsid : trySsid; }
const char* netTrySsid() { return trySsid; }
int netRssi() { return WiFi.RSSI(); }
bool netHasCreds() { return credSsid[0] != 0; }
bool netConnectViaPortal() { return viaPortal; }
const char* netApSsid() { return apSsid; }
const char* netApPass() { return apPass; }
const char* netIp() { return ipStr; }
bool netPortalActive() { return portalOn; }
bool netPortalClient() { return portalOn && WiFi.softAPgetStationNum() > 0; }
bool netScanBusy() { return scanning; }
int netScanCount() { return nNets; }
const ScanNet& netScanGet(int i) { return nets[i < 0 ? 0 : (i > 15 ? 15 : i)]; }
uint8_t netChannel() {
  uint8_t ch = 1; wifi_second_chan_t sc;
  if (esp_wifi_get_channel(&ch, &sc) != ESP_OK) ch = 1;
  return ch;
}

// ------------------------------------------------------------------ Verbinden
static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: evGotIp = true; break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: evReason = info.wifi_sta_disconnected.reason; evDisc = true; break;
    default: break;
  }
}

static void beginWith(const char* ssid, const char* pass, uint8_t channel) {
  WiFi.disconnect(false, false);
  delay(50);
  if (channel) WiFi.begin(ssid, pass[0] ? pass : nullptr, channel);
  else WiFi.begin(ssid, pass[0] ? pass : nullptr);
}

static void startConnect(const char* ssid, const char* pass, bool portal) {
  if (scanning) { WiFi.scanDelete(); scanning = false; }
  strncpy(trySsid, ssid, sizeof(trySsid) - 1); trySsid[sizeof(trySsid) - 1] = 0;
  strncpy(tryPass, pass, sizeof(tryPass) - 1); tryPass[sizeof(tryPass) - 1] = 0;
  viaPortal = portal;
  tryingNew = true;
  attempt = 1; failPass = 0; failNotFound = 0;
  connectStart = millis();
  state = NET_CONNECTING;
  Serial.printf("[WLAN] verbinde mit \"%s\" (%s)\n", trySsid, portal ? "Handy" : "Bildschirm");
  beginWith(trySsid, tryPass, 0);
}

void netConnect(const char* ssid, const char* pass) { startConnect(ssid, pass, false); }

static void reconnectSaved(bool fullScan) {
  if (!credSsid[0]) return;
  strncpy(trySsid, credSsid, sizeof(trySsid));
  strncpy(tryPass, credPass, sizeof(tryPass));
  tryingNew = false;
  Serial.printf("[WLAN] neuer Versuch mit \"%s\"%s\n", credSsid, fullScan ? " (alle Kanäle)" : "");
  beginWith(credSsid, credPass, fullScan ? 0 : lastChannel);
}

static bool isPassReason(uint8_t r) {
  return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT ||
         r == WIFI_REASON_AUTH_EXPIRE || r == WIFI_REASON_MIC_FAILURE || r == WIFI_REASON_802_1X_AUTH_FAILED;
}

// ------------------------------------------------------------------ Suche nach Netzen
void netScanStart() {
  if (state == NET_CONNECTING || scanning) return;
  WiFi.scanDelete();
  if (WiFi.scanNetworks(true, false, false, 220) == WIFI_SCAN_FAILED) { scanning = false; return; }
  scanning = true;
}

static void scanCollect() {
  int16_t r = WiFi.scanComplete();
  if (r == WIFI_SCAN_RUNNING) return;
  scanning = false;
  nNets = 0;
  if (r < 0) return;
  for (int i = 0; i < r && nNets < 16; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0 || s.length() > 32) continue;
    int dup = -1;
    for (int k = 0; k < nNets; k++) if (strcmp(nets[k].ssid, s.c_str()) == 0) { dup = k; break; }
    int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (dup >= 0) { if (rssi > nets[dup].rssi) nets[dup].rssi = rssi; continue; }
    strncpy(nets[nNets].ssid, s.c_str(), 32); nets[nNets].ssid[32] = 0;
    nets[nNets].rssi = rssi;
    nets[nNets].lock = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    nNets++;
  }
  for (int i = 1; i < nNets; i++)            // nach Signalstärke sortieren
    for (int k = i; k > 0 && nets[k].rssi > nets[k - 1].rssi; k--) { ScanNet t = nets[k]; nets[k] = nets[k - 1]; nets[k - 1] = t; }
  WiFi.scanDelete();
}

// ------------------------------------------------------------------ Einrichtungs-Webseite
static String htmlEsc(const char* s) {
  String o;
  for (; *s; s++) {
    switch (*s) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += *s;
    }
  }
  return o;
}

static const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html lang=de><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Wolkenstation einrichten</title><style>"
  "body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#f1eff6;color:#2b2540}"
  "main{max-width:440px;margin:0 auto;padding:18px 16px 40px}"
  "h1{font-size:22px;margin:6px 0 4px}p{line-height:1.45;margin:6px 0}"
  ".card{background:#fff;border-radius:14px;padding:12px 14px;margin:12px 0}"
  "label.net{display:flex;align-items:center;gap:10px;padding:11px 4px;border-bottom:1px solid #eee9f6;font-size:16px}"
  "label.net:last-child{border-bottom:0}label.net span{margin-left:auto;color:#6b57c9;font-size:13px}"
  "input[type=radio]{width:20px;height:20px;accent-color:#6b57c9}"
  ".f{display:block;font-weight:700;font-size:14px;margin:12px 0 4px}"
  "input[type=text],input[type=password]{width:100%;box-sizing:border-box;font-size:17px;padding:12px;border:2px solid #dfdaea;border-radius:10px}"
  ".chk{display:flex;gap:8px;align-items:center;margin:8px 0;font-size:15px}"
  "button,.btn{display:block;width:100%;box-sizing:border-box;font-size:17px;font-weight:800;border:0;border-radius:12px;padding:14px;"
  "background:#6b57c9;color:#fff;margin-top:16px;text-align:center;text-decoration:none}"
  ".sec{background:#e9e4f5;color:#3f3555}.err{background:#fde3e3;color:#9a2a2a;border-radius:10px;padding:10px 12px;font-weight:700}"
  ".ok{background:#e3f6ee;color:#1d6a4c;border-radius:10px;padding:12px;font-weight:700}.small{font-size:13px;color:#665f7d}"
  ".spin{width:34px;height:34px;border:4px solid #dfdaea;border-top-color:#6b57c9;border-radius:50%;animation:s 1s linear infinite;margin:18px auto}"
  "@keyframes s{to{transform:rotate(360deg)}}</style></head><body><main>";
static const char PAGE_TAIL[] PROGMEM = "</main></body></html>";

static void sendPageOn(WebServer* srv, const String& body) {
  String p = FPSTR(PAGE_HEAD);
  p += body;
  p += FPSTR(PAGE_TAIL);
  srv->sendHeader("Cache-Control", "no-store");
  srv->send(200, "text/html; charset=utf-8", p);
}
static void sendPage(const String& body) { sendPageOn(server, body); }

static void handleRoot() {
  String err = server->hasArg("e") ? server->arg("e") : String("");
  String b;
  b += "<h1>Wolkenstation einrichten</h1><p>Wähle das WLAN, mit dem sich die Station verbinden soll.</p>";
  if (err == "pw") b += "<p class=err>Das Passwort hat nicht gepasst. Bitte nochmal eingeben – auf Groß- und Kleinschreibung achten.</p>";
  else if (err == "nf") b += "<p class=err>Dieses WLAN wurde nicht gefunden. Ist der Router an und nah genug?</p>";
  else if (err == "x") b += "<p class=err>Die Verbindung hat nicht geklappt. Bitte nochmal versuchen.</p>";
  else if (err == "len") b += "<p class=err>Ein WLAN-Passwort hat mindestens 8 Zeichen.</p>";
  else if (err == "sel") b += "<p class=err>Bitte zuerst ein WLAN auswählen.</p>";
  b += "<form method=post action=/save><div class=card>";
  if (scanning && nNets == 0) b += "<div class=spin></div><p class=small style='text-align:center'>Suche WLAN-Netze …</p>";
  for (int i = 0; i < nNets; i++) {
    int bars = nets[i].rssi > -55 ? 4 : nets[i].rssi > -67 ? 3 : nets[i].rssi > -77 ? 2 : 1;
    b += "<label class=net><input type=radio name=ssid value=\"";
    b += htmlEsc(nets[i].ssid);
    b += "\"";
    bool sel = trySsid[0] ? strcmp(nets[i].ssid, trySsid) == 0 : i == 0;
    if (sel) b += " checked";
    b += ">";
    b += htmlEsc(nets[i].ssid);
    b += "<span>";
    if (nets[i].lock) b += "&#128274; ";
    for (int k = 0; k < 4; k++) b += k < bars ? "&#9646;" : "&#9647;";
    b += "</span></label>";
  }
  if (!scanning && nNets == 0) b += "<p>Keine WLAN-Netze gefunden.</p>";
  b += "</div><p class=small>Nur 2,4-GHz-Netze werden angezeigt. <a href=/rescan>Neu suchen</a></p>";
  b += "<label class=f for=pw>WLAN-Passwort</label><input type=password id=pw name=pass autocomplete=off placeholder=Passwort>";
  b += "<label class=chk><input type=checkbox onclick=\"pw.type=this.checked?'text':'password'\"> Passwort anzeigen</label>";
  b += "<label class=f for=nm>Name des Kindes (optional)</label><input type=text id=nm name=name maxlength=16 placeholder='z. B. Mia' value=\"";
  b += htmlEsc(S.name);
  b += "\"><button type=submit>Speichern und verbinden</button></form>";
  if (scanning && nNets == 0) b += "<script>setTimeout(function(){location.reload()},3000)</script>";
  sendPage(b);
}

static void redirectTo(const char* path) {
  String url = String("http://192.168.4.1") + path;
  server->sendHeader("Location", url, true);
  server->send(302, "text/plain", "");
}

static void handleRescan() {
  netScanStart();
  redirectTo("/");
}

static void handleSave() {
  String ssid = server->arg("ssid"), pass = server->arg("pass"), name = server->arg("name");
  if (ssid.length() == 0) { redirectTo("/?e=sel"); return; }
  bool lock = true;
  for (int i = 0; i < nNets; i++) if (ssid == nets[i].ssid) lock = nets[i].lock;
  if (lock && pass.length() < 8) { redirectTo("/?e=len"); return; }
  name.trim();
  if (name.length() <= 48) {
    strncpy(S.name, name.c_str(), sizeof(S.name) - 1); S.name[sizeof(S.name) - 1] = 0;
    storeSaveSettings();
  }
  String b = "<h1>Einen Moment …</h1><div class=spin></div>"
             "<p id=m>Die Station verbindet sich jetzt mit „" + htmlEsc(ssid.c_str()) + "“. Das Ergebnis siehst du gleich hier und auf der Station.</p>"
             "<script>var n=0;function q(){fetch('/status').then(function(r){return r.json()}).then(function(j){"
             "if(j.s=='ok'){document.querySelector('main').innerHTML='<h1>Fertig!</h1><p class=ok>Die Station ist mit dem WLAN verbunden. Weiter geht’s an der Station.</p><p class=small>Du kannst dieses Fenster jetzt schließen. Das Einrichtungs-WLAN geht gleich aus.</p>';return}"
             "if(j.s=='pw'||j.s=='nf'||j.s=='x'){location.href='/?e='+j.s;return}setTimeout(q,1500)})"
             ".catch(function(){if(++n>4)document.getElementById('m').innerHTML='Die Verbindung zum Handy war kurz weg. Schau bitte auf die Station – dort steht, ob es geklappt hat.';setTimeout(q,2000)})}"
             "setTimeout(q,2500)</script>";
  sendPage(b);
  startConnect(ssid.c_str(), pass.c_str(), true);
}

static void handleStatus() {
  const char* s = "wait";
  if (state == NET_CONNECTED) s = "ok";
  else if (state == NET_ERR_PASS) s = "pw";
  else if (state == NET_ERR_NOTFOUND) s = "nf";
  else if (state == NET_ERR_OTHER) s = "x";
  String j = String("{\"s\":\"") + s + "\"}";
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/json", j);
}

static void handleCaptive() { redirectTo("/"); }

void netPortalStart() {
  if (portalOn) return;
  otaStop();                         // beide brauchen Port 80
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof(apSsid), "Wolkenstation-%02X%02X", mac[4], mac[5]);
  snprintf(apPass, sizeof(apPass), "wolke%04u", (unsigned)(esp_random() % 10000));
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid, apPass, netChannel(), 0, 2);
  dns = new DNSServer();
  dns->setErrorReplyCode(DNSReplyCode::NoError);
  dns->start(53, "*", IPAddress(192, 168, 4, 1));
  server = new WebServer(80);
  server->on("/", HTTP_GET, handleRoot);
  server->on("/rescan", HTTP_GET, handleRescan);
  server->on("/save", HTTP_POST, handleSave);
  server->on("/status", HTTP_GET, handleStatus);
  const char* captive[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                           "/connecttest.txt", "/ncsi.txt", "/redirect", "/fwlink", "/canonical.html", "/success.txt"};
  for (const char* u : captive) server->on(u, handleCaptive);
  server->onNotFound(handleCaptive);
  server->begin();
  portalOn = true;
  portalStopAt = 0;
  Serial.printf("[Einrichtung] WLAN \"%s\" Passwort \"%s\" -> http://192.168.4.1\n", apSsid, apPass);
  netScanStart();
}

void netPortalStop() {
  if (!portalOn) return;
  server->stop(); delete server; server = nullptr;
  dns->stop(); delete dns; dns = nullptr;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  portalOn = false;
  portalStopAt = 0;
  Serial.println("[Einrichtung] Einrichtungs-WLAN aus");
}

// ------------------------------------------------------------------ Handy-Nachrichten (ntfy.sh)
struct NtfyMsg {
  char title[96];
  char msg[192];
  char tags[24];
  char token[9];
  uint8_t prio;
  uint8_t kind;
  uint32_t created;
};
static NtfyMsg queue[8];
static int qHead = 0, qCount = 0;
static SemaphoreHandle_t qMtx;
static TaskHandle_t ntfyTaskHandle = nullptr;
static volatile int testResult = 0;
static volatile bool ackSeen = false;
static volatile bool ackPollOn = false;
static char ackToken[9] = "";
static uint32_t ackSince = 0;

int ntfyTestResult() { return testResult; }
void ntfyTestReset() { testResult = 0; }

void ntfySend(const char* title, const char* msg, uint8_t prio, NtfyKind kind, const char* tags) {
  if (!S.ntfyOn && kind != NTFY_TEST) { Serial.printf("[ntfy] aus: %s\n", title); return; }
  xSemaphoreTake(qMtx, portMAX_DELAY);
  if (qCount == 8) {                 // voll: älteste normale Nachricht verwerfen
    qHead = (qHead + 1) % 8; qCount--;
  }
  NtfyMsg& m = queue[(qHead + qCount) % 8];
  memset(&m, 0, sizeof(m));
  strncpy(m.title, title, sizeof(m.title) - 1);
  strncpy(m.msg, msg, sizeof(m.msg) - 1);
  strncpy(m.tags, tags ? tags : "", sizeof(m.tags) - 1);
  if (kind == NTFY_HELP) strncpy(m.token, H.token, sizeof(m.token) - 1);
  m.prio = prio;
  m.kind = kind;
  m.created = millis();
  qCount++;
  if (kind == NTFY_HELP) {
    strncpy(ackToken, H.token, sizeof(ackToken) - 1);
    ackSince = H.startEpoch;
    ackSeen = false;
    ackPollOn = true;
  }
  xSemaphoreGive(qMtx);
  Serial.printf("[ntfy] in Warteschlange: %s\n", title);
  if (ntfyTaskHandle) xTaskNotifyGive(ntfyTaskHandle);
}

static void jsonEsc(String& o, const char* s) {
  for (; *s; s++) {
    char ch = *s;
    if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
    else if ((uint8_t)ch < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", ch); o += b; }
    else o += ch;
  }
}

// HTTPS-Anfrage. Erst mit Zertifikatsprüfung; klappt der Aufbau gar nicht (z. B. Uhrzeit noch unbekannt
// oder neue Zertifikatskette), einmal ohne Prüfung – eine verpasste Hilferuf-Nachricht wäre schlimmer.
static int httpsRequest(const String& url, const String* body, String* resp) {
  Serial.printf("[ntfy] vor HTTPS: Speicher frei %u, größter Block %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  for (int pass = 0; pass < 2; pass++) {
    WiFiClientSecure tls;
    if (pass == 0) tls.setCACert(ROOT_CA_PEM); else tls.setInsecure();
    tls.setHandshakeTimeout(15);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    http.setReuse(false);
    if (!http.begin(tls, url)) continue;
    int code;
    if (body) {
      http.addHeader("Content-Type", "application/json");
      code = http.POST((uint8_t*)body->c_str(), body->length());
    } else {
      code = http.GET();
    }
    if (code > 0) {
      if (resp) *resp = http.getString();
      http.end();
      return code;
    }
    http.end();
    Serial.printf("[ntfy] Verbindung fehlgeschlagen (%d)%s\n", code, pass == 0 ? ", versuche ohne Zertifikatsprüfung" : "");
  }
  return -1;
}

static bool ntfyPost(const NtfyMsg& m) {
  String b = "{\"topic\":\"";
  jsonEsc(b, S.topic);
  b += "\",\"title\":\""; jsonEsc(b, m.title);
  b += "\",\"message\":\""; jsonEsc(b, m.msg);
  b += "\",\"priority\":"; b += String((int)m.prio);
  if (m.tags[0]) { b += ",\"tags\":[\""; jsonEsc(b, m.tags); b += "\"]"; }
  if (m.kind == NTFY_HELP) {
    b += ",\"actions\":[{\"action\":\"http\",\"label\":\"Ich komme\",\"url\":\"https://ntfy.sh/";
    jsonEsc(b, S.topic);
    b += "-ok\",\"method\":\"POST\",\"body\":\"";
    jsonEsc(b, m.token);
    b += "\",\"clear\":true}]";
  }
  b += "}";
  uint32_t t0 = millis();
  int code = httpsRequest("https://ntfy.sh/", &b, nullptr);
  Serial.printf("[ntfy] \"%s\" -> %d (%lu ms, Stack-Reserve %u Bytes)\n", m.title, code,
                (unsigned long)(millis() - t0), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  return code >= 200 && code < 300;
}

// Hat jemand am Handy "Ich komme" getippt? (Die Schaltfläche schickt die Kennung an <kanal>-ok)
// Ab 0.1.8: EINE offene Verbindung (ntfy-JSON-Stream) während des Hilferufs, statt alle paar Sekunden neu zu
// verbinden. Die Antwort kommt sofort an, und die Verschlüsselung muss nur einmal aufgebaut werden.
// "since" holt auch eine Antwort nach, die kam, bevor die Verbindung stand.
static WiFiClientSecure* ackCli = nullptr;
static String ackBuf;
static uint32_t ackLastData = 0, ackRetryAt = 0;

static void ackClose() {
  if (ackCli) { ackCli->stop(); delete ackCli; ackCli = nullptr; }
  ackBuf = "";
}

static bool ackOpen() {
  for (int pass = 0; pass < 2; pass++) {          // wie beim Senden: erst mit Zertifikatsprüfung, dann ohne
    ackCli = new WiFiClientSecure();
    if (pass == 0) ackCli->setCACert(ROOT_CA_PEM); else ackCli->setInsecure();
    ackCli->setHandshakeTimeout(15);
    if (ackCli->connect("ntfy.sh", 443)) {
      String req = "GET /";
      req += S.topic;
      req += "-ok/json?since=";
      req += ackSince ? String((unsigned long)(ackSince - 5)) : String("10m");
      req += " HTTP/1.1\r\nHost: ntfy.sh\r\nUser-Agent: Wolkenstation\r\n\r\n";
      ackCli->print(req);
      ackBuf = "";
      ackLastData = millis();
      Serial.println("[ntfy] warte auf \"Ich komme\" (Verbindung offen)");
      return true;
    }
    ackClose();
  }
  return false;
}

// Neue Daten lesen (blockiert nicht). true = Kennung gefunden
static bool ackRead() {
  String needle = String("\"message\":\"") + ackToken + "\"";
  uint8_t buf[256];
  int n;
  while ((n = ackCli->read(buf, sizeof(buf))) > 0) {
    ackLastData = millis();
    for (int i = 0; i < n; i++) ackBuf += (char)buf[i];
    if (ackBuf.indexOf(needle) >= 0) return true;
    if (ackBuf.length() > 512) ackBuf.remove(0, ackBuf.length() - 128);   // Ende behalten: Kennung evtl. halb drin
  }
  return false;
}

static void ntfyTask(void*) {
  uint8_t fails = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ackCli ? 200 : 1000));   // bei offener Verbindung öfter nachsehen
    if (ackPollOn && (!H.active || H.acked)) ackPollOn = false;     // Hilferuf vorbei
    if (!ackPollOn && ackCli) ackClose();
    if (!WiFi.isConnected() || state != NET_CONNECTED) continue;
    // Nachrichten senden
    for (;;) {
      NtfyMsg m;
      xSemaphoreTake(qMtx, portMAX_DELAY);
      if (qCount == 0) { xSemaphoreGive(qMtx); break; }
      m = queue[qHead];
      xSemaphoreGive(qMtx);
      bool expired = millis() - m.created > 12UL * 3600UL * 1000UL;
      bool ok = expired ? false : ntfyPost(m);
      if (m.kind == NTFY_TEST && !expired) testResult = ok ? 1 : -1;
      if (ok || expired || ++fails >= 3) {
        xSemaphoreTake(qMtx, portMAX_DELAY);
        qHead = (qHead + 1) % 8; qCount--;
        xSemaphoreGive(qMtx);
        fails = 0;
      } else {
        vTaskDelay(pdMS_TO_TICKS(4000));
        break;
      }
    }
    // während eines Hilferufs auf "Ich komme" vom Handy achten
    if (ackPollOn) {
      if (!ackCli && (int32_t)(millis() - ackRetryAt) >= 0 && !ackOpen()) ackRetryAt = millis() + 5000;
      if (ackCli) {
        if (ackRead()) {
          Serial.println("[ntfy] \"Ich komme\" vom Handy erhalten");
          ackSeen = true; ackPollOn = false; ackClose();
        } else if (!ackCli->connected() || millis() - ackLastData > 100000UL) {   // ntfy sendet alle ~45 s ein Lebenszeichen
          Serial.println("[ntfy] Verbindung für \"Ich komme\" weg - verbinde neu");
          ackClose(); ackRetryAt = millis() + 5000;
        }
      }
    }
  }
}

// ------------------------------------------------------------------ Update über WLAN (ab 0.1.8)
// Läuft nur, solange in den Einstellungen "Update über WLAN" offen ist (höchstens 15 Minuten).
// Schutz: 4-stelliger Code vom Display, nach 5 falschen Codes ist Schluss. Es wird nur eine echte
// ESP32-Programmdatei angenommen; bei Fehlern bleibt die alte Firmware einfach drauf.
static WebServer* otaSrv = nullptr;
static char otaCodeStr[5] = "";
static uint32_t otaStartMs = 0, otaRestartAt = 0;
static bool otaOk = false, otaBegun = false;
static uint8_t otaBadCodes = 0;
static size_t otaTotal = 0, otaDone = 0;
static int otaLastPct = -1;
static String otaErr;

bool otaActive() { return otaSrv != nullptr; }
const char* otaCode() { return otaCodeStr; }

static void otaPage() {
  String b = "<h1>Wolkenstation – Update</h1><p class=small>Läuft gerade: Version " WOLKE_FW_VERSION "</p><div class=card>"
    "<p>Arduino IDE: <b>Sketch → Kompilierte Binärdatei exportieren</b>. Danach liegt im Sketch-Ordner unter "
    "<b>build/esp32.esp32.esp32/</b> die Datei <b>Wolkenstation.ino.bin</b>. Nur diese nehmen, nicht die mit "
    "„bootloader“, „partitions“ oder „merged“ im Namen.</p>"
    "<label class=f for=c>Code vom Display</label><input type=text id=c inputmode=numeric maxlength=4 autocomplete=off>"
    "<label class=f for=f>Firmware-Datei (.bin)</label><input type=file id=f accept=.bin>"
    "<button onclick=go()>Hochladen und installieren</button>"
    "<progress id=p max=100 value=0 style='width:100%;margin-top:14px;display:none'></progress><p id=m></p></div>"
    "<script>function go(){var f=document.getElementById('f').files[0],c=document.getElementById('c').value.trim(),"
    "m=document.getElementById('m'),p=document.getElementById('p');"
    "if(!f){m.textContent='Bitte zuerst die .bin-Datei wählen.';return}"
    "var x=new XMLHttpRequest(),d=new FormData();d.append('firmware',f,f.name);"
    "x.open('POST','/update?code='+encodeURIComponent(c));"
    "x.upload.onprogress=function(e){if(e.lengthComputable){p.style.display='block';p.value=e.loaded*100/e.total}};"
    "x.onload=function(){m.textContent=x.responseText};"
    "x.onerror=function(){m.textContent='Verbindung abgebrochen. Schau auf die Station.'};"
    "m.textContent='Lade hoch …';x.send(d)}</script>";
  sendPageOn(otaSrv, b);
}

static void otaFail(const String& why) {
  if (otaBegun) Update.abort();
  otaOk = false; otaBegun = false; otaErr = why;
  Serial.printf("[Update] abgebrochen: %s\n", why.c_str());
}

static void otaUpload() {
  HTTPUpload& up = otaSrv->upload();
  if (up.status == UPLOAD_FILE_START) {
    otaOk = false; otaBegun = false; otaErr = ""; otaDone = 0; otaLastPct = -1;
    otaTotal = (size_t)otaSrv->header("Content-Length").toInt();
    if (strcmp(otaSrv->arg("code").c_str(), otaCodeStr) != 0) { otaBadCodes++; otaErr = "Der Code stimmt nicht."; return; }
    audioStop();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { otaErr = String("Update startet nicht: ") + Update.errorString(); return; }
    otaOk = otaBegun = true;
    Serial.printf("[Update] empfange %s (%u Bytes)\n", up.filename.c_str(), (unsigned)otaTotal);
    sysOtaScreen(0, "Empfange Datei ...");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!otaOk) return;
    // Erster Block: Ist das wirklich ein ESP32-Programm? (Kennung 0xE9 und Programm-Beschreibung ab Byte 32)
    if (otaDone == 0 && (up.currentSize < 36 || up.buf[0] != 0xE9 ||
                         up.buf[32] != 0x32 || up.buf[33] != 0x54 || up.buf[34] != 0xCD || up.buf[35] != 0xAB)) {
      otaFail("Das ist nicht die richtige Datei. Bitte Wolkenstation.ino.bin nehmen.");
      sysOtaScreen(1, "Falsche Datei - alte Version bleibt");
      return;
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      otaFail(String("Schreibfehler: ") + Update.errorString());
      sysOtaScreen(1, "Fehler - alte Version bleibt");
      return;
    }
    otaDone += up.currentSize;
    int pct = otaTotal ? (int)(otaDone * 100 / otaTotal) : 0;
    if (pct > 99) pct = 99;
    if (pct >= otaLastPct + 2) {
      otaLastPct = pct;
      char t[40]; snprintf(t, sizeof(t), "%d %%  (%u KB)", pct, (unsigned)(otaDone / 1024));
      sysOtaScreen(pct, t);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!otaOk) return;
    if (!Update.end(true)) {
      otaBegun = false;
      otaFail(String("Datei ungültig: ") + Update.errorString());
      sysOtaScreen(1, "Fehler - alte Version bleibt");
      return;
    }
    otaBegun = false;
    Serial.printf("[Update] fertig, %u Bytes\n", (unsigned)up.totalSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    otaFail("Übertragung abgebrochen.");
  }
}

static void otaFinish() {
  if (otaOk) {
    otaSrv->send(200, "text/plain; charset=utf-8", "Fertig! Die Station startet jetzt neu und zeigt die neue Version an.");
    sysOtaScreen(100, "Fertig - startet neu ...");
    sysOtaMarker = OTA_MARKER;
    otaRestartAt = millis() + 1500;
  } else {
    otaSrv->send(400, "text/plain; charset=utf-8", otaErr.length() ? otaErr : String("Fehler beim Update."));
    uiDirty();                       // Station wieder normal anzeigen
  }
}

bool otaStart() {
  if (otaSrv) return true;
  if (portalOn || state != NET_CONNECTED) return false;
  snprintf(otaCodeStr, sizeof(otaCodeStr), "%04u", (unsigned)(esp_random() % 10000));
  otaBadCodes = 0;
  otaSrv = new WebServer(80);
  const char* hk[] = {"Content-Length"};
  otaSrv->collectHeaders(hk, 1);
  otaSrv->on("/", HTTP_GET, otaPage);
  otaSrv->on("/update", HTTP_POST, otaFinish, otaUpload);
  otaSrv->onNotFound([]() { otaSrv->sendHeader("Location", "/", true); otaSrv->send(302, "text/plain", ""); });
  otaSrv->begin();
  otaStartMs = millis();
  sysLoopWdt(false);                 // ein Upload dauert am Stück länger als 5 s
  Serial.printf("[Update] bereit: http://%s  Code %s\n", ipStr, otaCodeStr);
  return true;
}

void otaStop() {
  if (!otaSrv) return;
  otaSrv->stop();
  delete otaSrv;
  otaSrv = nullptr;
  sysLoopWdt(true);
  Serial.println("[Update] aus");
}

// ------------------------------------------------------------------ Start und Schleife
void netInit() {
  setenv("TZ", TZ_DE, 1);
  tzset();
  qMtx = xSemaphoreCreateMutex();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);      // wir steuern Neuversuche selbst (Kanal bleibt für den Knopf stabil)
  WiFi.setSleep(false);              // ohne Stromsparen – sonst verpasst die Station Knopf-Nachrichten
  WiFi.setHostname("wolkenstation");
  WiFi.onEvent(onWifiEvent);
  if (storeLoadWifi(credSsid, sizeof(credSsid), credPass, sizeof(credPass))) {
    strncpy(trySsid, credSsid, sizeof(trySsid));
    strncpy(tryPass, credPass, sizeof(tryPass));
    tryingNew = false;
    viaPortal = false;
    attempt = 1; failPass = failNotFound = 0;
    connectStart = millis();
    state = NET_CONNECTING;
    Serial.printf("[WLAN] verbinde mit gespeichertem WLAN \"%s\"\n", credSsid);
    beginWith(credSsid, credPass, 0);
  }
  // Kern 1, NICHT Kern 0: Die Verschlüsselung (TLS) rechnet beim Verbindungsaufbau mehrere Sekunden am Stück.
  // Auf Kern 0 kam dabei die Leerlauf-Aufgabe nicht mehr dran -> Watchdog nach 5 s -> Neustart (bis 0.1.5,
  // auch bei echten Hilferufen!). Kern 1 wird vom Watchdog nicht überwacht; WLAN und Funk bleiben auf Kern 0 frei.
  xTaskCreatePinnedToCore(ntfyTask, "ntfy", 12288, nullptr, 1, &ntfyTaskHandle, 1);
}

void netLoop() {
  uint32_t now = millis();
  if (evGotIp) {
    evGotIp = false;
    state = NET_CONNECTED;
    retryCount = 0;
    lastChannel = netChannel();
    if (tryingNew || strcmp(credSsid, trySsid) != 0 || strcmp(credPass, tryPass) != 0) {
      strncpy(credSsid, trySsid, sizeof(credSsid));
      strncpy(credPass, tryPass, sizeof(credPass));
      storeSaveWifi(credSsid, credPass);
      tryingNew = false;
    }
    WiFi.setSleep(false);
    strncpy(ipStr, WiFi.localIP().toString().c_str(), sizeof(ipStr) - 1);
    configTzTime(TZ_DE, "pool.ntp.org", "time.google.com", "ptbtime1.ptb.de");
    Serial.printf("[WLAN] verbunden mit \"%s\", IP %s, Kanal %u\n", credSsid, ipStr, lastChannel);
    if (portalOn) portalStopAt = now + 20000;   // Handy soll das Ergebnis noch sehen
    if (ntfyTaskHandle) xTaskNotifyGive(ntfyTaskHandle);
  }
  if (evDisc) {
    evDisc = false;
    uint8_t r = evReason;
    if (state == NET_CONNECTING) {
      if (isPassReason(r)) failPass++;
      if (r == WIFI_REASON_NO_AP_FOUND) failNotFound++;
      Serial.printf("[WLAN] Versuch %u fehlgeschlagen (Grund %u)\n", attempt, r);
      if (attempt < 3) {
        attempt++;
        beginWith(trySsid, tryPass, 0);
      } else {
        state = failPass ? NET_ERR_PASS : failNotFound ? NET_ERR_NOTFOUND : NET_ERR_OTHER;
        WiFi.disconnect(false, false);
        nextRetryMs = now + 90000;
        Serial.printf("[WLAN] nicht verbunden (%s)\n", state == NET_ERR_PASS ? "Passwort" : state == NET_ERR_NOTFOUND ? "nicht gefunden" : "Fehler");
      }
    } else if (state == NET_CONNECTED) {
      Serial.printf("[WLAN] Verbindung verloren (Grund %u)\n", r);
      state = NET_LOST;
      nextRetryMs = now + 5000;
    } else if (state == NET_LOST) {
      nextRetryMs = now + 30000;
    }
  }
  if (state == NET_CONNECTING && now - connectStart > 30000) {
    state = failPass ? NET_ERR_PASS : failNotFound ? NET_ERR_NOTFOUND : NET_ERR_OTHER;
    WiFi.disconnect(false, false);
    nextRetryMs = now + 90000;
    Serial.println("[WLAN] Zeitüberschreitung");
  }
  // Automatisch wieder verbinden (nicht während der Einrichtung übers Handy)
  bool errState = state == NET_ERR_PASS || state == NET_ERR_NOTFOUND || state == NET_ERR_OTHER;
  if ((state == NET_LOST || (errState && !portalOn)) && credSsid[0] && (int32_t)(now - nextRetryMs) >= 0) {
    if (errState) state = NET_LOST;
    retryCount++;
    reconnectSaved(lastChannel == 0 || retryCount % 4 == 0);
    nextRetryMs = now + 30000;
  }
  if (scanning) scanCollect();
  if (portalOn) {
    dns->processNextRequest();
    server->handleClient();
    if (portalStopAt && (int32_t)(now - portalStopAt) >= 0) netPortalStop();
  }
  if (otaSrv) {
    otaSrv->handleClient();
    if (otaBadCodes >= 5 || state != NET_CONNECTED || now - otaStartMs > 15UL * 60UL * 1000UL) {
      otaStop();
      uiDirty();
    }
  }
  if (otaRestartAt && (int32_t)(millis() - otaRestartAt) >= 0) ESP.restart();
  if (ackSeen) { ackSeen = false; helpAck(true); }
}
