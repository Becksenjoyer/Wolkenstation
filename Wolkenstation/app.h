// Wolkenstation – gemeinsamer Zustand und Schnittstellen zwischen den Modulen.
// Alles hier ist plattformneutral, damit Oberfläche und Logik auch am PC getestet werden können.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef WOLKE_FW_VERSION
#define WOLKE_FW_VERSION "0.1.9"
#endif

// Öffentliches GitHub-Repository, aus dessen Releases die Station Updates holt, z. B. "tobias/wolkenstation".
// Leer = automatische Updates aus (Update per Datei vom PC geht trotzdem).
#ifndef WOLKE_UPDATE_REPO
#define WOLKE_UPDATE_REPO ""
#endif

// ---------------------------------------------------------------- Einstellungen (NVS)
struct Settings {
  uint8_t  version;          // Layout-Version dieses Structs
  uint8_t  setupDone;
  char     name[49];         // Name des Kindes, UTF-8, max. 16 Zeichen
  uint8_t  melody;           // 0 Glöckchen, 1 Vogelzwitschern, 2 Sonnenaufgang
  uint8_t  volume;           // 0..100 in 10er-Schritten
  int16_t  wakeFrom, wakeTo; // Morgenfenster in Minuten ab Mitternacht
  int16_t  nightFrom, nightTo;
  uint8_t  helpMode;         // 0 = 2x schnell drücken, 1 = 2 s halten
  uint8_t  helpRepeat;       // Ruf alle 10 s wiederholen
  uint8_t  ntfyOn;           // Handy-Nachrichten senden
  uint8_t  ntfyTested;       // Test wurde schon einmal erfolgreich verschickt
  char     topic[24];        // ntfy-Kanal, Zufallsname
  uint8_t  rotation;         // 1 oder 3 (Display gedreht)
  // ab Version 2 (Firmware 0.1.8) – hinten angehängt, damit die alten Einstellungen übernommen werden können
  uint8_t  briDay;           // Helligkeit tagsüber in % (20..100)
  uint8_t  briNight;         // Helligkeit im Nachtmodus in % (10..100)
  uint32_t muteUntil;        // Morgen-Ton stumm bis zu dieser Unix-Zeit, 0 = Ton an
};
static const uint8_t SETTINGS_VERSION = 2;
static const size_t  SETTINGS_V1_SIZE = 92;   // Größe bis 0.1.7 (geprüft mit dem ESP32-Compiler)

// ---------------------------------------------------------------- Wolkenknopf
struct Knopf {
  uint8_t  version;
  uint8_t  paired;
  uint8_t  mac[6];
  uint8_t  lmk[16];          // Schlüssel für die verschlüsselte ESP-NOW-Verbindung
  uint16_t mvRest;           // Batteriespannung in Ruhe (mV, ganzes Pack)
  uint16_t mvLoad;           // Batteriespannung beim Senden (mV)
  int8_t   pct;              // -1 = unbekannt
  uint32_t lastSeen;         // Unix-Zeit der letzten Meldung, 0 = nie
  uint8_t  hist[30];         // Batterie-% der letzten 30 Tage (255 = keine Daten), ältester zuerst
  uint32_t histDay;          // Tagesschlüssel des letzten Eintrags (JJJJMMTT)
};
static const uint8_t KNOPF_VERSION = 1;

// ---------------------------------------------------------------- Statistik
struct Stats {
  uint8_t  version;
  int16_t  week[7];          // Aufwachzeit der letzten 7 Tage (ältester zuerst), -1 = kein Druck
  int16_t  today;            // Aufwachzeit heute, -1 = noch nicht
  int16_t  night[6];         // Drücke außerhalb des Morgenfensters heute
  uint8_t  nightN;
  uint32_t dayKey;           // JJJJMMTT von "heute"
  uint32_t reportDay;        // an welchem Tag der 6-Uhr-Bericht schon kam
  uint8_t  reportSilent;     // Knopf hat sich heute nicht gemeldet
  int8_t   reportPct;
};
static const uint8_t STATS_VERSION = 1;

// ---------------------------------------------------------------- Hilferuf (nur im RAM)
struct HelpState {
  bool     active;
  bool     acked;
  bool     test;             // ausgelöst über "Test: Hilferuf"
  int16_t  minute;
  uint8_t  n;                // wievielter Ruf (0 = erster)
  uint32_t nextMs;
  uint32_t startEpoch;
  char     token[9];         // Kennung für die "Ich komme"-Antwort vom Handy
};

extern Settings  S;
extern Knopf     K;
extern Stats     ST;
extern HelpState H;

// ---------------------------------------------------------------- Plattform: Zeit
bool     timeValid();
uint32_t epochNow();
int      minuteNow();                 // Minuten seit Mitternacht (lokal), -1 wenn Uhrzeit unbekannt
uint32_t dayKeyNow();                 // JJJJMMTT, 0 wenn unbekannt
int      weekdayNow();                // 0 = Sonntag
uint32_t msNow();
uint32_t randomU32();

// ---------------------------------------------------------------- Plattform: Ton
void audioMelody(int mel, int vol);
void audioHelp(int level);
void audioStop();

