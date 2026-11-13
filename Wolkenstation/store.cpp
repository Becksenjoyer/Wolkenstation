// Wolkenstation – dauerhafter Speicher (NVS). Bleibt bei Stromausfall erhalten.
#include <Arduino.h>
#include <Preferences.h>
#include "app.h"
#include "platform.h"

static Preferences prefs;
static bool opened = false;

static void openPrefs() {
  if (!opened) { prefs.begin("wolke", false); opened = true; }
}

template <typename T>
static bool loadBlob(const char* key, T& out, uint8_t version) {
  if (prefs.getBytesLength(key) != sizeof(T)) return false;
  T tmp;
  if (prefs.getBytes(key, &tmp, sizeof(T)) != sizeof(T)) return false;
  if (tmp.version != version) return false;
  out = tmp;
  return true;
}

// Einstellungen von 0.1.7 und älter (Version 1) übernehmen: alle alten Felder bleiben, neue bekommen Standardwerte.
// Sonst würde nach dem Update die Einrichtung neu starten und der ntfy-Kanal wäre ein anderer.
static bool migrateSettingsV1() {
  static_assert(offsetof(Settings, briDay) == SETTINGS_V1_SIZE - 1, "Settings-Layout passt nicht zur Übernahme");
  if (prefs.getBytesLength("set") != SETTINGS_V1_SIZE) return false;
  uint8_t buf[SETTINGS_V1_SIZE];
  if (prefs.getBytes("set", buf, SETTINGS_V1_SIZE) != SETTINGS_V1_SIZE || buf[0] != 1) return false;
  memcpy(&S, buf, offsetof(Settings, briDay));
  S.version = SETTINGS_VERSION;
  prefs.putBytes("set", &S, sizeof(S));
  Serial.println("[Speicher] Einstellungen aus 0.1.7 übernommen");
  return true;
}

void storeLoad() {
  openPrefs();
  logicDefaults();
  if (!loadBlob("set", S, SETTINGS_VERSION) && !migrateSettingsV1()) Serial.println("[Speicher] Einstellungen: Standardwerte");
  if (!loadBlob("knopf", K, KNOPF_VERSION)) Serial.println("[Speicher] Wolkenknopf: noch nicht gekoppelt");
  if (!loadBlob("stats", ST, STATS_VERSION)) Serial.println("[Speicher] Statistik: leer");
  S.name[sizeof(S.name) - 1] = 0;
  S.topic[sizeof(S.topic) - 1] = 0;
  if (S.rotation != 1 && S.rotation != 3) S.rotation = 1;
  if (S.melody > 2) S.melody = 0;
  if (S.volume > 100) S.volume = 60;
  if (S.briDay < 20 || S.briDay > 100) S.briDay = 80;
  if (S.briNight < 10 || S.briNight > 100) S.briNight = 20;
}

void storeSaveSettings() { openPrefs(); prefs.putBytes("set", &S, sizeof(S)); }
void storeSaveKnopf()    { openPrefs(); prefs.putBytes("knopf", &K, sizeof(K)); }
void storeSaveStats()    { openPrefs(); prefs.putBytes("stats", &ST, sizeof(ST)); }

bool storeLoadWifi(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
  openPrefs();
  if (!prefs.isKey("wssid")) return false;
  prefs.getString("wssid", ssid, ssidLen);
  if (prefs.isKey("wpass")) prefs.getString("wpass", pass, passLen); else pass[0] = 0;
  return ssid[0] != 0;
}

void storeSaveWifi(const char* ssid, const char* pass) {
  openPrefs();
  prefs.putString("wssid", ssid);
  prefs.putString("wpass", pass);
}

bool storeLoadCal(uint16_t cal[8]) {
  openPrefs();
  if (prefs.getBytesLength("cal") != 16) return false;
  return prefs.getBytes("cal", cal, 16) == 16;
}

void storeSaveCal(const uint16_t cal[8]) { openPrefs(); prefs.putBytes("cal", cal, 16); }

bool storeLoadStr(const char* key, char* out, size_t n) {
  openPrefs();
  out[0] = 0;
  if (!prefs.isKey(key)) return false;
  prefs.getString(key, out, n);
  return out[0] != 0;
}

void storeSaveStr(const char* key, const char* v) { openPrefs(); prefs.putString(key, v); }

void storeFactoryReset() {
  openPrefs();
  prefs.clear();
  prefs.end();
  opened = false;
  Serial.println("[Speicher] Werkseinstellungen - Neustart");
  sysRestart();
}
