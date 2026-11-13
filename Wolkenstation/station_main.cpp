// =====================================================================================
//  Wolkenstation – Firmware für das Display der Eltern (ESP32-2432S028 "Cheap Yellow Display")
//  Version siehe platformio.ini (WOLKE_FW_VERSION)
//
//  Hardware:  Display ILI9341 oder ST7789 (wird beim Start erkannt), Touch XPT2046,
//             Lautsprecher an der SPEAK-Buchse (Verstärker an GPIO26), RGB-LED (aus).
//  Funk:      ESP-NOW zum Wolkenknopf, WLAN für Uhrzeit und Handy-Nachrichten (ntfy.sh).
// =====================================================================================
#include <Arduino.h>

#define LGFX_SUNTON_ESP32_2432S028      // Autodetect nur für dieses Board (beide Display-Varianten)
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>

#include "app.h"
#include "platform.h"
#include <WiFi.h>
#include <esp_now.h>
#include <driver/i2s.h>

static LGFX lcd;
static LGFX_Sprite band(&lcd);
static int bandH = 0;
static uint8_t curBacklight = 255;

static const int LED_R = 4, LED_G = 16, LED_B = 17;   // RGB-LED, LOW = an
static const int PIN_TOUCH_IRQ = 36;                    // T_IRQ des Touch-Chips XPT2046

// ------------------------------------------------------------------ System-Funktionen für die Oberfläche
void sysRestart() {
  delay(300);
  ESP.restart();
}

void sysSetRotation(uint8_t rot) {
  lcd.setRotation(rot);
  uiDirty();
}

// Watchdog für die Hauptschleife: hängt loop() länger als 5 s, startet die Station neu (statt still einzufrieren).
// Aus bei Dingen, die absichtlich lange dauern (Touch einstellen, Update über WLAN).
static bool loopWdtOn = false;
void sysLoopWdt(bool on) {
  if (on == loopWdtOn) return;
  loopWdtOn = on;
  if (on) enableLoopWDT(); else disableLoopWDT();
}

// Überlebt einen Neustart per Software (nicht Stromausfall): Merker "Update wurde gerade installiert"
RTC_NOINIT_ATTR uint32_t sysOtaMarker;

static void centerText(const char* s, int y, const lgfx::IFont* f, uint16_t col) {
  lcd.setFont(f);
  lcd.setTextColor(col);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.drawString(s, lcd.width() / 2, y);
}

void sysCalibrateTouch() {
  bool wdt = loopWdtOn;
  sysLoopWdt(false);                 // wartet auf Berührungen, darf länger als 5 s dauern
  audioStop();
  lcd.setBrightness(200);
  lcd.fillScreen(TFT_WHITE);
  centerText("Touch einstellen", 80, &fonts::FreeSansBold12pt7b, lcd.color565(0x3f, 0x35, 0x55));
  centerText("Tippe nacheinander genau auf die", 118, &fonts::FreeSans9pt7b, lcd.color565(0x5b, 0x51, 0x70));
  centerText("Pfeilspitzen in den Ecken.", 140, &fonts::FreeSans9pt7b, lcd.color565(0x5b, 0x51, 0x70));
  centerText("Am besten mit einem Stift.", 168, &fonts::FreeSans9pt7b, lcd.color565(0x7a, 0x70, 0x90));
  delay(400);
  uint16_t cal[8];
  lcd.calibrateTouch(cal, lcd.color565(0x6b, 0x57, 0xc9), (uint16_t)TFT_WHITE, 16);
  storeSaveCal(cal);
  sysLoopWdt(wdt);
  lcd.setRotation(S.rotation);
  lcd.fillScreen(TFT_WHITE);
  Serial.println("[Touch] neu eingestellt");
  uiDirty();
}

// Anzeige während des Updates über WLAN (wird direkt gezeichnet, die Oberfläche steht solange still)
void sysOtaScreen(int pct, const char* text) {
  const uint16_t ink = lcd.color565(0x3f, 0x35, 0x55), acc = lcd.color565(0x6b, 0x57, 0xc9), trk = lcd.color565(0xe9, 0xe4, 0xf5);
  if (pct <= 0) {
    lcd.setBrightness(200);
    curBacklight = 200;
    lcd.fillScreen(TFT_WHITE);
    centerText("Update wird installiert", 80, &fonts::FreeSansBold12pt7b, ink);
    centerText("Bitte nicht ausstecken.", 196, &fonts::FreeSans9pt7b, lcd.color565(0x7a, 0x70, 0x90));
  }
  int w = 240 * (pct > 100 ? 100 : (pct < 0 ? 0 : pct)) / 100;
  lcd.fillRoundRect(40, 114, 240, 16, 8, trk);
  if (w >= 16) lcd.fillRoundRect(40, 114, w, 16, 8, acc);
  lcd.fillRect(0, 140, lcd.width(), 34, TFT_WHITE);
  centerText(text, 156, &fonts::FreeSans9pt7b, ink);
}

