// Wolkenstation – Updates aus dem Internet (GitHub-Releases).
//
// Ablauf:
//  - Einmal am Tag (das erste Mal 3 Minuten nach dem Start) und auf Knopfdruck fragt die Station bei GitHub nach
//    dem neuesten Release von WOLKE_UPDATE_REPO (app.h). Vorabversionen ("pre-release") und Entwürfe zählen nicht.
//  - Ist die Version (Tag, z. B. "v0.1.9") neuer als die laufende: Hinweis auf der Startseite und EINMAL pro
//    Version eine leise Nachricht aufs Handy.
//  - Installiert wird nur, wenn jemand an der Station auf "Jetzt installieren" tippt.
//  - Geprüft wird: echtes ESP32-Programm, richtige Größe und SHA256-Prüfsumme (rechnet GitHub selbst aus).
//    Bei jedem Fehler bleibt die alte Version aktiv. Verbindungen NUR mit Zertifikatsprüfung (kein Rückfall wie
//    bei ntfy), sonst könnte jemand im Netz eine fremde Firmware unterschieben.
//  - Läuft ein Hilferuf, wird nicht installiert bzw. erst danach neu gestartet.
//
// So veröffentlichst du ein Update:
//  1. WOLKE_FW_VERSION in app.h erhöhen, z. B. "0.1.10".
//  2. Arduino IDE: Sketch -> Kompilierte Binärdatei exportieren.
//  3. GitHub: Releases -> "Draft a new release" -> Tag "v0.1.10" (dieselbe Nummer!) -> Titel und Text
//     (die ersten Zeilen erscheinen auf der Station) -> Datei build/esp32.esp32.esp32/Wolkenstation.ino.bin
//     anhängen -> "Publish release". Als "pre-release" markiert bekommen es die Stationen noch nicht (zum Testen).
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include "app.h"
#include "platform.h"
#include "update_certs.h"
#include "update_json.h"

static const char* BIN_SUFFIX = ".ino.bin";

static volatile UpdState st = UPD_IDLE;
static char newVer[16] = "", notes[200] = "", err[96] = "";
static char binUrl[320] = "", binSha[65] = "";
static long binSize = -1;
static volatile int prog = 0;
static volatile uint32_t lastCheck = 0, restartAt = 0;
static volatile bool notifyPending = false;
static volatile bool wantInstall = false;
static TaskHandle_t worker = nullptr;
static uint32_t nextCheckMs = 0;

bool        updRepoSet()    { return WOLKE_UPDATE_REPO[0] != 0; }
UpdState    updState()      { return st; }
const char* updVersion()    { return newVer; }
const char* updNotes()      { return notes; }
const char* updError()      { return err; }
int         updProgress()   { return prog; }
uint32_t    updLastCheck()  { return lastCheck; }
bool        updCanInstall() { return binUrl[0] && verNewer(newVer, WOLKE_FW_VERSION); }

static void fail(const char* msg) {
  strncpy(err, msg, sizeof(err) - 1);
  err[sizeof(err) - 1] = 0;
  st = UPD_ERROR;
  Serial.printf("[Update] Fehler: %s\n", msg);
}

static void prepare(WiFiClientSecure& cli, HTTPClient& http) {
  cli.setCACert(UPDATE_CA_PEM);
  cli.setHandshakeTimeout(20);
  http.setConnectTimeout(10000);
  http.setTimeout(20000);
  http.setReuse(false);
  http.setUserAgent("Wolkenstation/" WOLKE_FW_VERSION);
}