// ---------------------------------------------------------------- Plattform: Netz
enum NetState : uint8_t { NET_IDLE, NET_CONNECTING, NET_CONNECTED, NET_ERR_PASS, NET_ERR_NOTFOUND, NET_ERR_OTHER, NET_LOST };
struct ScanNet { char ssid[33]; int8_t rssi; bool lock; };

NetState    netState();
bool        netOnline();
const char* netSsid();               // gespeichertes / verbundenes WLAN
const char* netTrySsid();            // WLAN, mit dem gerade verbunden wird
int         netRssi();
bool        netHasCreds();
bool        netConnectViaPortal();   // letzter Verbindungsversuch kam vom Handy
void        netConnect(const char* ssid, const char* pass);
void        netScanStart();
bool        netScanBusy();
int         netScanCount();
const ScanNet& netScanGet(int i);
void        netPortalStart();
void        netPortalStop();
bool        netPortalActive();
bool        netPortalClient();       // Handy ist mit dem Einrichtungs-WLAN verbunden
const char* netApSsid();
const char* netApPass();
const char* netIp();

enum NtfyKind : uint8_t { NTFY_NORMAL = 0, NTFY_HELP = 1, NTFY_TEST = 2 };
void ntfySend(const char* title, const char* msg, uint8_t prio, NtfyKind kind, const char* tags);
int  ntfyTestResult();               // 0 = läuft/unbekannt, 1 = ok, -1 = Fehler
void ntfyTestReset();

// Update über WLAN (net.cpp) – nur solange die Seite in den Einstellungen offen ist
bool        otaStart();              // false = kein WLAN oder Einrichtungs-WLAN aktiv
void        otaStop();
bool        otaActive();
const char* otaCode();               // 4-stelliger Code für die Webseite

// Updates aus dem Internet (update.cpp, GitHub-Releases) – installiert wird nur nach Bestätigung an der Station
enum UpdState : uint8_t { UPD_IDLE, UPD_CHECKING, UPD_NONE, UPD_AVAILABLE, UPD_LOADING, UPD_DONE, UPD_ERROR };
bool        updRepoSet();            // Update-Quelle in WOLKE_UPDATE_REPO eingetragen
UpdState    updState();
const char* updVersion();            // gefundene neue Version, z. B. "0.1.10"
const char* updNotes();              // erste Zeilen aus dem Release-Text
const char* updError();
int         updProgress();           // 0..100 beim Laden
uint32_t    updLastCheck();          // Unix-Zeit der letzten erfolgreichen Prüfung, 0 = noch nie
bool        updCanInstall();         // eine neuere Version ist bekannt
void        updCheckNow();           // im Hintergrund bei GitHub nachsehen
bool        updInstall();            // laden und installieren (nur nach Tippen auf "Jetzt installieren")

// ---------------------------------------------------------------- Plattform: Wolkenknopf (ESP-NOW)
void linkPairStart();
void linkPairStop();
bool linkPairing();
void linkUnpair();
void linkSendHelpAck();

// ---------------------------------------------------------------- Plattform: Speicher & System
void storeSaveSettings();
void storeSaveKnopf();
void storeSaveStats();
void storeFactoryReset();            // löscht alles und startet neu
void sysRestart();
void sysCalibrateTouch();            // Touch neu einstellen (blockiert)
void sysSetRotation(uint8_t rot);

// ---------------------------------------------------------------- Logik (logic.cpp)
void logicDefaults();
void logicInit();
void logicTick();
void onKnopfPress();
void onKnopfHelp(bool test);
void onKnopfReport(uint16_t mvRest, uint16_t mvLoad);   // jede Meldung des Knopfs
void onKnopfPaired(const uint8_t mac[6], const uint8_t lmk[16], uint16_t mvRest, uint16_t mvLoad);
void helpAck(bool fromPhone);
void simulatePress();                // "Test: Aufwachen"

int         battPct(uint16_t mvRest);
const char* etaText(int pct, char* buf, size_t n);
const char* childName();             // Name oder "Euer Kind"
bool        inWakeWindow(int m);
bool        inNight(int m);
bool        knopfSilent();           // länger als 26 h nichts gehört
void        fmtHM(int minute, char* buf);   // "06:05"
float       weekAverage(bool& ok);
bool        toneMuted();             // Morgen-Ton gerade stumm geschaltet
bool        toneMuteToggle();        // true = jetzt stumm (bis Ende des nächsten Morgenfensters)
int         toneMuteEndMinute();     // Uhrzeit (Minuten), bis wann stumm, -1 = nicht stumm

// ---------------------------------------------------------------- Oberfläche (ui.cpp)
namespace lgfx { inline namespace v1 { class LovyanGFX; } }
void uiInit();
void uiRender(lgfx::LovyanGFX& g, int oy, bool collectHits);
void uiTouch(bool down, int x, int y);
bool uiTick();                       // true = neu zeichnen
void uiDirty();
void uiToast(const char* text, bool big, uint16_t ms = 2600);
void uiWake(uint32_t ms = 20000);    // Display einschalten
uint8_t uiBacklight();               // gewünschte Helligkeit 0..255
void uiOnNetEvent();                 // Netzstatus hat sich geändert
void uiOnPaired();
void uiOnPortalClient();
void uiOnHelp();
