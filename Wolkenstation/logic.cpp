// Wolkenstation – Ablauflogik: Knopfdruck, Hilferuf, Tageswechsel, 6-Uhr-Bericht.
// Plattformneutral (läuft auch im PC-Test).
#include "app.h"
#include <stdio.h>
#include <math.h>

Settings  S;
Knopf     K;
Stats     ST;
HelpState H;

static uint32_t lastPressMs   = 0;
static bool     lastPressValid = false;

// ------------------------------------------------------------------ Hilfsfunktionen
void fmtHM(int m, char* buf) {
  if (m < 0) { strcpy(buf, "--:--"); return; }
  m %= 1440;
  snprintf(buf, 6, "%02d:%02d", m / 60, m % 60);
}

const char* childName() { return S.name[0] ? S.name : "Euer Kind"; }

bool inWakeWindow(int m) {
  if (m < 0) return false;
  return m >= S.wakeFrom && m < S.wakeTo;
}

bool inNight(int m) {
  if (m < 0) return false;
  int a = S.nightFrom, b = S.nightTo;
  if (a == b) return false;
  return a > b ? (m >= a || m < b) : (m >= a && m < b);
}

// Alkaline-Kennlinie pro Zelle (Ruhespannung) -> Prozent
int battPct(uint16_t mvRest) {
  if (mvRest == 0) return -1;
  static const float PCT_TAB[6] = {100, 80, 60, 40, 20, 0};
  static const float VOLT_TAB[6] = {1.55f, 1.45f, 1.35f, 1.27f, 1.20f, 1.10f};
  float v = mvRest / 3000.0f;
  if (v >= VOLT_TAB[0]) return 100;
  if (v <= VOLT_TAB[5]) return 0;
  for (int i = 0; i < 5; i++) {
    if (v <= VOLT_TAB[i] && v >= VOLT_TAB[i + 1]) {
      float p = PCT_TAB[i + 1] + (PCT_TAB[i] - PCT_TAB[i + 1]) * (v - VOLT_TAB[i + 1]) / (VOLT_TAB[i] - VOLT_TAB[i + 1]);
      return (int)lroundf(p);
    }
  }
  return 0;
}

const char* etaText(int pct, char* buf, size_t n) {
  if (pct < 0) { snprintf(buf, n, "noch keine Messung"); return buf; }
  if (pct <= 0) { snprintf(buf, n, "leer"); return buf; }
  int months = (int)lroundf(pct / 0.18f / 30.4f);
  if (months < 1) snprintf(buf, n, "nur noch wenige Tage");
  else if (months == 1) snprintf(buf, n, "reicht noch ca. 1 Monat");
  else snprintf(buf, n, "reicht noch ca. %d Monate", months);
  return buf;
}

bool knopfSilent() {
  if (!K.paired || !timeValid()) return false;
  if (K.lastSeen == 0) return true;
  return epochNow() - K.lastSeen > 26UL * 3600UL;
}

// ------------------------------------------------------------------ Morgen-Ton stumm
bool toneMuted() { return S.muteUntil && timeValid() && epochNow() < S.muteUntil; }

int toneMuteEndMinute() {
  if (!toneMuted()) return -1;
  int m = minuteNow();
  if (m < 0) return -1;
  int left = (int)((S.muteUntil - epochNow() + 59) / 60);
  return (m + left) % 1440;
}

// Stumm bis zum Ende des nächsten Morgenfensters (vor dem Fensterende: heute, danach: morgen früh)
bool toneMuteToggle() {
  if (toneMuted()) { S.muteUntil = 0; storeSaveSettings(); return false; }
  int m = minuteNow();
  if (m < 0) return false;                       // ohne Uhrzeit geht es nicht
  int d = (S.wakeTo - m + 1440) % 1440;
  if (d == 0) d = 1440;
  uint32_t now = epochNow();
  S.muteUntil = now - (now % 60) + (uint32_t)d * 60;
  storeSaveSettings();
  return true;
}

float weekAverage(bool& ok) {
  int sum = 0, n = 0;
  for (int i = 0; i < 7; i++) if (ST.week[i] >= 0) { sum += ST.week[i]; n++; }
  if (ST.today >= 0) { sum += ST.today; n++; }
  ok = n > 0;
  return n ? (float)sum / n : 0;
}