// ------------------------------------------------------------------ Nachsehen
static void doCheck() {
  st = UPD_CHECKING;
  WiFiClientSecure cli;
  HTTPClient http;
  prepare(cli, http);
  String url = String("https://api.github.com/repos/") + WOLKE_UPDATE_REPO + "/releases/latest";
  if (!http.begin(cli, url)) { fail("Update-Adresse ungültig."); return; }
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  if (code != 200) {
    char e[80] = "";
    cli.lastError(e, sizeof(e));
    Serial.printf("[Update] GitHub antwortet %d %s\n", code, e);
    http.end();
    fail(code == 404 ? "Auf GitHub gibt es noch kein Release." :
         code < 0    ? "Keine sichere Verbindung zu GitHub." : "GitHub antwortet gerade nicht richtig.");
    return;
  }
  String s = http.getString();
  http.end();
  const char* a = s.c_str();
  const char* e = a + s.length();
  char tag[24], body[600], url2[sizeof(binUrl)], sha[65];
  long size = -1;
  if (!jStr(a, e, "tag_name", tag, sizeof(tag), false)) { fail("Antwort von GitHub unvollständig."); return; }
  if (!jStr(a, e, "body", body, sizeof(body), true)) body[0] = 0;
  if (!jAsset(a, e, BIN_SUFFIX, url2, sizeof(url2), &size, sha, sizeof(sha))) {
    fail("Im Release fehlt die Datei Wolkenstation.ino.bin.");
    return;
  }
  lastCheck = timeValid() ? epochNow() : 1;
  if (!verNewer(tag, WOLKE_FW_VERSION)) {
    st = UPD_NONE;
    Serial.printf("[Update] aktuell (neuestes Release: %s)\n", tag);
    return;
  }
  const char* t = tag;
  while (*t && !isdigit((unsigned char)*t)) t++;          // "v0.1.9" -> "0.1.9"
  strncpy(newVer, t, sizeof(newVer) - 1);
  newVer[sizeof(newVer) - 1] = 0;
  notesClean(body, notes, sizeof(notes), 4);
  strcpy(binUrl, url2);
  strcpy(binSha, sha);
  binSize = size;
  Serial.printf("[Update] neue Version %s (%ld Bytes, Prüfsumme %s)\n", newVer, size, sha[0] ? "ja" : "fehlt");
  st = UPD_AVAILABLE;
  notifyPending = true;
}

// ------------------------------------------------------------------ Laden und installieren
// Nimmt den Download entgegen: Kopf prüfen, in den Update-Bereich schreiben, Prüfsumme mitrechnen.
class UpdSink : public Stream {
 public:
  UpdSink(size_t total, mbedtls_sha256_context* sha) : total(total), sha(sha) {}
  size_t total, done = 0;
  const char* why = nullptr;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) override {
    if (why) return 0;
    size_t used = 0;
    if (headN < sizeof(head)) {                     // die ersten 36 Bytes sammeln und prüfen
      used = n < sizeof(head) - headN ? n : sizeof(head) - headN;
      memcpy(head + headN, b, used);
      headN += used;
      if (headN < sizeof(head)) return n;
      if (head[0] != 0xE9 || head[32] != 0x32 || head[33] != 0x54 || head[34] != 0xCD || head[35] != 0xAB) {
        why = "Die Datei ist kein Programm für die Station.";
        return 0;
      }
      if (!put(head, sizeof(head))) return 0;
    }
    if (n > used && !put(b + used, n - used)) return 0;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  mbedtls_sha256_context* sha;
  uint8_t head[36];
  size_t headN = 0;
  bool put(const uint8_t* b, size_t n) {
    if (Update.write((uint8_t*)b, n) != n) { why = "Schreiben in den Speicher fehlgeschlagen."; return false; }
    mbedtls_sha256_update_ret(sha, b, n);
    done += n;
    if (total) prog = (int)(done * 100 / total);
    return true;
  }
};