// ------------------------------------------------------------------ Zeichnen
static void renderScreen() {
  for (int y = 0; y < 240; y += bandH) {
    uiRender(band, y, y == 0);
    band.pushSprite(0, y);
  }
}

// ------------------------------------------------------------------ Warum neu gestartet?
// Ein ungewollter Neustart sieht aus wie "springt auf die Startseite". Darum nach Absturz oder Stromeinbruch
// 10 s lang einen Hinweis zeigen und den Grund im seriellen Monitor ausgeben.
static void showResetReason() {
  esp_reset_reason_t rr = esp_reset_reason();
  bool otaDone = rr == ESP_RST_SW && sysOtaMarker == OTA_MARKER;
  sysOtaMarker = 0;
  if (otaDone) {
    Serial.println("[System] Update installiert, Version " WOLKE_FW_VERSION);
    uiToast("Update installiert: Version " WOLKE_FW_VERSION, true, 6000);
    ntfySend("Wolkenstation aktualisiert", "Läuft jetzt mit Version " WOLKE_FW_VERSION ".", 2, NTFY_NORMAL, "white_check_mark");
    return;
  }
  const char* why = nullptr;
  switch (rr) {
    case ESP_RST_BROWNOUT:
      why = "Neustart: Strom war kurz zu schwach. Netzteil oder Kabel prüfen.";
      break;
    case ESP_RST_PANIC: case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT:
      why = "Neustart nach einem Absturz. Bitte Bescheid geben.";
      break;
    default: break;
  }
  Serial.printf("[System] Start-Grund %d (%s)\n", (int)rr,
                rr == ESP_RST_POWERON ? "eingeschaltet" : rr == ESP_RST_SW ? "Neustart per Software" :
                rr == ESP_RST_BROWNOUT ? "STROMEINBRUCH" : why ? "ABSTURZ" : "sonstiges");
  if (why) uiToast(why, true, 10000);
}