// Tage seit 1970 aus JJJJMMTT (H. Hinnant, days_from_civil)
static int32_t daysFromKey(uint32_t key) {
  int y = key / 10000, m = (key / 100) % 100, d = key % 100;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

static void randomTopic(char* out) {
  static const char A[] = "abcdefghjkmnpqrstuvwxyz23456789";
  strcpy(out, "wolke-");
  for (int i = 0; i < 10; i++) out[6 + i] = A[randomU32() % (sizeof(A) - 1)];
  out[16] = 0;
}

void logicDefaults() {
  memset(&S, 0, sizeof(S));
  S.version = SETTINGS_VERSION;
  S.melody = 0; S.volume = 60;
  S.wakeFrom = 4 * 60 + 30; S.wakeTo = 10 * 60;
  S.nightFrom = 20 * 60;    S.nightTo = 6 * 60;
  S.helpMode = 0; S.helpRepeat = 1;
  S.ntfyOn = 1; S.ntfyTested = 0;
  randomTopic(S.topic);
  S.rotation = 1;
  S.briDay = 80; S.briNight = 20; S.muteUntil = 0;

  memset(&K, 0, sizeof(K));
  K.version = KNOPF_VERSION; K.pct = -1;
  memset(K.hist, 255, sizeof(K.hist));

  memset(&ST, 0, sizeof(ST));
  ST.version = STATS_VERSION; ST.today = -1; ST.reportPct = -1;
  for (int i = 0; i < 7; i++) ST.week[i] = -1;
}

void logicInit() {
  if (S.topic[0] == 0) { randomTopic(S.topic); storeSaveSettings(); }
  memset(&H, 0, sizeof(H));
}

// ------------------------------------------------------------------ Tageswechsel
static void dayCheck() {
  uint32_t dk = dayKeyNow();
  if (!dk) return;
  if (ST.dayKey == 0) { ST.dayKey = dk; storeSaveStats(); return; }
  if (dk == ST.dayKey) return;
  int32_t diff = daysFromKey(dk) - daysFromKey(ST.dayKey);
  if (diff > 7) {
    for (int i = 0; i < 7; i++) ST.week[i] = -1;     // länger als eine Woche aus: alles veraltet
  } else if (diff > 0) {
    for (int i = 0; i < diff; i++) {
      memmove(&ST.week[0], &ST.week[1], 6 * sizeof(int16_t));
      ST.week[6] = (i == 0) ? ST.today : -1;
    }
  }
  ST.today = -1; ST.nightN = 0; ST.dayKey = dk;
  lastPressValid = false;
  storeSaveStats();
  uiDirty();
}

// ------------------------------------------------------------------ 6-Uhr-Bericht
static void reportCheck() {
  int m = minuteNow();
  if (m < 0 || !S.setupDone || !K.paired) return;
  if (m < 6 * 60 || m >= 10 * 60) return;
  if (ST.reportDay == ST.dayKey) return;
  ST.reportDay = ST.dayKey;
  ST.reportSilent = knopfSilent() ? 1 : 0;
  ST.reportPct = K.pct;
  storeSaveStats();
  uiDirty();
  if (ST.reportSilent) {
    ntfySend("Wolkenknopf meldet sich nicht",
             "Bitte die Batterien prüfen (3 × AA). Er hat sich seit über einem Tag nicht gemeldet.",
             4, NTFY_NORMAL, "battery");
  } else if (K.pct >= 0 && K.pct <= 20) {
    char eta[40], msg[96];
    etaText(K.pct, eta, sizeof(eta));
    snprintf(msg, sizeof(msg), "Wolkenknopf: %d %% · %s", K.pct, eta);
    ntfySend("Batterien bald tauschen", msg, 3, NTFY_NORMAL, "battery");
  }
}

void logicTick() {
  static uint32_t last = 0;
  uint32_t now = msNow();
  if (now - last >= 1000) {
    last = now;
    dayCheck();
    reportCheck();
  }
  if (H.active && !H.acked && S.helpRepeat && H.n < 11 && (int32_t)(now - H.nextMs) >= 0) {
    H.n++;
    H.nextMs += 10000;
    audioHelp(H.n);
    uiWake(60000);
    uiDirty();
  }
}

// ------------------------------------------------------------------ Ereignisse vom Knopf
void onKnopfReport(uint16_t mvRest, uint16_t mvLoad) {
  if (mvRest) {
    K.mvRest = mvRest;
    K.mvLoad = mvLoad;
    K.pct = (int8_t)battPct(mvRest);
  }
  if (timeValid()) K.lastSeen = epochNow();
  uint32_t dk = dayKeyNow();
  if (dk && K.pct >= 0) {
    if (K.histDay != dk) {
      int32_t diff = K.histDay ? daysFromKey(dk) - daysFromKey(K.histDay) : 1;
      if (diff < 1) diff = 1;
      if (diff > 30) diff = 30;
      for (int i = 0; i < diff; i++) {
        memmove(&K.hist[0], &K.hist[1], 29);
        K.hist[29] = 255;
      }
      K.histDay = dk;
    }
    K.hist[29] = (uint8_t)K.pct;
  }
  storeSaveKnopf();
  uiDirty();
}

void onKnopfPaired(const uint8_t mac[6], const uint8_t lmk[16], uint16_t mvRest, uint16_t mvLoad) {
  memcpy(K.mac, mac, 6);
  memcpy(K.lmk, lmk, 16);
  K.paired = 1;
  K.histDay = 0;
  memset(K.hist, 255, sizeof(K.hist));
  onKnopfReport(mvRest, mvLoad);   // speichert auch
  audioMelody(S.melody, S.volume);
  uiOnPaired();
}

void onKnopfPress() {
  uiWake();
  uint32_t now = msNow();
  if (lastPressValid && now - lastPressMs < 120000UL) {
    lastPressMs = now;
    uiToast("Schon gemeldet", false);
    return;
  }
  lastPressValid = true;
  lastPressMs = now;
  if (!toneMuted()) audioMelody(S.melody, S.volume);   // Nachricht ans Handy kommt trotzdem

  int m = minuteNow();
  char hm[6], title[80], msg[96], t[80];
  fmtHM(m, hm);
  if (m < 0) {
    snprintf(title, sizeof(title), "%s hat gedrückt", childName());
    ntfySend(title, "Die Station kennt gerade keine Uhrzeit (kein WLAN).", 4, NTFY_NORMAL, "bell");
    uiToast(title, true, 3200);
    return;
  }
  if (inWakeWindow(m)) {
    if (ST.today < 0) {
      ST.today = m;
      storeSaveStats();
      snprintf(title, sizeof(title), "%s ist wach", childName());
      snprintf(msg, sizeof(msg), "Aufgewacht um %s", hm);
      ntfySend(title, msg, 3, NTFY_NORMAL, "sunny");
      snprintf(t, sizeof(t), "%s ist wach · %s", childName(), hm);
      uiToast(t, true, 3200);
    } else {
      snprintf(t, sizeof(t), "%s drückt nochmal", childName());
      uiToast(t, false);
    }
  } else {
    if (ST.nightN < 6) ST.night[ST.nightN++] = m;
    else { memmove(&ST.night[0], &ST.night[1], 5 * sizeof(int16_t)); ST.night[5] = m; }
    storeSaveStats();
    snprintf(title, sizeof(title), "%s hat gedrückt", childName());
    snprintf(msg, sizeof(msg), "Um %s – außerhalb des Morgenfensters", hm);
    ntfySend(title, msg, 4, NTFY_NORMAL, "bell");
    snprintf(t, sizeof(t), "%s hat gedrückt · %s", childName(), hm);
    uiToast(t, true, 3200);
  }
  uiDirty();
}

void simulatePress() {
  uiWake();
  audioMelody(S.melody, S.volume);
  char title[80];
  snprintf(title, sizeof(title), "Test: %s ist wach", childName());
  ntfySend(title, "Nur ein Test der Wolkenstation – es wurde nichts gespeichert.", 3, NTFY_NORMAL, "sunny");
  uiToast("Test: Morgen-Ton gespielt", true, 2600);
}

void onKnopfHelp(bool test) {
  uiWake(60000);
  if (H.active && !H.acked) return;          // läuft schon
  memset(&H, 0, sizeof(H));
  H.active = true;
  H.test = test;
  H.minute = (int16_t)minuteNow();
  H.n = 0;
  H.nextMs = msNow() + 10000;
  H.startEpoch = timeValid() ? epochNow() : 0;
  snprintf(H.token, sizeof(H.token), "%08lx", (unsigned long)randomU32());
  if (!test && H.minute >= 0 && inWakeWindow(H.minute) && ST.today < 0) {
    ST.today = H.minute;
    storeSaveStats();
  }
  audioHelp(0);

  char hm[6], title[96], msg[96];
  fmtHM(H.minute, hm);
  if (test) {
    snprintf(title, sizeof(title), "TEST: %s ruft", childName());
    snprintf(msg, sizeof(msg), "Nur ein Test des Hilferufs. Tippe auf „Ich komme“ zum Ausprobieren.");
  } else {
    snprintf(title, sizeof(title), "%s ruft: Bitte nach mir schauen", childName());
    snprintf(msg, sizeof(msg), "Hilferuf um %s vom Wolkenknopf", hm);
  }
  ntfySend(title, msg, 5, NTFY_HELP, "rotating_light");
  uiOnHelp();
}

void helpAck(bool fromPhone) {
  if (!H.active || H.acked) return;
  H.acked = true;
  audioStop();
  if (!H.test) linkSendHelpAck();
  ntfySend(H.test ? "TEST: Hilferuf bestätigt" : "Hilferuf bestätigt",
           fromPhone ? "Jemand hat am Handy „Ich komme“ gedrückt."
                     : "Jemand hat an der Station „Ich komme“ gedrückt.",
           3, NTFY_NORMAL, "white_check_mark");
  H.active = false;
  uiToast(H.test ? "Test beendet" : "Bestätigt - der Knopf blinkt: Hilfe kommt", true, 2800);
  uiDirty();
}