static void doInstall() {
  st = UPD_LOADING;
  prog = 0;
  err[0] = 0;
  audioStop();
  WiFiClientSecure cli;
  HTTPClient http;
  prepare(cli, http);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GitHub leitet zum Download-Server weiter
  if (!http.begin(cli, binUrl)) { fail("Download-Adresse ungültig."); return; }
  int code = http.GET();
  if (code != 200) {
    char e[80] = "";
    cli.lastError(e, sizeof(e));
    Serial.printf("[Update] Download antwortet %d %s\n", code, e);
    http.end();
    fail(code < 0 ? "Keine sichere Verbindung zum Download." : "Download nicht gefunden.");
    return;
  }
  int len = http.getSize();
  if (len <= 0 || (binSize > 0 && len != binSize)) { http.end(); fail("Download hat die falsche Größe."); return; }
  if (!Update.begin(len, U_FLASH)) { http.end(); fail("Nicht genug Platz für das Update."); return; }
  Serial.printf("[Update] lade %d Bytes\n", len);
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  UpdSink sink((size_t)len, &sha);
  int w = http.writeToStream(&sink);
  http.end();
  uint8_t hash[32];
  mbedtls_sha256_finish_ret(&sha, hash);
  mbedtls_sha256_free(&sha);
  if (sink.why || w < 0 || sink.done != (size_t)len) {
    Update.abort();
    fail(sink.why ? sink.why : "Download abgebrochen - alte Version bleibt.");
    return;
  }
  if (binSha[0]) {
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    if (strcasecmp(hex, binSha) != 0) { Update.abort(); fail("Prüfsumme falsch - Datei beschädigt."); return; }
  }
  if (!Update.end(true)) { fail("Installation fehlgeschlagen - alte Version bleibt."); return; }
  Serial.printf("[Update] Version %s installiert\n", newVer);
  prog = 100;
  restartAt = millis() + 3000;
  st = UPD_DONE;
}

// ------------------------------------------------------------------ Hintergrund-Aufgabe (Kern 1, wie ntfy)
static void workerTask(void*) {
  if (wantInstall) { wantInstall = false; doInstall(); }
  else doCheck();
  worker = nullptr;
  vTaskDelete(nullptr);
}

static bool spawn() {
  if (xTaskCreatePinnedToCore(workerTask, "update", 12288, nullptr, 1, &worker, 1) == pdPASS) return true;
  worker = nullptr;
  return false;
}

void updCheckNow() {
  if (worker || !updRepoSet() || !netOnline() || st == UPD_LOADING || st == UPD_DONE) return;
  wantInstall = false;
  st = UPD_CHECKING;
  if (!spawn()) fail("Konnte nicht nachsehen.");
}

bool updInstall() {
  if (worker || !updCanInstall() || (H.active && !H.acked) || otaActive() || !netOnline()) return false;
  if (st != UPD_AVAILABLE && st != UPD_ERROR) return false;
  wantInstall = true;
  st = UPD_LOADING;
  prog = 0;
  if (!spawn()) { fail("Konnte das Update nicht starten."); return false; }
  return true;
}

// Aus loop(): Zeitplan, Handy-Nachricht, Neustart nach der Installation
void updLoop() {
  if (!updRepoSet()) return;
  uint32_t now = millis();
  if (notifyPending) {
    notifyPending = false;
    char last[16] = "";
    storeLoadStr("updnote", last, sizeof(last));
    if (strcmp(last, newVer) != 0) {                // nur einmal pro Version
      storeSaveStr("updnote", newVer);
      char title[64], msg[240], first[100];
      snprintf(title, sizeof(title), "Update verfügbar: Wolkenstation %s", newVer);
      strncpy(first, notes, sizeof(first) - 1);
      first[sizeof(first) - 1] = 0;
      char* nl = strchr(first, '\n');
      if (nl) *nl = 0;
      snprintf(msg, sizeof(msg), "%s%sInstallieren an der Station: auf das gelbe Schild oben tippen oder Einstellungen > Updates.",
               first, first[0] ? "\n" : "");
      ntfySend(title, msg, 2, NTFY_NORMAL, "package");
    }
    uiDirty();
  }
  if (st == UPD_DONE) {
    if (!(H.active && !H.acked) && (int32_t)(now - restartAt) >= 0) {
      Serial.println("[Update] Neustart mit der neuen Version");
      sysOtaMarker = OTA_MARKER;
      delay(100);
      ESP.restart();
    }
    return;
  }
  if (worker || !netOnline() || !timeValid() || otaActive()) return;
  if (nextCheckMs == 0) { nextCheckMs = now + 3UL * 60UL * 1000UL; return; }
  if ((int32_t)(now - nextCheckMs) >= 0) {
    nextCheckMs = now + 24UL * 3600UL * 1000UL;
    if (st != UPD_AVAILABLE) updCheckNow();         // gefundenes Update bleibt angezeigt
  }
}