// ------------------------------------------------------------------ Start
void setup() {
  // Lautsprecher-Eingang SOFORT auf Masse. Bis das Ton-Modul startet (z. B. während der Touch-Einstellung)
  // wäre GPIO26 sonst offen und fängt den Takt der Touch-Leitung direkt daneben ein -> Rauschen.
  pinMode(26, OUTPUT);
  digitalWrite(26, LOW);

  Serial.begin(115200);
  delay(100);
  Serial.printf("\n\n=== Wolkenstation %s ===\n", WOLKE_FW_VERSION);

  pinMode(LED_R, OUTPUT); digitalWrite(LED_R, HIGH);
  pinMode(LED_G, OUTPUT); digitalWrite(LED_G, HIGH);
  pinMode(LED_B, OUTPUT); digitalWrite(LED_B, HIGH);

  storeLoad();

  lcd.init();
  // Hintergrundlicht-PWM von 12 kHz auf 44 kHz: ausserhalb des Hörbereichs, kein Pfeifen im Verstärker
  if (auto* bl = static_cast<lgfx::Light_PWM*>(lcd.light())) {
    auto cfg = bl->config();
    cfg.freq = 44100;
    bl->config(cfg);
    bl->init(190);
  }
  lcd.setRotation(S.rotation);
  lcd.setBrightness(190);
  lcd.fillScreen(TFT_WHITE);
  Serial.printf("[Display] erkannt: %s\n", lcd.getBoard() == lgfx::board_t::board_Sunton_ESP32_2432S028 ? "ESP32-2432S028" : "unbekannt");

  // Streifen-Puffer: so groß wie möglich, aber RAM für WLAN und Verschlüsselung übrig lassen
  band.setColorDepth(16);
  const int tryH[] = {60, 40, 30, 20};
  for (int h : tryH) {
    if (band.createSprite(320, h)) { bandH = h; break; }
  }
  Serial.printf("[Display] Zeichenpuffer 320x%d\n", bandH);

  // Touch: beim ersten Start oder wenn beim Einschalten der Bildschirm berührt wird -> einstellen
  uint16_t cal[8];
  lgfx::touch_point_t tp;
  bool held = lcd.getTouchRaw(&tp, 1) > 0;
  if (!storeLoadCal(cal) || held) {
    if (held) {
      lcd.fillScreen(TFT_WHITE);
      centerText("Bitte loslassen ...", 120, &fonts::FreeSansBold12pt7b, lcd.color565(0x3f, 0x35, 0x55));
      uint32_t t0 = millis();
      while (lcd.getTouchRaw(&tp, 1) > 0 && millis() - t0 < 8000) delay(20);
      delay(300);
    }
    sysCalibrateTouch();
  } else {
    lcd.setTouchCalibrate(cal);
  }
  pinMode(PIN_TOUCH_IRQ, INPUT);   // T_IRQ des Touch-Chips: LOW = Finger auf dem Bildschirm

  audioInit();
  netInit();
  linkInit();
  logicInit();
  uiInit();
  showResetReason();
  renderScreen();
  sysLoopWdt(true);                  // ab jetzt: hängt loop() länger als 5 s -> Neustart
  Serial.printf("[System] bereit, freier Speicher %u Bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.println("[Diagnose] Ziffer + Enter: 1 = WLAN aus, 2 = Ton-Modul aus, 3 = Bildschirm einfrieren, 4 = Licht aus");
}

// ------------------------------------------------------------------ Rausch-Diagnose über den Seriellen Monitor
// Ziffer eintippen + Enter: 1 = WLAN/Funk aus, 2 = Ton-Modul aus, 3 = Bildschirm+Touch einfrieren, 4 = Licht aus
static bool diagNoNet = false, diagFreeze = false, diagDark = false;
static void diagCommand(char c) {
  switch (c) {
    case '1':
      esp_now_deinit();
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      diagNoNet = true;
      Serial.println("[Diagnose] 1: WLAN und Funk AUS - rauscht es noch?");
      break;
    case '2':
      i2s_driver_uninstall(I2S_NUM_0);
      pinMode(26, OUTPUT);
      digitalWrite(26, LOW);
      Serial.println("[Diagnose] 2: Ton-Modul AUS (Ausgang auf Masse) - rauscht es noch?");
      break;
    case '3':
      diagFreeze = true;
      Serial.println("[Diagnose] 3: Bildschirm und Touch eingefroren - rauscht es noch?");
      break;
    case '4':
      diagDark = true;
      lcd.setBrightness(0);
      Serial.println("[Diagnose] 4: Display-Licht AUS - rauscht es noch?");
      break;
    default: break;
  }
}

// ------------------------------------------------------------------ Touch nur bei Berührung abfragen
// Der Touch-Chip hängt an Software-SPI über GPIO25 – direkt neben GPIO26 (Lautsprecher). Bis 0.1.3 wurde er bei
// jedem Schleifendurchlauf (~100x pro Sekunde) abgefragt -> der Takt landete als Brummen im Verstärker.
// Jetzt nur noch, wenn T_IRQ (GPIO36) eine Berührung meldet. Sicherheitsnetz: alle 3 s trotzdem einmal nachsehen.
// Findet das eine Berührung, obwohl T_IRQ "frei" meldet, ist die Leitung bei diesem Board nicht nutzbar
// -> ab dann wieder dauernd abfragen wie früher (Touch geht also auf keinen Fall verloren).
static bool touchIrqOk = true;

static bool readTouch(int32_t& x, int32_t& y) {
  if (!touchIrqOk || digitalRead(PIN_TOUCH_IRQ) == LOW) return lcd.getTouch(&x, &y) > 0;
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 3000) return false;
  lastCheck = millis();
  bool t = lcd.getTouch(&x, &y) > 0;
  if (t) {
    delayMicroseconds(200);                       // Touch-Chip hat T_IRQ nach dem Lesen wieder freigegeben
    if (digitalRead(PIN_TOUCH_IRQ) == HIGH) {     // Finger liegt drauf, T_IRQ sagt trotzdem "frei"
      touchIrqOk = false;
      Serial.println("[Touch] T_IRQ meldet keine Berührung - frage ab jetzt wieder dauernd ab");
    }
  }
  return t;
}

// ------------------------------------------------------------------ Hauptschleife
void loop() {
  while (Serial.available()) diagCommand((char)Serial.read());

  if (!diagNoNet) { netLoop(); linkLoop(); updLoop(); }
  logicTick();

  if (!diagFreeze) {
    int32_t x = 0, y = 0;
    bool touched = readTouch(x, y);
    uiTouch(touched, x, y);
    if (uiTick() && bandH) renderScreen();
  }

  uint8_t bl = diagDark ? 0 : uiBacklight();
  if (bl != curBacklight) { lcd.setBrightness(bl); curBacklight = bl; }

  static uint32_t lastHeap = 0;
  if (millis() - lastHeap > 600000) {
    lastHeap = millis();
    Serial.printf("[System] freier Speicher %u, kleinster Block %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  }
  delay(8);
}
