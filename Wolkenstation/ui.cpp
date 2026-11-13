// Wolkenstation – Oberfläche (320 x 240, Touch).
// Gezeichnet wird in Streifen (Sprite 320 x N), dadurch flackert nichts und es reicht wenig RAM.
// Plattformneutral: läuft auf dem ESP32 und im PC-Test (Bilder aller Ansichten).
#include "app.h"
#include <LovyanGFX.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

extern "C" {
extern const uint8_t u8g2_font_helvB08_tf[];
extern const uint8_t u8g2_font_helvB10_tf[];
extern const uint8_t u8g2_font_helvB12_tf[];
extern const uint8_t u8g2_font_helvB14_tf[];
extern const uint8_t u8g2_font_helvB18_tf[];
extern const uint8_t u8g2_font_helvB24_tf[];
extern const uint8_t u8g2_font_helvR08_tf[];
extern const uint8_t u8g2_font_helvR10_tf[];
extern const uint8_t u8g2_font_fub30_tn[];
}

static const lgfx::U8g2font fTiny (u8g2_font_helvB08_tf);
static const lgfx::U8g2font fTinyR(u8g2_font_helvR08_tf);
static const lgfx::U8g2font fSm   (u8g2_font_helvB10_tf);
static const lgfx::U8g2font fBody (u8g2_font_helvR10_tf);
static const lgfx::U8g2font fTitle(u8g2_font_helvB12_tf);
static const lgfx::U8g2font fH2   (u8g2_font_helvB14_tf);
static const lgfx::U8g2font fH1   (u8g2_font_helvB18_tf);
static const lgfx::U8g2font fXL   (u8g2_font_helvB24_tf);
static const lgfx::U8g2font fClock(u8g2_font_fub30_tn);
typedef const lgfx::IFont* Font;

using lgfx::textdatum_t;
static constexpr textdatum_t DT_L = textdatum_t::middle_left;
static constexpr textdatum_t DT_C = textdatum_t::middle_center;
static constexpr textdatum_t DT_R = textdatum_t::middle_right;

// ------------------------------------------------------------------ Farben
static constexpr uint16_t rgb(uint32_t c) {
  return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}
static const uint16_t C_INK = rgb(0x3f3555), C_INK2 = rgb(0x2b2540), C_MUTED = rgb(0x7a7090),
  C_TEXT = rgb(0x5b5170), C_BG = rgb(0xf6f3fb), C_BG2 = rgb(0xf1edf8), C_WHITE = rgb(0xffffff),
  C_LINE = rgb(0xe4def0), C_LINE2 = rgb(0xeee9f6), C_CHIP = rgb(0xefebf7), C_ACC = rgb(0x6b57c9),
  C_ACCD = rgb(0x5644a8), C_BTN = rgb(0xe9e4f5), C_BTND = rgb(0xd6cfea), C_WARNBG = rgb(0xfbe4e4),
  C_WARNFG = rgb(0xa53030), C_OKBG = rgb(0xe3f6ee), C_OKFG = rgb(0x1d6a4c), C_BADBG = rgb(0xfde3e3),
  C_BADFG = rgb(0x9a2a2a), C_TOG = rgb(0xcfc8de), C_TOGON = rgb(0x2f9d7b), C_YEL = rgb(0xffd23f),
  C_GOLD = rgb(0xffb800), C_BAR = rgb(0x9a7cc2), C_TRK = rgb(0xebe6f5), C_KSH = rgb(0xd8d1e6),
  C_KFN = rgb(0xe4def0), C_HACK = rgb(0xb5361a), C_TOAST = rgb(0x2b2540), C_SUB = rgb(0xffe7a3),
  C_CLOUD = rgb(0x9fcbf0), C_CHEEK = rgb(0xf4a6bd), C_FACE = rgb(0x4b3a63), C_WARNCARD = rgb(0xfff1d1),
  C_WARNCARDFG = rgb(0x6b4800), C_RED = rgb(0xe05252), C_BLACK = 0, C_GREYOFF = rgb(0x57536a),
  C_SIGOFF = rgb(0xcfc6e6);

static uint32_t lerp888(uint32_t a, uint32_t b, float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  int r = ((a >> 16) & 255) + (int)((((int)((b >> 16) & 255)) - (int)((a >> 16) & 255)) * t);
  int g = ((a >> 8) & 255) + (int)((((int)((b >> 8) & 255)) - (int)((a >> 8) & 255)) * t);
  int bl = (a & 255) + (int)((((int)(b & 255)) - (int)(a & 255)) * t);
  return (uint32_t)(r << 16 | g << 8 | bl);
}
static uint16_t to565(uint32_t c) { return rgb(c); }
static uint32_t mix888(uint32_t base, uint32_t over, float alpha) { return lerp888(base, over, alpha); }

enum Sky : uint8_t { SKY_DAY, SKY_MORNING, SKY_NIGHT, SKY_HELP };
static uint32_t skyAt(Sky s, int y) {
  float t = y / 239.0f;
  switch (s) {
    case SKY_NIGHT:   return lerp888(0x1f2350, 0x3b3270, t);
    case SKY_MORNING: return t < 0.6f ? lerp888(0x4a4f9e, 0x9a7cc2, t / 0.6f) : lerp888(0x9a7cc2, 0xf5a8a0, (t - 0.6f) / 0.4f);
    case SKY_HELP:    return lerp888(0xd9532f, 0xf0993a, t);
    default:          return lerp888(0x7fb6e8, 0xb6d8f2, t);
  }
}

// ------------------------------------------------------------------ Zeichenfläche mit Streifen-Versatz
struct Cv {
  lgfx::LovyanGFX* g;
  int oy;
  bool collect;
  bool vis(int y, int h) const { return y + h > oy && y < oy + g->height(); }
  void rect(int x, int y, int w, int h, uint16_t c) { if (vis(y, h)) g->fillRect(x, y - oy, w, h, c); }
  void rrect(int x, int y, int w, int h, int r, uint16_t c) { if (vis(y, h)) g->fillSmoothRoundRect(x, y - oy, w, h, r, c); }
  void rframe(int x, int y, int w, int h, int r, uint16_t c) { if (vis(y, h)) g->drawRoundRect(x, y - oy, w, h, r, c); }
  void hline(int x, int y, int w, uint16_t c) { if (vis(y, 1)) g->drawFastHLine(x, y - oy, w, c); }
  void circle(int x, int y, int r, uint16_t c) { if (vis(y - r, 2 * r + 1)) g->fillSmoothCircle(x, y - oy, r, c); }
  void arc(int x, int y, int r0, int r1, float a0, float a1, uint16_t c) { if (vis(y - r1, 2 * r1 + 1)) g->fillArc(x, y - oy, r0, r1, a0, a1, c); }
  void line(int x0, int y0, int x1, int y1, float w, uint16_t c) {
    int top = (y0 < y1 ? y0 : y1) - 3, bot = (y0 > y1 ? y0 : y1) + 3;
    if (vis(top, bot - top)) g->drawWideLine(x0, y0 - oy, x1, y1 - oy, w, c);
  }
  void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) { g->fillTriangle(x0, y0 - oy, x1, y1 - oy, x2, y2 - oy, c); }
  void ellipse(int x, int y, int rx, int ry, uint16_t c) { if (vis(y - ry, 2 * ry + 1)) g->fillEllipse(x, y - oy, rx, ry, c); }
  void alpha(int x, int y, int w, int h, uint8_t a, uint16_t c) { if (vis(y, h)) g->fillRectAlpha(x, y - oy, w, h, a, c); }
  int text(const char* s, int x, int y, Font f, uint16_t c, textdatum_t d = DT_L) {
    g->setFont(f); g->setTextColor(c); g->setTextDatum(d);
    if (vis(y - 30, 60)) g->drawString(s, x, y - oy);
    return g->textWidth(s);
  }
  int tw(const char* s, Font f) { g->setFont(f); return g->textWidth(s); }
  // Verlauf mit Fehlerverteilung von Zeile zu Zeile, damit in 16 Bit Farbe keine Streifen entstehen
  void gradient(Sky s, int y0, int y1) {
    int a = y0 > oy ? y0 : oy, b = y1 < oy + g->height() ? y1 : oy + g->height();
    float er = 0, eg = 0, eb = 0;
    for (int y = y0; y < b; y++) {
      uint32_t c = skyAt(s, y);
      float r = ((c >> 16) & 255) + er, gg = ((c >> 8) & 255) + eg, bb = (c & 255) + eb;
      int qr = (int)(r / 8.225f + 0.5f), qg = (int)(gg / 4.048f + 0.5f), qb = (int)(bb / 8.225f + 0.5f);
      qr = qr < 0 ? 0 : (qr > 31 ? 31 : qr);
      qg = qg < 0 ? 0 : (qg > 63 ? 63 : qg);
      qb = qb < 0 ? 0 : (qb > 31 ? 31 : qb);
      er = r - qr * 8.225f; eg = gg - qg * 4.048f; eb = bb - qb * 8.225f;
      if (y >= a) g->drawFastHLine(0, y - oy, 320, (uint16_t)(qr << 11 | qg << 5 | qb));
    }
  }
};

// ------------------------------------------------------------------ Aktionen und Trefferflächen
enum Act : uint8_t {
  A_NONE, A_GO, A_WAKE, A_PICKNET, A_RESCAN, A_LISTUP, A_LISTDN, A_KEY, A_SHIFT, A_SYM, A_BKSP,
  A_SPACE, A_KBOK, A_EYE, A_RETRYKB, A_FINISH, A_STEP, A_MEL, A_PREV, A_VOL, A_HELPMODE, A_HELPREP,
  A_HELPPREV, A_NTFYTOG, A_NTFYTEST, A_REPAIR, A_ASKRESET, A_ASKFACTORY, A_CNO, A_CYES, A_HELPACK,
  A_EDITNAME, A_ROTATE, A_CALIB, A_TESTWAKE, A_TESTHELP, A_GEAR, A_STATS, A_PORTAL, A_SCREENWIFI,
  A_PAIRLATER, A_CONTINUE, A_BRI, A_MUTE, A_OTARETRY, A_UPDCHECK, A_UPDINSTALL
};
enum Scr : uint8_t {
  SC_WELCOME, SC_HSQR, SC_HSWAIT, SC_WLIST, SC_KB, SC_CONNECTING, SC_CONNERR, SC_WIFIOK, SC_PAIR,
  SC_PAIROK, SC_NTFY, SC_DONE, SC_HOME, SC_STATS, SC_SETTINGS, SC_ZEITEN, SC_TON, SC_HILFE, SC_KNOPF,
  SC_WLAN, SC_WCHOOSE, SC_SNTFY, SC_NAME, SC_SSTATS, SC_SYSTEM, SC_DISPLAY, SC_OTA, SC_UPDATE
};
struct Hit { int16_t x, y, w, h; uint8_t act; int16_t arg; };
static Hit hits[64];
static int nHits = 0;

struct Ui {
  uint8_t scr = SC_WELCOME, statsBack = SC_HOME, kbBack = SC_WLIST, kbTarget = 0, ntfyBack = SC_SETTINGS;
  uint32_t gen = 0;
  bool dirty = true;
  char toast[72] = {0}; bool toastBig = false; uint32_t toastUntil = 0;
  uint8_t confirm = 0;                         // 1 = Statistik löschen, 2 = Werkseinstellungen, 3 = Handy-Nachrichten aus
  char kbVal[66] = {0}; bool kbShift = false, kbSym = false, kbShow = false;
  int scroll = 0, scrollMax = 0, dragStartScroll = 0;
  bool down = false, dragging = false, swallow = false;
  int downX = 0, downY = 0;
  bool pressValid = false; Hit press{}; uint32_t pressGen = 0;
  bool holding = false; uint32_t holdStart = 0;
  uint32_t awakeUntil = 0;
  char selSsid[33] = {0}; bool selLock = true;
  NetState lastNet = NET_IDLE;
  bool lastPortalClient = false;
  uint32_t animMs = 0;
  int lastMinute = -2;
  int listOff = 0;
  bool wasOff = false;
  bool testPending = false;
  uint8_t briPreview = 0; uint32_t briPreviewUntil = 0;   // Helligkeit beim Einstellen kurz zeigen
  bool lastOta = false;
  uint8_t updBack = SC_SETTINGS, otaBack = SC_SYSTEM, lastUpd = 0; int lastUpdPct = -1;
  int clipTop = 0;
} U;

static void hit(Cv& c, int x, int y, int w, int h, uint8_t act, int16_t arg = 0) {
  if (!c.collect || nHits >= 64) return;
  if (y < U.clipTop) { h -= U.clipTop - y; y = U.clipTop; }
  if (h <= 0) return;
  hits[nHits++] = Hit{(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, act, arg};
}
static bool pressed(int x, int y, int w, int h) {
  return U.pressValid && U.pressGen == U.gen && U.press.x == x && U.press.w == w &&
         (U.press.y == y || (U.press.y > y && U.press.y < y + h)) && U.press.h <= h;
}

void uiDirty() { U.dirty = true; }
void uiWake(uint32_t ms) {
  uint32_t t = msNow() + ms;
  if ((int32_t)(t - U.awakeUntil) > 0) U.awakeUntil = t;
  U.dirty = true;
}
void uiToast(const char* text, bool big, uint16_t ms) {
  strncpy(U.toast, text, sizeof(U.toast) - 1);
  U.toast[sizeof(U.toast) - 1] = 0;
  U.toastBig = big;
  U.toastUntil = msNow() + ms;
  U.dirty = true;
}

static void go(uint8_t s);

// ------------------------------------------------------------------ Text-Helfer
static void utf8Trim(char* s) {   // letztes (evtl. mehrbytiges) Zeichen entfernen
  size_t n = strlen(s);
  if (!n) return;
  n--;
  while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
  s[n] = 0;
}
static int utf8Len(const char* s) { int n = 0; for (; *s; s++) if (((uint8_t)*s & 0xC0) != 0x80) n++; return n; }

static void fit(Cv& c, const char* s, int maxW, Font f, char* out, size_t n) {
  strncpy(out, s, n - 1); out[n - 1] = 0;
  if (c.tw(out, f) <= maxW) return;
  char tmp[96];
  while (strlen(out) > 0) {
    utf8Trim(out);
    snprintf(tmp, sizeof(tmp), "%s...", out);
    if (c.tw(tmp, f) <= maxW) { strncpy(out, tmp, n - 1); out[n - 1] = 0; return; }
  }
}

// Zeilenumbruch an Leerzeichen. align: 0 links, 1 mittig. Gibt Höhe zurück.
static int wrap(Cv& c, const char* s, int x, int y, int w, Font f, uint16_t col, int lh, int align = 0, bool draw = true) {
  char line[128], wbuf[96];
  int lines = 0;
  line[0] = 0;
  const char* p = s;
  auto flush = [&]() {
    if (draw) {
      if (align == 1) c.text(line, x + w / 2, y + lines * lh, f, col, DT_C);
      else c.text(line, x, y + lines * lh, f, col, DT_L);
    }
    lines++;
    line[0] = 0;
  };
  while (*p) {
    size_t wl = 0;
    while (*p == ' ') p++;
    if (*p == '\n') { flush(); p++; continue; }
    while (*p && *p != ' ' && *p != '\n' && wl < sizeof(wbuf) - 1) wbuf[wl++] = *p++;
    wbuf[wl] = 0;
    if (!wl) continue;
    char test[224];
    snprintf(test, sizeof(test), "%s%s%s", line, line[0] ? " " : "", wbuf);
    if (line[0] && c.tw(test, f) > w) { flush(); strncpy(line, wbuf, sizeof(line) - 1); line[sizeof(line) - 1] = 0; }
    else { strncpy(line, test, sizeof(line) - 1); line[sizeof(line) - 1] = 0; }
  }
  if (line[0]) flush();
  return lines * lh;
}

// ------------------------------------------------------------------ Symbole
static void iconWifi(Cv& c, int x, int y, uint16_t col, bool ok) {  // x,y = links oben, 16 x 12
  int cx = x + 8, cy = y + 12;
  uint16_t cc = ok ? col : C_TOG;
  c.circle(cx, cy - 1, 2, cc);
  c.arc(cx, cy, 5, 7, 225, 315, cc);
  c.arc(cx, cy, 9, 11, 225, 315, cc);
  if (!ok) c.line(x + 2, y, x + 14, y + 12, 1.2f, C_RED);
}
static void iconBat(Cv& c, int x, int y, int pct, uint16_t col, float s = 1.0f) {  // 22 x 11
  int w = (int)(18 * s), h = (int)(10 * s);
  uint16_t fill = (pct >= 0 && pct <= 20) ? rgb(0xf0a020) : col;
  c.rframe(x, y, w, h, (int)(2 * s), col);
  c.rframe(x + 1, y + 1, w - 2, h - 2, (int)(2 * s), col);
  c.rect(x + w, y + (int)(3 * s), (int)(2 * s), h - (int)(6 * s), col);
  int fw = pct < 0 ? 0 : (int)((w - 6) * pct / 100.0f);
  if (pct > 0 && fw < 2) fw = 2;
  if (fw > 0) c.rect(x + 3, y + 3, fw, h - 6, fill);
}
static void iconGear(Cv& c, int cx, int cy, uint16_t col, uint16_t bg) {
  for (int i = 0; i < 8; i++) {
    float a = i * 3.14159265f / 4;
    c.line(cx + (int)lroundf(cosf(a) * 5), cy + (int)lroundf(sinf(a) * 5), cx + (int)lroundf(cosf(a) * 9), cy + (int)lroundf(sinf(a) * 9), 1.6f, col);
  }
  c.circle(cx, cy, 7, col);
  c.circle(cx, cy, 3, bg);
}
static void iconChart(Cv& c, int x, int y, uint16_t col) {  // 16 x 16
  c.rrect(x, y + 7, 4, 9, 1, col);
  c.rrect(x + 6, y + 1, 4, 15, 1, col);
  c.rrect(x + 12, y + 4, 4, 12, 1, col);
}
static void iconBack(Cv& c, int cx, int cy, uint16_t col) {
  c.line(cx + 3, cy - 5, cx - 2, cy, 1.1f, col);
  c.line(cx - 2, cy, cx + 3, cy + 5, 1.1f, col);
}
static void iconChev(Cv& c, int cx, int cy, uint16_t col) {
  c.line(cx - 2, cy - 4, cx + 2, cy, 1.0f, col);
  c.line(cx + 2, cy, cx - 2, cy + 4, 1.0f, col);
}
static void iconLock(Cv& c, int x, int y, uint16_t col) {  // 9 x 11
  c.arc(x + 4, y + 4, 2, 4, 180, 360, col);
  c.rect(x, y + 4, 2, 2, col); c.rect(x + 7, y + 4, 2, 2, col);
  c.rrect(x, y + 5, 9, 6, 1, col);
}
static void iconSig(Cv& c, int x, int y, int bars, uint16_t col) {  // 15 x 11
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 3;
    c.rect(x + i * 4, y + 11 - h, 3, h, i < bars ? col : C_SIGOFF);
  }
}
static void iconPlay(Cv& c, int cx, int cy, uint16_t col) { c.tri(cx - 3, cy - 5, cx - 3, cy + 5, cx + 5, cy, col); }
static void iconCheck(Cv& c, int cx, int cy, uint16_t col) {
  c.line(cx - 4, cy, cx - 1, cy + 3, 1.2f, col);
  c.line(cx - 1, cy + 3, cx + 4, cy - 3, 1.2f, col);
}
static void iconClock(Cv& c, int cx, int cy, uint16_t col, uint16_t bg) {
  c.circle(cx, cy, 7, col); c.circle(cx, cy, 5, bg);
  c.line(cx, cy, cx, cy - 4, 0.8f, col); c.line(cx, cy, cx + 3, cy + 1, 0.8f, col);
}
static void iconNote(Cv& c, int cx, int cy, uint16_t col) {
  c.ellipse(cx - 4, cy + 4, 3, 2, col); c.ellipse(cx + 4, cy + 3, 3, 2, col);
  c.line(cx - 1, cy + 4, cx - 1, cy - 6, 0.8f, col); c.line(cx + 7, cy + 3, cx + 7, cy - 7, 0.8f, col);
  c.line(cx - 1, cy - 6, cx + 7, cy - 7, 1.4f, col);
}
static void iconCloud(Cv& c, int cx, int cy, uint16_t col) {
  c.circle(cx - 3, cy + 1, 4, col); c.circle(cx + 2, cy - 1, 5, col); c.circle(cx + 6, cy + 2, 3, col);
  c.rrect(cx - 7, cy + 1, 15, 5, 2, col);
}
static void iconPhone(Cv& c, int cx, int cy, uint16_t col, uint16_t bg, float s = 1.0f) {
  int w = (int)(10 * s), h = (int)(16 * s);
  c.rrect(cx - w / 2, cy - h / 2, w, h, (int)(2 * s), col);
  c.rrect(cx - w / 2 + (int)(2 * s), cy - h / 2 + (int)(2 * s), w - (int)(4 * s), h - (int)(5 * s), 1, bg);
}
static void iconPerson(Cv& c, int cx, int cy, uint16_t col) {
  c.circle(cx, cy - 3, 3, col);
  c.arc(cx, cy + 7, 0, 6, 180, 360, col);
}
static void iconChip(Cv& c, int cx, int cy, uint16_t col, uint16_t bg) {
  c.rrect(cx - 5, cy - 5, 11, 11, 2, col); c.rrect(cx - 3, cy - 3, 7, 7, 1, bg);
  for (int i = -1; i <= 1; i += 2) {
    c.rect(cx - 3, cy + i * 7 - (i < 0 ? 0 : 1), 2, 2, col); c.rect(cx + 2, cy + i * 7 - (i < 0 ? 0 : 1), 2, 2, col);
    c.rect(cx + i * 7 - (i < 0 ? 0 : 1), cy - 3, 2, 2, col); c.rect(cx + i * 7 - (i < 0 ? 0 : 1), cy + 2, 2, 2, col);
  }
}
static void iconBell(Cv& c, int cx, int cy, uint16_t col) {
  c.arc(cx, cy - 1, 0, 5, 180, 360, col);
  c.rect(cx - 5, cy - 1, 11, 4, col);
  c.rect(cx - 7, cy + 3, 15, 2, col);
  c.circle(cx, cy + 6, 2, col);
}
static void iconShift(Cv& c, int cx, int cy, uint16_t col) {
  c.tri(cx, cy - 7, cx - 7, cy, cx + 7, cy, col);
  c.rect(cx - 3, cy, 7, 6, col);
}
static void iconBksp(Cv& c, int cx, int cy, uint16_t col, uint16_t bg) {
  c.tri(cx - 9, cy, cx - 4, cy - 6, cx - 4, cy + 6, col);
  c.rect(cx - 4, cy - 6, 13, 13, col);
  c.line(cx - 1, cy - 3, cx + 5, cy + 3, 0.8f, bg); c.line(cx - 1, cy + 3, cx + 5, cy - 3, 0.8f, bg);
}

static void iconSpeaker(Cv& c, int cx, int cy, uint16_t col, bool muted) {
  c.rect(cx - 8, cy - 3, 4, 7, col);
  c.tri(cx - 4, cy - 3, cx + 2, cy - 8, cx + 2, cy + 8, col);
  c.tri(cx - 4, cy - 3, cx + 2, cy + 8, cx - 4, cy + 4, col);
  if (muted) {
    c.line(cx + 5, cy - 4, cx + 11, cy + 4, 1.4f, col);
    c.line(cx + 5, cy + 4, cx + 11, cy - 4, 1.4f, col);
  } else {
    c.arc(cx + 2, cy, 4, 6, -50, 50, col);
    c.arc(cx + 2, cy, 8, 10, -50, 50, col);
  }
}
static void iconDownload(Cv& c, int cx, int cy, uint16_t col) {
  c.line(cx, cy - 7, cx, cy + 3, 1.4f, col);
  c.line(cx - 4, cy - 1, cx, cy + 3, 1.4f, col);
  c.line(cx + 4, cy - 1, cx, cy + 3, 1.4f, col);
  c.rect(cx - 7, cy + 6, 15, 2, col);
}
static void iconSun(Cv& c, int cx, int cy, uint16_t col) {
  for (int i = 0; i < 8; i++) {
    float a = i * 3.14159265f / 4;
    c.line(cx + (int)lroundf(cosf(a) * 6), cy + (int)lroundf(sinf(a) * 6), cx + (int)lroundf(cosf(a) * 8), cy + (int)lroundf(sinf(a) * 8), 1.0f, col);
  }
  c.circle(cx, cy, 4, col);
}

// Maskottchen-Wolke (Vorlage 120 x 74), x/y = links oben
static void mascot(Cv& c, int x, int y, float s) {
  auto X = [&](float v) { return x + (int)lroundf(v * s); };
  auto Y = [&](float v) { return y + (int)lroundf(v * s); };
  auto R = [&](float v) { int r = (int)lroundf(v * s); return r < 1 ? 1 : r; };
  c.ellipse(X(62), Y(22), R(12), R(8), C_YEL);
  c.circle(X(38), Y(44), R(20), C_CLOUD);
  c.circle(X(62), Y(32), R(26), C_CLOUD);
  c.circle(X(88), Y(44), R(19), C_CLOUD);
  c.rrect(X(18), Y(44), R(88), R(24), R(12), C_CLOUD);
  c.arc(X(50), Y(48), R(3.2f), R(5.6f), 25, 155, C_FACE);
  c.arc(X(72), Y(48), R(3.2f), R(5.6f), 25, 155, C_FACE);
  c.arc(X(61), Y(55), R(3.0f), R(5.4f), 25, 155, C_FACE);
  c.ellipse(X(42), Y(58), R(5), R(3), C_CHEEK);
  c.ellipse(X(82), Y(58), R(5), R(3), C_CHEEK);
}

// ------------------------------------------------------------------ Bausteine
enum { B_NORM, B_PRI, B_WARN, B_ACK, B_LIGHT };
static void button(Cv& c, int x, int y, int w, int h, const char* label, uint8_t style, uint8_t act, int16_t arg = 0,
                   const char* sub = nullptr, Font f = &fSm) {
  bool p = pressed(x, y, w, h);
  uint16_t bg = C_BTN, fg = C_INK;
  if (style == B_PRI)  { bg = p ? C_ACCD : C_ACC; fg = C_WHITE; }
  else if (style == B_WARN) { bg = p ? rgb(0xf3cccc) : C_WARNBG; fg = C_WARNFG; }
  else if (style == B_ACK)  { bg = p ? rgb(0xffe2d6) : C_WHITE; fg = C_HACK; }
  else if (style == B_LIGHT){ bg = p ? C_BTND : C_WHITE; fg = C_INK; }
  else if (p) bg = C_BTND;
  c.rrect(x, y, w, h, 9, bg);
  if (sub) {
    c.text(label, x + w / 2, y + h / 2 - 7, f, fg, DT_C);
    c.text(sub, x + w / 2, y + h / 2 + 9, &fTinyR, fg, DT_C);
  } else {
    c.text(label, x + w / 2, y + h / 2, f, fg, DT_C);
  }
  hit(c, x, y, w, h, act, arg);
}
static void smallBtn(Cv& c, int x, int y, const char* label, uint8_t style, uint8_t act, int16_t arg = 0, int h = 26) {
  int w = c.tw(label, &fSm) + 22;
  button(c, x, y, w, h, label, style, act, arg);
}
static int smallBtnW(Cv& c, const char* label) { return c.tw(label, &fSm) + 22; }

static void badge(Cv& c, int cx, int y, const char* label, uint8_t kind) {  // 0 ok, 1 bad, 2 warn
  uint16_t bg = kind == 1 ? C_BADBG : kind == 2 ? C_WARNCARD : C_OKBG;
  uint16_t fg = kind == 1 ? C_BADFG : kind == 2 ? C_WARNCARDFG : C_OKFG;
  int w = c.tw(label, &fTiny) + (kind == 0 ? 34 : 20);
  c.rrect(cx - w / 2, y, w, 20, 10, bg);
  if (kind == 0) { iconCheck(c, cx - w / 2 + 13, y + 10, fg); c.text(label, cx - w / 2 + 23, y + 10, &fTiny, fg, DT_L); }
  else c.text(label, cx, y + 10, &fTiny, fg, DT_C);
}

static void toggle(Cv& c, int x, int y, bool on, uint8_t act) {
  c.rrect(x, y, 40, 22, 11, on ? C_TOGON : C_TOG);
  c.circle(on ? x + 29 : x + 11, y + 11, 8, C_WHITE);
  hit(c, x - 8, y - 6, 56, 34, act);
}

static void panel(Cv& c, int x, int y, int w, int h) { c.rrect(x, y, w, h, 9, C_WHITE); }

static void spinner(Cv& c, int cx, int cy, int r, uint16_t col, uint16_t track) {
  float a = (msNow() / 3) % 360;
  c.arc(cx, cy, r - 4, r, 0, 360, track);
  c.arc(cx, cy, r - 4, r, a, a + 100, col);
}

static void header(Cv& c, const char* title, int step, int back) {
  c.rect(0, 0, 320, 30, C_WHITE);
  c.hline(0, 30, 320, C_LINE);
  int x = 10;
  if (back >= 0) {
    bool p = pressed(0, 0, 44, 30);
    c.rrect(6, 4, 24, 22, 7, p ? C_BTND : C_BTN);
    iconBack(c, 17, 15, C_INK);
    x = 38;
    hit(c, 0, 0, 44, 30, A_GO, back);
  }
  if (step > 0) {
    char s[16]; snprintf(s, sizeof(s), "%d/3", step);
    c.rrect(x, 7, 30, 16, 8, C_CHIP);
    c.text(s, x + 15, 15, &fTiny, C_ACC, DT_C);
    x += 38;
  }
  char t[64]; fit(c, title, 320 - x - 8, &fTitle, t, sizeof(t));
  c.text(t, x, 15, &fTitle, C_INK, DT_L);
}

static void scrollBar(Cv& c) {
  if (U.scrollMax <= 0) return;
  int track = 240 - 34;
  int vis = 209;
  int h = track * vis / (vis + U.scrollMax);
  int y = 32 + (track - h) * U.scroll / U.scrollMax;
  c.rrect(315, y, 3, h, 1, C_TOG);
}

static void setScrollMax(Cv& c, int contentBottom) {
  if (!c.collect) return;
  int m = contentBottom - 236;
  U.scrollMax = m > 0 ? m : 0;
  if (U.scroll > U.scrollMax) { U.scroll = U.scrollMax; U.dirty = true; }
}

// ------------------------------------------------------------------ Statusleiste (Startseite)
static void statusBar(Cv& c, uint16_t col) {
  char hm[6]; fmtHM(minuteNow(), hm);
  c.text(hm, 8, 11, &fTiny, col, DT_L);
  if (updRepoSet() && updState() == UPD_AVAILABLE) {     // Hinweis: neue Version, antippen zum Ansehen
    char t[28]; snprintf(t, sizeof(t), "Update %s", updVersion());
    int w = c.tw(t, &fTiny) + 22;
    c.rrect(160 - w / 2, 2, w, 18, 9, C_YEL);
    c.text(t, 160, 11, &fTiny, C_INK2, DT_C);
    hit(c, 160 - w / 2 - 6, 0, w + 12, 26, A_GO, SC_UPDATE);
  }
  int x = 312;
  iconWifi(c, x - 16, 3, col, netOnline());
  x -= 24;
  if (K.paired && K.pct >= 0) {
    char p[8]; snprintf(p, sizeof(p), "%d%%", K.pct);
    int w = c.tw(p, &fTiny);
    c.text(p, x, 11, &fTiny, col, DT_R);
    iconBat(c, x - w - 25, 6, K.pct, col);
  }
}

// ================================================================== Ansichten
static void scWelcome(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  {   // falls das Bild auf dem Kopf steht: hier drehen (später auch unter Einstellungen > System)
    int w = c.tw("Bild drehen", &fTiny) + 18;
    button(c, 314 - w, 6, w, 20, "Bild drehen", B_LIGHT, A_ROTATE, 0, nullptr, &fTiny);
  }
  mascot(c, 112, 10, 0.8f);
  c.text("Hallo!", 160, 88, &fH2, C_INK, DT_C);
  if (netOnline()) {
    char t[80]; snprintf(t, sizeof(t), "WLAN ist schon verbunden: %s", netSsid());
    char f2[80]; fit(c, t, 290, &fBody, f2, sizeof(f2));
    c.text(f2, 160, 112, &fBody, C_TEXT, DT_C);
    c.text("Jetzt noch den Wolkenknopf koppeln.", 160, 130, &fBody, C_TEXT, DT_C);
    button(c, 50, 152, 220, 36, "Weiter", B_PRI, A_GO, SC_PAIR);
    button(c, 50, 196, 220, 32, "Anderes WLAN", B_NORM, A_SCREENWIFI);
  } else {
    c.text("Lass uns die Wolkenstation einrichten.", 160, 112, &fBody, C_TEXT, DT_C);
    c.text("Das dauert etwa zwei Minuten.", 160, 130, &fBody, C_TEXT, DT_C);
    button(c, 50, 150, 220, 42, "Mit dem Handy einrichten", B_PRI, A_PORTAL, 0, "empfohlen");
    button(c, 50, 198, 220, 32, "Hier am Bildschirm", B_NORM, A_SCREENWIFI);
  }
}

static void scWChoose(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  header(c, "WLAN wechseln", 0, SC_WLAN);
  c.text("Wie möchtest du das neue WLAN eingeben?", 160, 70, &fBody, C_TEXT, DT_C);
  button(c, 50, 96, 220, 42, "Mit dem Handy", B_PRI, A_PORTAL, 0, "empfohlen");
  button(c, 50, 146, 220, 34, "Hier am Bildschirm", B_NORM, A_SCREENWIFI);
}

static void scHsQr(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  bool setup = !S.setupDone;
  header(c, setup ? "WLAN verbinden" : "WLAN wechseln", setup ? 1 : 0, setup ? SC_WELCOME : SC_WCHOOSE);
  panel(c, 8, 38, 128, 128);
  char qr[128];
  snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", netApSsid(), netApPass());
  if (c.vis(42, 120)) c.g->qrcode(qr, 12, 42 - c.oy, 120, 3);
  int x = 146;
  c.text("Mit dem Handy scannen", x, 48, &fSm, C_INK, DT_L);
  int h = wrap(c, "Kamera öffnen und auf den Code halten. Die Einrichtung öffnet sich dann von selbst.", x, 68, 166, &fBody, C_TEXT, 15);
  int y = 68 + h + 2;
  c.rrect(x - 2, y - 4, 170, 38, 7, C_WHITE);
  char t[64];
  snprintf(t, sizeof(t), "WLAN: %s", netApSsid()); c.text(t, x + 4, y + 6, &fTiny, C_INK, DT_L);
  snprintf(t, sizeof(t), "Passwort: %s", netApPass()); c.text(t, x + 4, y + 22, &fTiny, C_INK, DT_L);
  wrap(c, "Kein QR-Scanner? Mit dem WLAN oben verbinden und 192.168.4.1 im Browser öffnen.", 10, 186, 300, &fTinyR, C_MUTED, 13);
  smallBtn(c, 10, 210, "Lieber hier am Bildschirm", B_NORM, A_SCREENWIFI);
}

static void scHsWait(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  bool setup = !S.setupDone;
  header(c, setup ? "WLAN verbinden" : "WLAN wechseln", setup ? 1 : 0, -1);
  iconPhone(c, 160, 82, C_ACC, C_BG, 2.6f);
  c.text("Handy ist verbunden", 160, 132, &fH2, C_INK, DT_C);
  wrap(c, "Wähle jetzt am Handy dein WLAN aus und gib das Passwort ein.", 30, 158, 260, &fBody, C_TEXT, 16, 1);
  smallBtn(c, 160 - smallBtnW(c, "Abbrechen") / 2, 200, "Abbrechen", B_NORM, A_GO, setup ? SC_WELCOME : SC_WLAN);
}

static void scWList(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  bool setup = !S.setupDone;
  header(c, setup ? "WLAN wählen" : "WLAN wechseln", setup ? 1 : 0, setup ? SC_WELCOME : SC_WCHOOSE);
  c.rect(0, 31, 320, 24, C_BG2);
  c.text("Nur 2,4-GHz-Netze werden angezeigt", 10, 43, &fTinyR, C_MUTED, DT_L);
  const int kRows = 5, kRH = 32;
  int n = netScanCount();
  if (!netScanBusy()) {
    int w = c.tw("Neu suchen", &fTiny) + 18;
    button(c, 314 - w, 34, w, 18, "Neu suchen", B_LIGHT, A_RESCAN, 0, nullptr, &fTiny);
  }
  if (netScanBusy() && n == 0) {
    spinner(c, 160, 130, 16, C_ACC, C_LINE);
    c.text("Suche WLAN-Netze ...", 160, 165, &fSm, C_TEXT, DT_C);
    return;
  }
  if (n == 0) {
    c.text("Keine WLAN-Netze gefunden.", 160, 120, &fSm, C_INK, DT_C);
    c.text("Ist der Router an und in der Nähe?", 160, 142, &fBody, C_TEXT, DT_C);
    return;
  }
  if (U.listOff > n - 1) U.listOff = 0;
  for (int i = 0; i < kRows && U.listOff + i < n; i++) {
    const ScanNet& sn = netScanGet(U.listOff + i);
    int y = 56 + i * kRH;
    bool p = pressed(0, y, 320, kRH - 1);
    c.rect(0, y, 320, kRH - 1, p ? C_BTN : C_WHITE);
    c.hline(0, y + kRH - 1, 320, C_LINE2);
    char t[48]; fit(c, sn.ssid, 220, &fSm, t, sizeof(t));
    c.text(t, 12, y + kRH / 2, &fSm, C_INK, DT_L);
    int bars = sn.rssi > -55 ? 4 : sn.rssi > -67 ? 3 : sn.rssi > -77 ? 2 : 1;
    iconSig(c, 290, y + kRH / 2 - 6, bars, C_ACC);
    if (sn.lock) iconLock(c, 276, y + kRH / 2 - 6, C_ACC);
    hit(c, 0, y, 320, kRH - 1, A_PICKNET, U.listOff + i);
  }
  if (n > kRows) {   // Blättern
    int y = 56 + kRows * kRH;
    c.rect(0, y, 320, 240 - y, C_BG2);
    char t[48]; snprintf(t, sizeof(t), "%d-%d von %d", U.listOff + 1, (U.listOff + kRows < n ? U.listOff + kRows : n), n);
    c.text(t, 160, y + 12, &fTiny, C_MUTED, DT_C);
    if (U.listOff > 0) { button(c, 6, y + 2, 76, 20, "zurück", B_LIGHT, A_LISTUP, 0, nullptr, &fTiny); }
    if (U.listOff + kRows < n) { button(c, 238, y + 2, 76, 20, "weitere", B_LIGHT, A_LISTDN, 0, nullptr, &fTiny); }
  }
}

// Tastatur
static const char* const KB_L[3][10] = {
  {"q","w","e","r","t","z","u","i","o","p"},
  {"a","s","d","f","g","h","j","k","l","ö"},
  {"","y","x","c","v","b","n","m","ä","ü"}};
static const char* const KB_LU[3][10] = {
  {"Q","W","E","R","T","Z","U","I","O","P"},
  {"A","S","D","F","G","H","J","K","L","Ö"},
  {"","Y","X","C","V","B","N","M","Ä","Ü"}};
static const char* const KB_S1[3][10] = {
  {"1","2","3","4","5","6","7","8","9","0"},
  {"-","_",".",",",":",";","/","+","=","?"},
  {"","!","@","#","$","%","&","*","(",")"}};
static const char* const KB_S2[3][10] = {
  {"1","2","3","4","5","6","7","8","9","0"},
  {"'","\"","<",">","[","]","{","}","|","\\"},
  {"","~","`","^","°","§","´","µ","²","³"}};

static const char* kbKey(int r, int col) {
  if (U.kbSym) return U.kbShift ? KB_S2[r][col] : KB_S1[r][col];
  return U.kbShift ? KB_LU[r][col] : KB_L[r][col];
}

static void kbKeyBox(Cv& c, int x, int y, int w, int h, uint16_t bg, uint8_t act, int16_t arg) {
  bool p = pressed(x, y, w, h);
  c.rrect(x, y + 1, w, h, 6, C_KSH);
  c.rrect(x, y, w, h - 1, 6, p ? C_BTND : bg);
  hit(c, x, y, w, h, act, arg);
}

static void scKb(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  bool isW = U.kbTarget == 0;
  char title[64];
  if (isW) snprintf(title, sizeof(title), "Passwort: %s", U.selSsid); else strcpy(title, "Name des Kindes");
  header(c, title, 0, U.kbBack);
  // Eingabefeld
  int fx = 6, fy = 35, fw = 308, fh = 28;
  c.rrect(fx, fy, fw, fh, 7, C_WHITE);
  int textW = fw - 16 - (isW ? 82 : 0);
  char shown[140];
  if (isW && !U.kbShow) {
    int n = utf8Len(U.kbVal);
    int dots = n;
    int maxDots = textW / 9;
    if (dots > maxDots) dots = maxDots;
    for (int i = 0; i < dots; i++) c.circle(fx + 12 + i * 9, fy + fh / 2, 3, C_INK);
    if (U.kbVal[0] == 0) c.text("Passwort", fx + 8, fy + fh / 2, &fBody, C_TOG, DT_L);
    else c.rect(fx + 8 + dots * 9 + 2, fy + 7, 2, fh - 14, C_ACC);
  } else {
    strncpy(shown, U.kbVal, sizeof(shown) - 1); shown[sizeof(shown) - 1] = 0;
    const char* s = shown;
    while (*s && c.tw(s, &fTitle) > textW) { s++; while (((uint8_t)*s & 0xC0) == 0x80) s++; }
    int w = c.text(s, fx + 8, fy + fh / 2, &fTitle, C_INK, DT_L);
    if (U.kbVal[0] == 0 && !isW) c.text("z. B. Mia", fx + 8, fy + fh / 2, &fBody, C_TOG, DT_L);
    c.rect(fx + 8 + w + 1, fy + 7, 2, fh - 14, C_ACC);
  }
  if (isW) button(c, fx + fw - 80, fy + 3, 76, fh - 6, U.kbShow ? "verbergen" : "zeigen", B_NORM, A_EYE, 0, nullptr, &fTiny);
  // Tasten
  const int kKW = 28, kGAP = 3, kX0 = 7, kY0 = 68, kKH = 40;
  for (int r = 0; r < 3; r++) {
    for (int col = 0; col < 10; col++) {
      int x = kX0 + col * (kKW + kGAP), y = kY0 + r * (kKH + 3);
      if (r == 2 && col == 0) {
        kbKeyBox(c, x, y, kKW, kKH, U.kbShift ? C_ACC : C_KFN, A_SHIFT, 0);
        iconShift(c, x + kKW / 2, y + kKH / 2 - 1, U.kbShift ? C_WHITE : C_INK);
        continue;
      }
      kbKeyBox(c, x, y, kKW, kKH, C_WHITE, A_KEY, r * 10 + col);
      c.text(kbKey(r, col), x + kKW / 2, y + kKH / 2 - 1, &fTitle, C_INK, DT_C);
    }
  }
  int y = kY0 + 3 * (kKH + 3);
  int x = kX0;
  kbKeyBox(c, x, y, 2 * kKW + kGAP, kKH, C_KFN, A_SYM, 0);
  c.text(U.kbSym ? "abc" : "123", x + kKW + 1, y + kKH / 2 - 1, &fSm, C_INK, DT_C);
  x += 2 * (kKW + kGAP);
  kbKeyBox(c, x, y, kKW, kKH, C_WHITE, A_KEY, 99);
  c.text("ß", x + kKW / 2, y + kKH / 2 - 1, &fTitle, C_INK, DT_C);
  x += kKW + kGAP;
  kbKeyBox(c, x, y, 4 * kKW + 3 * kGAP, kKH, C_WHITE, A_SPACE, 0);
  c.text("Leertaste", x + (4 * kKW + 3 * kGAP) / 2, y + kKH / 2 - 1, &fTiny, C_MUTED, DT_C);
  x += 4 * (kKW + kGAP);
  kbKeyBox(c, x, y, kKW, kKH, C_KFN, A_BKSP, 0);
  iconBksp(c, x + kKW / 2, y + kKH / 2 - 1, C_INK, C_KFN);
  x += kKW + kGAP;
  kbKeyBox(c, x, y, 2 * kKW + kGAP, kKH, C_ACC, A_KBOK, 0);
  c.text("OK", x + kKW + 1, y + kKH / 2 - 1, &fSm, C_WHITE, DT_C);
}

static void scConnecting(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  spinner(c, 160, 86, 20, C_ACC, C_LINE);
  char t[80], f2[80];
  snprintf(t, sizeof(t), "Verbinde mit %s ...", netTrySsid());
  fit(c, t, 300, &fH2, f2, sizeof(f2));
  c.text(f2, 160, 136, &fH2, C_INK, DT_C);
  c.text("Das dauert ein paar Sekunden.", 160, 162, &fBody, C_MUTED, DT_C);
}

static void scConnErr(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  badge(c, 160, 34, "Nicht verbunden", 1);
  char t[120];
  NetState ns = netState();
  if (ns == NET_ERR_PASS) snprintf(t, sizeof(t), "Das Passwort für »%s« stimmt nicht.", netTrySsid());
  else if (ns == NET_ERR_NOTFOUND) snprintf(t, sizeof(t), "»%s« wurde nicht gefunden.", netTrySsid());
  else snprintf(t, sizeof(t), "Die Verbindung mit »%s« hat nicht geklappt.", netTrySsid());
  int h = wrap(c, t, 20, 74, 280, &fH2, C_INK, 22, 1);
  const char* hint = ns == NET_ERR_PASS ? "Achte auf Groß- und Kleinschreibung."
                   : ns == NET_ERR_NOTFOUND ? "Ist der Router an und nah genug?" : "Bitte nochmal versuchen.";
  int y = 74 + h + 2;
  c.text(hint, 160, y, &fBody, C_TEXT, DT_C);
  bool phone = netConnectViaPortal();
  if (phone) {
    c.text("Gib es am Handy nochmal ein.", 160, y + 18, &fBody, C_TEXT, DT_C);
    button(c, 60, 186, 200, 34, "Lieber hier eingeben", B_NORM, A_SCREENWIFI);
  } else {
    button(c, 20, 186, 136, 34, "Nochmal eingeben", B_PRI, A_RETRYKB);
    button(c, 164, 186, 136, 34, "Anderes WLAN", B_NORM, A_SCREENWIFI);
  }
}

static void scWifiOk(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  badge(c, 160, 40, "Verbunden", 0);
  char t[80];
  snprintf(t, sizeof(t), "Mit %s verbunden", netSsid());
  int h = wrap(c, t, 20, 86, 280, &fH2, C_INK, 22, 1);
  c.text(timeValid() ? "Die Uhrzeit wurde automatisch gestellt." : "Die Uhrzeit wird gleich automatisch gestellt.",
         160, 86 + h + 4, &fBody, C_TEXT, DT_C);
  button(c, 100, 180, 120, 36, "Weiter", B_PRI, A_GO, SC_PAIR);
}

static void scPair(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  bool setup = !S.setupDone;
  header(c, "Wolkenknopf koppeln", setup ? 2 : 0, setup ? -1 : SC_KNOPF);
  mascot(c, 124, 36, 0.6f);
  c.text("Drück jetzt einmal auf", 160, 102, &fH2, C_INK, DT_C);
  c.text("den Wolkenknopf.", 160, 124, &fH2, C_INK, DT_C);
  c.text("Ein Druck reicht. Er sollte in der Nähe stehen.", 160, 152, &fBody, C_TEXT, DT_C);
  float a = (msNow() / 4) % 360;
  c.arc(30, 210, 6, 9, a, a + 90, C_ACC);
  c.text("Station wartet ...", 46, 210, &fTinyR, C_MUTED, DT_L);
  if (setup) button(c, 196, 194, 114, 32, "Später koppeln", B_NORM, A_PAIRLATER);
}

static void scPairOk(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  badge(c, 160, 40, "Gekoppelt", 0);
  c.text("Wolkenknopf verbunden", 160, 92, &fH2, C_INK, DT_C);
  char eta[40], t[80];
  if (K.pct >= 0) { etaText(K.pct, eta, sizeof(eta)); snprintf(t, sizeof(t), "Batterie: %d %% · %s", K.pct, eta); }
  else snprintf(t, sizeof(t), "Batteriestand kommt mit der nächsten Meldung.");
  c.text(t, 160, 120, &fBody, C_TEXT, DT_C);
  button(c, 100, 180, 120, 36, S.setupDone ? "Fertig" : "Weiter", B_PRI, A_GO, S.setupDone ? SC_KNOPF : SC_NTFY);
}

// Handy-Nachrichten: QR-Code, Kanal, Schalter und Test auf EINER Seite.
// Wird für Schritt 3 der Einrichtung und für die Einstellungen benutzt (vorher zwei getrennte, volle Seiten).
static void scNtfyPage(Cv& c, bool setup) {
  c.rect(0, 0, 320, 240, setup ? C_BG : C_BG2);
  header(c, setup ? "Nachrichten aufs Handy" : "Handy-Nachrichten", setup ? 3 : 0, setup ? -1 : U.ntfyBack);
  panel(c, 8, 38, 120, 120);
  char url[64]; snprintf(url, sizeof(url), "https://ntfy.sh/%s", S.topic);
  if (c.vis(42, 112)) c.g->qrcode(url, 12, 42 - c.oy, 112, 2);
  int x = 138;
  if (setup) {
    c.text("So geht's:", x, 50, &fSm, C_INK, DT_L);
  } else {
    c.text(S.ntfyOn ? "Senden: an" : "Senden: AUS", x, 50, &fSm, S.ntfyOn ? C_INK : C_BADFG, DT_L);
    toggle(c, 272, 39, S.ntfyOn, A_NTFYTOG);
  }
  c.text("1. App »ntfy« laden", x, 76, &fBody, C_TEXT, DT_L);
  c.text("2. Code links scannen", x, 96, &fBody, C_TEXT, DT_L);
  c.text("oder in der App diesen Kanal:", x, 118, &fTinyR, C_MUTED, DT_L);
  c.rrect(x - 2, 128, 176, 24, 7, C_WHITE);
  c.text(S.topic, x + 86, 140, &fSm, C_ACC, DT_C);
  char t[96];
  if (setup) snprintf(t, sizeof(t), "Dann kommt eine Nachricht, wenn %s wach ist oder Hilfe ruft.", childName());
  else strcpy(t, "Mehrere Handys? Alle scannen einfach denselben Code.");
  wrap(c, t, 10, 172, 300, &fTinyR, C_MUTED, 13);
  if (U.testPending) {
    spinner(c, 24, 216, 9, C_ACC, C_LINE);
    c.text("sende Test ...", 40, 216, &fTiny, C_MUTED, DT_L);
  } else {
    button(c, 8, 200, setup ? 148 : 304, 32, "Test senden", setup ? B_NORM : B_PRI, A_NTFYTEST);
  }
  if (setup) button(c, 164, 200, 148, 32, S.ntfyTested ? "Weiter" : "Überspringen", S.ntfyTested ? B_PRI : B_NORM, A_GO, SC_DONE);
}

static void scNtfy(Cv& c) { scNtfyPage(c, !S.setupDone); }

static void doneRow(Cv& c, int y, int icon, const char* label, const char* val) {
  panel(c, 45, y, 230, 26);
  if (icon == 0) iconWifi(c, 53, y + 7, C_ACC, true);
  else if (icon == 1) iconCloud(c, 61, y + 12, C_ACC);
  else iconPhone(c, 61, y + 13, C_ACC, C_WHITE);
  c.text(label, 78, y + 13, &fSm, C_INK, DT_L);
  char v[32]; fit(c, val, 90, &fTinyR, v, sizeof(v));
  c.text(v, 267, y + 13, &fTinyR, C_MUTED, DT_R);
}

static void scDone(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG);
  c.text("Alles bereit!", 160, 26, &fH2, C_INK, DT_C);
  doneRow(c, 46, 0, "WLAN", netSsid()[0] ? netSsid() : "-");
  doneRow(c, 76, 1, "Wolkenknopf", K.paired ? "gekoppelt" : "später");
  doneRow(c, 106, 2, "Handy-Nachrichten", S.ntfyTested ? "an" : "später");
  wrap(c, "Einstellungen: Zahnrad unten rechts antippen.", 30, 146, 260, &fTinyR, C_MUTED, 13, 1);
  button(c, 90, 186, 140, 36, "Los geht's", B_PRI, A_FINISH);
}

// ------------------------------------------------------------------ Startseite
static bool screenOff() {
  if (updState() == UPD_LOADING || updState() == UPD_DONE) return false;
  if (!S.setupDone || U.confirm || (H.active && !H.acked) || U.scr != SC_HOME) return false;
  int m = minuteNow();
  if (!inNight(m)) return false;
  if (ST.today >= 0 && inWakeWindow(m)) return false;   // Kind ist schon wach: Guten-Morgen-Anzeige bleibt an
  return (int32_t)(msNow() - U.awakeUntil) > 0;
}

static uint16_t glassOn(Sky s, int y, uint8_t a255, uint32_t over = 0xffffff) {
  return to565(mix888(skyAt(s, y), over, a255 / 255.0f));
}

static void scHome(Cv& c) {
  int m = minuteNow();
  bool night = inNight(m), morning = inWakeWindow(m) && !night;
  Sky sky = SKY_DAY;
  if (ST.today >= 0) sky = (m >= 0 && m < 720) ? SKY_MORNING : SKY_DAY;
  else if (night) sky = SKY_NIGHT;
  else if (morning) sky = SKY_MORNING;
  c.gradient(sky, 0, 240);
  statusBar(c, C_WHITE);

  int top = 24;
  if (!netOnline()) {
    uint16_t bg = glassOn(sky, 30, 235);
    c.rrect(8, top, 304, 20, 7, bg);
    c.text(netHasCreds() ? "Kein WLAN - Knopf und Ton gehen trotzdem." : "Kein WLAN eingerichtet - nur Knopf und Ton.",
           160, top + 10, &fTiny, C_BADFG, DT_C);
    top += 24;
  }

  // Bericht 6-10 Uhr
  bool rep = S.setupDone && K.paired && ST.reportDay == ST.dayKey && ST.dayKey && m >= 360 && m < 600;
  int bottomLimit = rep ? 166 : 196;
  int mid = (top + bottomLimit) / 2;
  char hm[6], t[96];
  if (ST.today >= 0) {
    bool ok; float avg = weekAverage(ok);
    if (S.name[0]) snprintf(t, sizeof(t), "Guten Morgen, %s!", S.name); else strcpy(t, "Guten Morgen!");
    char f2[96]; fit(c, t, 300, &fH1, f2, sizeof(f2));
    c.text(f2, 160, mid - 26, &fH1, C_WHITE, DT_C);
    fmtHM(ST.today, hm);
    int w1 = c.tw("wach seit ", &fH2), w2 = c.tw(hm, &fH2);
    int x0 = 160 - (w1 + w2) / 2;
    c.text("wach seit ", x0, mid + 4, &fH2, C_WHITE, DT_L);
    c.text(hm, x0 + w1, mid + 4, &fH2, C_SUB, DT_L);
    if (ok) { fmtHM((int)lroundf(avg), hm); snprintf(t, sizeof(t), "Ø diese Woche %s", hm); c.text(t, 160, mid + 28, &fSm, C_WHITE, DT_C); }
  } else if (m < 0) {
    c.text("--:--", 160, mid - 10, &fXL, C_WHITE, DT_C);
    c.text(netOnline() ? "Uhrzeit wird gestellt ..." : "Uhrzeit fehlt - braucht WLAN", 160, mid + 26, &fSm, C_WHITE, DT_C);
  } else if (night) {
    fmtHM(m, hm);
    c.text(hm, 160, mid - 10, &fClock, C_WHITE, DT_C);
    snprintf(t, sizeof(t), "Psst - %s schläft noch", childName());
    c.text(t, 160, mid + 26, &fSm, C_WHITE, DT_C);
  } else if (morning) {
    c.text("Guten Morgen!", 160, mid - 30, &fH1, C_WHITE, DT_C);
    fmtHM(m, hm);
    c.text(hm, 160, mid + 2, &fXL, C_WHITE, DT_C);
    snprintf(t, sizeof(t), "%s schläft noch", childName());
    c.text(t, 160, mid + 32, &fSm, C_WHITE, DT_C);
  } else {
    fmtHM(m, hm);
    c.text(hm, 160, mid - 10, &fClock, C_WHITE, DT_C);
    c.text(K.paired ? "Heute noch nicht gedrückt" : "Wolkenknopf noch nicht gekoppelt", 160, mid + 26, &fSm, C_WHITE, DT_C);
  }
  if (ST.nightN) {
    char list[64] = "Nachts gedrückt: ";
    for (int i = 0; i < ST.nightN; i++) {
      fmtHM(ST.night[i], hm);
      if (i) strcat(list, ", ");
      if (strlen(list) + 7 < sizeof(list)) strcat(list, hm);
    }
    char f2[64]; fit(c, list, 300, &fTiny, f2, sizeof(f2));
    c.text(f2, 160, bottomLimit - 6, &fTiny, C_WHITE, DT_C);
  }
  if (rep) {
    int y = 172;
    char eta[40];
    if (ST.reportSilent) {
      c.rrect(8, y, 304, 26, 9, C_BADBG);
      iconCloud(c, 22, y + 12, C_BADFG);
      c.text("Knopf hat sich nicht gemeldet - Batterien prüfen", 36, y + 13, &fTiny, C_BADFG, DT_L);
    } else {
      int p = K.pct;
      bool warn = p >= 0 && p <= 20;
      uint16_t fg = warn ? C_WARNCARDFG : C_INK;
      c.rrect(8, y, 304, 26, 9, warn ? C_WARNCARD : glassOn(sky, y + 13, 235));
      iconBat(c, 16, y + 8, p, fg);
      snprintf(t, sizeof(t), "%s %d %%", warn ? "Bald tauschen:" : "Knopf-Batterie", p);
      int w = c.text(t, 42, y + 13, &fTiny, fg, DT_L);
      etaText(p, eta, sizeof(eta));
      const char* e = strncmp(eta, "reicht ", 7) == 0 ? eta + 7 : eta;
      char f2[48]; fit(c, e, 304 - 48 - w - 12, &fTinyR, f2, sizeof(f2));
      c.text(f2, 304, y + 13, &fTinyR, fg, DT_R);
    }
  }
  // Fußleiste
  {
    int x = 8, y = 202;
    bool p = pressed(x, y, 34, 34);
    c.rrect(x, y, 34, 34, 10, glassOn(sky, y + 17, p ? 110 : 60));
    iconChart(c, x + 9, y + 9, C_WHITE);
    hit(c, x - 6, y - 6, 46, 40, A_STATS);
  }
  {   // Handy-Nachrichten direkt erreichbar (Ausschalten fragt nach). Roter Punkt = Nachrichten sind aus.
    int x = 50, y = 202;
    bool p = pressed(x - 2, y - 6, 40, 40);
    uint16_t bg = glassOn(sky, y + 17, p ? 110 : 60);
    c.rrect(x, y, 34, 34, 10, bg);
    iconPhone(c, x + 17, y + 17, C_WHITE, bg, 1.2f);
    if (!S.ntfyOn) c.circle(x + 29, y + 5, 5, C_RED);
    hit(c, x - 2, y - 6, 40, 40, A_GO, SC_SNTFY);
  }
  {   // Morgen-Ton stumm bis zum Ende des nächsten Morgenfensters (Hilferuf bleibt immer laut)
    int x = 92, y = 202;
    bool muted = toneMuted();
    bool p = pressed(x - 2, y - 6, 40, 40);
    uint16_t bg = glassOn(sky, y + 17, p ? 110 : 60);
    c.rrect(x, y, 34, 34, 10, bg);
    iconSpeaker(c, x + 15, y + 17, C_WHITE, muted);
    if (muted) {
      c.circle(x + 29, y + 5, 5, C_RED);
      char hm[6], t[32]; fmtHM(toneMuteEndMinute(), hm);
      snprintf(t, sizeof(t), "Ton aus bis %s", hm);
      c.text(t, x + 42, y + 17, &fTiny, C_WHITE, DT_L);
    }
    hit(c, x - 2, y - 6, 40, 40, A_MUTE);
  }
  {
    int x = 278, y = 202;
    uint16_t bg = glassOn(sky, y + 17, pressed(x - 8, y - 8, 48, 46) ? 110 : 60);
    c.rrect(x, y, 34, 34, 10, bg);
    iconGear(c, x + 17, y + 17, C_WHITE, bg);
    hit(c, x - 8, y - 8, 48, 46, A_GO, SC_SETTINGS);     // ab 0.1.7: einmal antippen statt 3 s halten
  }
}

static void scStats(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "Aufwachzeiten", 0, U.statsBack);
  static const char* WDAY[7] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
  int wd = weekdayNow();
  int lo = 9999, hi = -1;
  for (int i = 0; i < 7; i++) if (ST.week[i] >= 0) { if (ST.week[i] < lo) lo = ST.week[i]; if (ST.week[i] > hi) hi = ST.week[i]; }
  if (ST.today >= 0) { if (ST.today < lo) lo = ST.today; if (ST.today > hi) hi = ST.today; }
  if (hi < 0) { lo = S.wakeFrom; hi = S.wakeTo; }
  lo -= 30; hi += 30;
  if (hi - lo < 120) { int m = (hi + lo) / 2; lo = m - 60; hi = m + 60; }
  char hm[6];
  for (int i = 0; i < 8; i++) {
    int y = 38 + i * 21;
    bool today = i == 7;
    int v = today ? ST.today : ST.week[i];
    const char* label = today ? "Heute" : (wd >= 0 ? WDAY[(wd - 7 + i + 14) % 7] : "-");
    c.text(label, 10, y + 8, &fTiny, today ? C_INK : C_MUTED, DT_L);
    c.rrect(50, y + 3, 210, 10, 5, C_TRK);
    if (v >= 0) {
      int w = (int)(210.0f * (v - lo) / (hi - lo));
      if (w < 10) w = 10;
      if (w > 210) w = 210;
      c.rrect(50, y + 3, w, 10, 5, today ? C_GOLD : C_BAR);
    }
    fmtHM(v, hm);
    c.text(v >= 0 ? hm : "-", 310, y + 8, &fTiny, C_INK, DT_R);
  }
  bool ok; float avg = weekAverage(ok);
  int mn = 9999, mx = -1;
  for (int i = 0; i < 7; i++) if (ST.week[i] >= 0) { if (ST.week[i] < mn) mn = ST.week[i]; if (ST.week[i] > mx) mx = ST.week[i]; }
  if (ST.today >= 0) { if (ST.today < mn) mn = ST.today; if (ST.today > mx) mx = ST.today; }
  panel(c, 8, 208, 304, 26);
  char t[64], a[6], b[6];
  fmtHM(ok ? (int)lroundf(avg) : -1, hm);
  snprintf(t, sizeof(t), "Ø %s", ok ? hm : "-");
  c.text(t, 16, 221, &fSm, C_INK, DT_L);
  fmtHM(mx >= 0 ? mn : -1, a); fmtHM(mx, b);
  snprintf(t, sizeof(t), "früheste %s · späteste %s", mx >= 0 ? a : "-", mx >= 0 ? b : "-");
  c.text(t, 304, 221, &fTinyR, C_MUTED, DT_R);
}

// ------------------------------------------------------------------ Einstellungen
static const char* const MELS[3] = {"Glöckchen", "Vogelzwitschern", "Sonnenaufgang"};

static void settingsRow(Cv& c, int y, int icon, const char* label, const char* val, uint8_t target) {
  const int kRH = 34;
  bool p = pressed(0, y, 320, kRH - 1);
  c.rect(0, y, 320, kRH - 1, p ? C_BTN : C_WHITE);
  c.hline(0, y + kRH - 1, 320, C_LINE2);
  c.rrect(10, y + 7, 20, 20, 6, C_CHIP);
  int cx = 20, cy = y + 17;
  switch (icon) {
    case 0: iconClock(c, cx, cy, C_ACC, C_CHIP); break;
    case 1: iconNote(c, cx, cy, C_ACC); break;
    case 2: iconBell(c, cx, cy - 1, C_ACC); break;
    case 3: iconCloud(c, cx, cy - 1, C_ACC); break;
    case 4: iconWifi(c, cx - 8, cy - 7, C_ACC, true); break;
    case 5: iconPhone(c, cx, cy, C_ACC, C_CHIP); break;
    case 6: iconPerson(c, cx, cy, C_ACC); break;
    case 7: iconChart(c, cx - 8, cy - 8, C_ACC); break;
    case 9: iconSun(c, cx, cy, C_ACC); break;
    case 10: iconDownload(c, cx, cy, C_ACC); break;
    default: iconChip(c, cx, cy, C_ACC, C_CHIP); break;
  }
  c.text(label, 38, y + 17, &fSm, C_INK, DT_L);
  char v[40]; fit(c, val, 130, &fTinyR, v, sizeof(v));
  c.text(v, 300, y + 17, &fTinyR, C_MUTED, DT_R);
  iconChev(c, 309, y + 17, C_TOG);
  hit(c, 0, y, 320, kRH - 1, A_GO, target);
}

static void scSettings(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  U.clipTop = 31;
  int y = 31 - U.scroll;
  char a[6], b[6], t[40];
  settingsRow(c, y, 5, "Handy-Nachrichten", S.ntfyOn ? "an" : "AUS", SC_SNTFY); y += 34;
  fmtHM(S.wakeFrom, a); fmtHM(S.wakeTo, b); snprintf(t, sizeof(t), "%s-%s", a, b);
  settingsRow(c, y, 0, "Zeiten", t, SC_ZEITEN); y += 34;
  settingsRow(c, y, 1, "Ton", MELS[S.melody % 3], SC_TON); y += 34;
  snprintf(t, sizeof(t), "Tag %d %% · Nacht %d %%", S.briDay, S.briNight);
  settingsRow(c, y, 9, "Display", t, SC_DISPLAY); y += 34;
  settingsRow(c, y, 2, "Hilferuf", S.helpMode ? "lang halten" : "2x drücken", SC_HILFE); y += 34;
  if (!K.paired) strcpy(t, "nicht gekoppelt");
  else if (knopfSilent()) strcpy(t, "meldet sich nicht");
  else if (K.pct >= 0) snprintf(t, sizeof(t), "%d %%", K.pct);
  else strcpy(t, "gekoppelt");
  settingsRow(c, y, 3, "Wolkenknopf", t, SC_KNOPF); y += 34;
  settingsRow(c, y, 4, "WLAN", netOnline() ? netSsid() : (netHasCreds() ? "getrennt" : "nicht eingerichtet"), SC_WLAN); y += 34;
  settingsRow(c, y, 6, "Name des Kindes", S.name[0] ? S.name : "-", SC_NAME); y += 34;
  settingsRow(c, y, 7, "Statistik", "", SC_SSTATS); y += 34;
  settingsRow(c, y, 10, "Updates", !updRepoSet() ? "nur vom PC" : updState() == UPD_AVAILABLE ? "neue Version!" : "automatisch", SC_UPDATE); y += 34;
  snprintf(t, sizeof(t), "Version %s", WOLKE_FW_VERSION);
  settingsRow(c, y, 8, "System", t, SC_SYSTEM); y += 34;
  setScrollMax(c, y + U.scroll);
  U.clipTop = 0;
  header(c, "Einstellungen", 0, SC_HOME);
  scrollBar(c);
}

static void stepper(Cv& c, int y, const char* label, int value, int which) {
  panel(c, 8, y, 304, 34);
  c.text(label, 20, y + 17, &fSm, C_INK, DT_L);
  char hm[6]; fmtHM(value, hm);
  button(c, 186, y + 4, 34, 26, "-", B_NORM, A_STEP, which * 2, nullptr, &fH2);
  c.text(hm, 247, y + 17, &fTitle, C_INK, DT_C);
  button(c, 272, y + 4, 34, 26, "+", B_NORM, A_STEP, which * 2 + 1, nullptr, &fH2);
}

static void scZeiten(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "Zeiten", 0, SC_SETTINGS);
  c.text("Morgenfenster - erster Druck zählt als Aufwachzeit", 10, 42, &fTiny, C_MUTED, DT_L);
  stepper(c, 52, "von", S.wakeFrom, 0);
  stepper(c, 90, "bis", S.wakeTo, 1);
  c.text("Nachtmodus - Display aus, der Knopf weckt es", 10, 140, &fTiny, C_MUTED, DT_L);
  stepper(c, 150, "von", S.nightFrom, 2);
  stepper(c, 188, "bis", S.nightTo, 3);
}

static void scTon(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "Ton", 0, SC_SETTINGS);
  for (int i = 0; i < 3; i++) {
    int y = 40 + i * 40;
    bool sel = S.melody == i;
    c.rrect(8, y, 304, 34, 9, sel ? C_ACC : C_WHITE);
    c.rrect(10, y + 2, 300, 30, 8, C_WHITE);
    c.circle(26, y + 17, 7, sel ? C_ACC : C_TOG);
    if (!sel) c.circle(26, y + 17, 5, C_WHITE);
    c.text(MELS[i], 42, y + 17, &fSm, C_INK, DT_L);
    hit(c, 8, y, 250, 34, A_MEL, i);
    bool p = pressed(266, y + 4, 40, 26);
    c.rrect(266, y + 4, 40, 26, 8, p ? C_BTND : C_BTN);
    iconPlay(c, 287, y + 17, C_ACC);
    hit(c, 262, y, 50, 34, A_PREV, i);
  }
  int y = 166;
  panel(c, 8, y, 304, 44);
  c.text("Lautstärke", 18, y + 22, &fSm, C_INK, DT_L);
  button(c, 104, y + 9, 30, 26, "-", B_NORM, A_VOL, -10, nullptr, &fH2);
  c.rrect(142, y + 18, 110, 8, 4, C_TRK);
  if (S.volume) c.rrect(142, y + 18, 110 * S.volume / 100, 8, 4, C_ACC);
  button(c, 258, y + 9, 30, 26, "+", B_NORM, A_VOL, 10, nullptr, &fH2);
  char t[8]; snprintf(t, sizeof(t), "%d%%", S.volume);
  c.text(t, 197, y + 6, &fTiny, C_MUTED, DT_C);
  c.text("Gilt für den Morgen-Ton. Der Hilferuf hat seine eigene Lautstärke.", 10, 224, &fTinyR, C_MUTED, DT_L);
}

static void scHilfe(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  U.clipTop = 31;
  int y = 38 - U.scroll;
  char t[80];
  snprintf(t, sizeof(t), "So ruft %s Hilfe:", childName());
  c.text(t, 10, y + 6, &fTiny, C_MUTED, DT_L);
  y += 16;
  for (int i = 0; i < 2; i++) {
    bool sel = S.helpMode == i;
    c.rrect(8, y, 304, 40, 9, sel ? C_ACC : C_WHITE);
    c.rrect(10, y + 2, 300, 36, 8, C_WHITE);
    c.circle(26, y + 20, 7, sel ? C_ACC : C_TOG);
    if (!sel) c.circle(26, y + 20, 5, C_WHITE);
    c.text(i == 0 ? "2x schnell drücken" : "2 Sekunden gedrückt halten", 42, y + 13, &fSm, C_INK, DT_L);
    c.text(i == 0 ? "innerhalb von 1 Sekunde" : "weniger Fehlalarme bei kleinen Kindern", 42, y + 28, &fTinyR, C_MUTED, DT_L);
    hit(c, 8, y, 304, 40, A_HELPMODE, i);
    y += 46;
  }
  panel(c, 8, y, 304, 42);
  c.text("Ruf wiederholen", 18, y + 14, &fSm, C_INK, DT_L);
  c.text("alle 10 s, bis jemand »Ich komme« drückt", 18, y + 30, &fTinyR, C_MUTED, DT_L);
  toggle(c, 264, y + 10, S.helpRepeat, A_HELPREP);
  y += 48;
  y += wrap(c, "Eigener Ton nur für den Hilferuf: startet mit 70 % und wird bei jeder Wiederholung lauter (80, 90, 100 %) - egal wie leise der Morgen-Ton ist.",
            10, y + 6, 300, &fTinyR, C_MUTED, 13);
  y += 4;
  button(c, 8, y, 130, 30, "Ruf anhören", B_NORM, A_HELPPREV);
  y += 36;
  setScrollMax(c, y + U.scroll);
  U.clipTop = 0;
  header(c, "Hilferuf", 0, SC_SETTINGS);
  scrollBar(c);
}

static void scKnopf(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "Wolkenknopf", 0, SC_SETTINGS);
  if (!K.paired) {
    c.text("Noch kein Wolkenknopf gekoppelt.", 160, 100, &fBody, C_TEXT, DT_C);
    button(c, 90, 130, 140, 36, "Jetzt koppeln", B_PRI, A_REPAIR);
    return;
  }
  bool silent = knopfSilent();
  int p = K.pct;
  panel(c, 8, 38, 304, 48);
  iconBat(c, 20, 52, p, (p >= 0 && p <= 20) ? C_WARNCARDFG : C_INK, 2.0f);
  char t[64], eta[40];
  if (silent || p < 0) strcpy(t, "-"); else snprintf(t, sizeof(t), "%d %%", p);
  c.text(t, 70, 54, &fH1, (p >= 0 && p <= 20) ? rgb(0xa86a12) : C_INK, DT_L);
  c.text(silent ? "meldet sich nicht" : etaText(p, eta, sizeof(eta)), 70, 75, &fTinyR, C_MUTED, DT_L);
  if (K.mvRest) {
    snprintf(t, sizeof(t), "%d,%02d V Ruhe", K.mvRest / 1000, (K.mvRest % 1000) / 10);
    c.text(t, 304, 54, &fTiny, C_MUTED, DT_R);
    snprintf(t, sizeof(t), "%d,%02d V Senden", K.mvLoad / 1000, (K.mvLoad % 1000) / 10);
    c.text(t, 304, 72, &fTiny, K.mvLoad && K.mvLoad < 3000 ? C_WARNFG : C_MUTED, DT_R);
  }
  // Verlauf 30 Tage
  panel(c, 8, 92, 304, 74);
  c.text("Batterie, letzte 30 Tage", 18, 102, &fTinyR, C_MUTED, DT_L);
  int x0 = 18, x1 = 300, y0 = 114, y1 = 158;
  c.hline(x0, y1, x1 - x0, C_LINE);
  int lo = 100, hi = 0, cnt = 0;
  for (int i = 0; i < 30; i++) if (K.hist[i] != 255) { cnt++; if (K.hist[i] < lo) lo = K.hist[i]; if (K.hist[i] > hi) hi = K.hist[i]; }
  if (cnt == 0) {
    c.text("noch keine Daten", 160, 136, &fTinyR, C_MUTED, DT_C);
  } else {
    lo -= 3; hi += 3; if (lo < 0) lo = 0; if (hi > 100) hi = 100; if (hi - lo < 6) hi = lo + 6;
    int px = -1, py = -1;
    for (int i = 0; i < 30; i++) {
      if (K.hist[i] == 255) continue;
      int x = x0 + (x1 - x0) * i / 29;
      int y = y1 - 2 - (y1 - y0 - 4) * (K.hist[i] - lo) / (hi - lo);
      if (px >= 0) c.line(px, py, x, y, 1.0f, C_ACC);
      px = x; py = y;
    }
    c.circle(px, py, 3, C_ACC);
  }
  panel(c, 8, 172, 304, 40);
  if (K.lastSeen && timeValid()) {
    uint32_t ago = epochNow() - K.lastSeen;
    if (ago < 120) strcpy(t, "gerade eben");
    else if (ago < 3600) snprintf(t, sizeof(t), "vor %lu Min.", (unsigned long)(ago / 60));
    else if (ago < 86400) snprintf(t, sizeof(t), "vor %lu Std.", (unsigned long)(ago / 3600));
    else snprintf(t, sizeof(t), "vor %lu Tagen", (unsigned long)(ago / 86400));
  } else strcpy(t, K.lastSeen ? "bekannt" : "noch nie");
  char line[96]; snprintf(line, sizeof(line), "Zuletzt gemeldet: %s", t);
  c.text(line, 18, 192, &fTinyR, C_MUTED, DT_L);
  button(c, 206, 178, 100, 28, "Neu koppeln", B_NORM, A_REPAIR);
}

static void scWlan(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "WLAN", 0, SC_SETTINGS);
  bool ok = netOnline();
  panel(c, 8, 40, 304, 46);
  iconWifi(c, 20, 54, C_ACC, ok);
  char t[80];
  if (ok) snprintf(t, sizeof(t), "Verbunden mit %s", netSsid());
  else if (netHasCreds()) snprintf(t, sizeof(t), "Keine Verbindung zu %s", netSsid());
  else strcpy(t, "Kein WLAN eingerichtet");
  char f2[80]; fit(c, t, 256, &fSm, f2, sizeof(f2));
  c.text(f2, 46, 54, &fSm, C_INK, DT_L);
  if (ok) {
    int r = netRssi();
    snprintf(t, sizeof(t), "Signal %s · IP %s", r > -60 ? "gut" : r > -72 ? "mittel" : "schwach", netIp());
  } else strcpy(t, netHasCreds() ? "Die Station versucht es regelmäßig neu." : "Ohne WLAN keine Uhrzeit und keine Handy-Nachrichten.");
  c.text(t, 46, 73, &fTinyR, C_MUTED, DT_L);
  wrap(c, "Der Wolkenknopf und der Ton funktionieren auch ohne WLAN. Ohne WLAN fehlen nur die Handy-Nachrichten.",
       12, 104, 296, &fTinyR, C_MUTED, 13);
  button(c, 60, 150, 200, 36, "WLAN wechseln", B_NORM, A_GO, SC_WCHOOSE);
}

static void scSNtfy(Cv& c) { scNtfyPage(c, false); }

static void scName(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "Name des Kindes", 0, SC_SETTINGS);
  wrap(c, "Erscheint bei »Guten Morgen« und in den Handy-Nachrichten.", 20, 62, 280, &fBody, C_TEXT, 16, 1);
  c.text(S.name[0] ? S.name : "-", 160, 116, &fH1, C_INK, DT_C);
  button(c, 100, 156, 120, 36, "Ändern", B_PRI, A_EDITNAME);
}

static void scSStats(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "Statistik", 0, SC_SETTINGS);
  wrap(c, "Gespeichert sind die Aufwachzeiten der letzten 7 Tage. Sie bleiben auch bei Stromausfall erhalten.",
       20, 56, 280, &fBody, C_TEXT, 16, 1);
  button(c, 70, 120, 180, 34, "Ansehen", B_NORM, A_STATS, 1);
  button(c, 70, 164, 180, 34, "Statistik zurücksetzen", B_WARN, A_ASKRESET);
}

static void pctStepper(Cv& c, int y, const char* label, const char* sub, int value, int which) {
  panel(c, 8, y, 304, 42);
  c.text(label, 20, y + 14, &fSm, C_INK, DT_L);
  c.text(sub, 20, y + 30, &fTinyR, C_MUTED, DT_L);
  char t[8]; snprintf(t, sizeof(t), "%d %%", value);
  button(c, 186, y + 8, 34, 26, "-", B_NORM, A_BRI, which * 2, nullptr, &fH2);
  c.text(t, 246, y + 21, &fTitle, C_INK, DT_C);
  button(c, 272, y + 8, 34, 26, "+", B_NORM, A_BRI, which * 2 + 1, nullptr, &fH2);
}

static void scDisplay(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "Display", 0, SC_SETTINGS);
  char a[6], b[6], t[40];
  pctStepper(c, 40, "Helligkeit Tag", "tagsüber", S.briDay, 0);
  fmtHM(S.nightFrom, a); fmtHM(S.nightTo, b);
  snprintf(t, sizeof(t), "Nachtmodus %s-%s", a, b);
  pctStepper(c, 88, "Helligkeit Nacht", t, S.briNight, 1);
  wrap(c, "Beim Einstellen zeigt das Display kurz die neue Helligkeit. Nachts geht die Startseite trotzdem ganz aus, bis jemand tippt oder der Knopf gedrückt wird.",
       12, 146, 296, &fTinyR, C_MUTED, 13);
  button(c, 8, 196, 148, 32, "Display drehen", B_NORM, A_ROTATE);
  button(c, 164, 196, 148, 32, "Touch einstellen", B_NORM, A_CALIB);
}

static void scOta(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "Update vom PC", 0, U.otaBack);
  if (!netOnline()) {
    wrap(c, "Dafür muss die Station mit dem WLAN verbunden sein.", 20, 100, 280, &fBody, C_TEXT, 16, 1);
    return;
  }
  if (!otaActive()) {
    wrap(c, "Der Update-Modus ist aus (nach 15 Minuten oder nach zu vielen falschen Codes).", 20, 86, 280, &fBody, C_TEXT, 16, 1);
    button(c, 70, 150, 180, 34, "Wieder einschalten", B_PRI, A_OTARETRY);
    return;
  }
  c.text("1. Am PC (im selben WLAN) öffnen:", 12, 46, &fSm, C_INK, DT_L);
  panel(c, 8, 58, 304, 34);
  char t[40]; snprintf(t, sizeof(t), "http://%s", netIp());
  c.text(t, 160, 75, &fH2, C_ACC, DT_C);
  c.text("2. Diesen Code eingeben:", 12, 108, &fSm, C_INK, DT_L);
  panel(c, 100, 120, 120, 40);
  c.text(otaCode(), 160, 140, &fXL, C_INK, DT_C);
  wrap(c, "3. Datei Wolkenstation.ino.bin wählen und hochladen. Die Station zeigt den Fortschritt und startet danach neu.",
       12, 176, 296, &fTinyR, C_MUTED, 13);
  c.text("Nur aktiv, solange diese Seite offen ist.", 160, 226, &fTiny, C_MUTED, DT_C);
}

static void scUpdate(Cv& c) {
  UpdState us = updState();
  bool busy = us == UPD_LOADING || us == UPD_DONE;
  c.rect(0, 0, 320, 240, C_BG2);
  header(c, "Updates", 0, busy ? -1 : U.updBack);
  char t[96];
  if (busy) {                                     // läuft: nur Fortschritt, keine Knöpfe
    snprintf(t, sizeof(t), "Version %s wird installiert", updVersion());
    c.text(t, 160, 78, &fH2, C_INK, DT_C);
    int p = us == UPD_DONE ? 100 : updProgress();
    int w = 260 * p / 100;
    c.rrect(30, 104, 260, 16, 8, C_TRK);
    if (w >= 16) c.rrect(30, 104, w, 16, 8, C_ACC);
    if (us == UPD_DONE) strcpy(t, (H.active && !H.acked) ? "Fertig - Neustart nach dem Hilferuf" : "Fertig - startet gleich neu");
    else if (p == 0) strcpy(t, "Verbinde mit GitHub ...");
    else snprintf(t, sizeof(t), "%d %%", p);
    c.text(t, 160, 140, &fSm, C_TEXT, DT_C);
    c.text("Bitte nicht ausstecken.", 160, 172, &fBody, C_MUTED, DT_C);
    c.text("Einstellungen und Kopplung bleiben erhalten.", 160, 192, &fTinyR, C_MUTED, DT_C);
    return;
  }
  snprintf(t, sizeof(t), "Installiert: Version %s", WOLKE_FW_VERSION);
  c.text(t, 12, 46, &fSm, C_INK, DT_L);
  if (!updRepoSet()) {
    wrap(c, "Automatische Updates sind in dieser Firmware nicht eingerichtet. Ein Update geht per Datei vom PC.",
         12, 76, 296, &fBody, C_TEXT, 16);
    button(c, 8, 196, 304, 32, "Datei vom PC hochladen", B_NORM, A_GO, SC_OTA);
    return;
  }
  if (us == UPD_CHECKING) {
    spinner(c, 160, 104, 14, C_ACC, C_LINE);
    c.text("Sehe bei GitHub nach ...", 160, 136, &fSm, C_TEXT, DT_C);
    return;
  }
  if (us == UPD_AVAILABLE || (us == UPD_ERROR && updCanInstall())) {
    if (us == UPD_ERROR) {
      c.rrect(8, 58, 304, 26, 9, C_BADBG);
      char f2[96]; fit(c, updError(), 290, &fTiny, f2, sizeof(f2));
      c.text(f2, 160, 71, &fTiny, C_BADFG, DT_C);
    } else {
      c.rrect(8, 58, 304, 26, 9, C_OKBG);
      snprintf(t, sizeof(t), "Neue Version %s ist da", updVersion());
      c.text(t, 160, 71, &fSm, C_OKFG, DT_C);
    }
    if (updNotes()[0]) wrap(c, updNotes(), 12, 98, 296, &fTinyR, C_TEXT, 13);
    c.text("Dauert etwa eine Minute. Einstellungen bleiben erhalten.", 160, 182, &fTinyR, C_MUTED, DT_C);
    button(c, 8, 196, 304, 34, us == UPD_ERROR ? "Nochmal versuchen" : "Jetzt installieren", B_PRI, A_UPDINSTALL);
    return;
  }
  if (us == UPD_ERROR) {
    c.rrect(8, 58, 304, 26, 9, C_BADBG);
    char f2[96]; fit(c, updError(), 290, &fTiny, f2, sizeof(f2));
    c.text(f2, 160, 71, &fTiny, C_BADFG, DT_C);
  } else {
    c.rrect(8, 58, 304, 26, 9, C_WHITE);
    c.text(us == UPD_NONE ? "Das ist die neueste Version." : "Noch nicht nachgesehen.", 160, 71, &fSm, C_INK, DT_C);
  }
  uint32_t lc = updLastCheck();
  if (lc > 1 && timeValid()) {
    uint32_t ago = epochNow() - lc;
    if (ago < 120) strcpy(t, "Zuletzt nachgesehen: gerade eben");
    else if (ago < 3600) snprintf(t, sizeof(t), "Zuletzt nachgesehen: vor %lu Min.", (unsigned long)(ago / 60));
    else snprintf(t, sizeof(t), "Zuletzt nachgesehen: vor %lu Std.", (unsigned long)(ago / 3600));
    c.text(t, 12, 100, &fTinyR, C_MUTED, DT_L);
  }
  wrap(c, "Die Station sieht einmal am Tag nach. Gibt es etwas Neues, erscheint oben auf der Startseite ein gelbes Schild und eine Nachricht aufs Handy. Installiert wird erst, wenn du es bestätigst.",
       12, 118, 296, &fTinyR, C_MUTED, 13);
  button(c, 8, 164, 304, 32, us == UPD_ERROR ? "Nochmal versuchen" : "Jetzt nachsehen", B_NORM, A_UPDCHECK);
  button(c, 8, 204, 304, 28, "Datei vom PC hochladen", B_LIGHT, A_GO, SC_OTA, nullptr, &fTiny);
}

static void scSystem(Cv& c) {
  c.rect(0, 0, 320, 240, C_BG2);
  U.clipTop = 31;
  int y = 38 - U.scroll;
  panel(c, 8, y, 304, 30);
  c.text("Firmware", 18, y + 15, &fSm, C_INK, DT_L);
  char t[48]; snprintf(t, sizeof(t), "Version %s", WOLKE_FW_VERSION);
  c.text(t, 302, y + 15, &fTinyR, C_MUTED, DT_R);
  y += 36;
  button(c, 8, y, 148, 32, "Display drehen", B_NORM, A_ROTATE);
  button(c, 164, y, 148, 32, "Touch einstellen", B_NORM, A_CALIB);
  y += 38;
  button(c, 8, y, 148, 32, "Test: Aufwachen", B_NORM, A_TESTWAKE);
  button(c, 164, y, 148, 32, "Test: Hilferuf", B_NORM, A_TESTHELP);
  y += 38;
  button(c, 8, y, 304, 32, "Updates", B_NORM, A_GO, SC_UPDATE);
  y += 38;
  y += wrap(c, "Die Tests schicken auch eine Handy-Nachricht (mit »Test« im Titel).",
            10, y + 6, 300, &fTinyR, C_MUTED, 13);
  y += 6;
  button(c, 8, y, 304, 30, "Auf Werkseinstellungen zurücksetzen", B_WARN, A_ASKFACTORY);
  y += 36;
  setScrollMax(c, y + U.scroll);
  U.clipTop = 0;
  header(c, "System", 0, SC_SETTINGS);
  scrollBar(c);
}

// ------------------------------------------------------------------ Hilferuf, Dialog, Toast
static int helpVolPct(int level) { int v = 70 + 10 * level; return v > 100 ? 100 : v; }

static void scHelp(Cv& c) {
  c.gradient(SKY_HELP, 0, 240);
  if ((msNow() / 600) % 2 == 0) {
    for (int i = 0; i < 4; i++) {
      c.hline(0, i, 320, C_WHITE); c.hline(0, 239 - i, 320, C_WHITE);
    }
    c.rect(0, 0, 4, 240, C_WHITE); c.rect(316, 0, 4, 240, C_WHITE);
  }
  char hm[6], t[64];
  fmtHM(H.minute, hm);
  snprintf(t, sizeof(t), H.test ? "TEST · HILFERUF · %s" : "HILFERUF · %s", hm);
  int w = c.tw(t, &fTiny) + 20;
  c.rrect(160 - w / 2, 18, w, 20, 10, glassOn(SKY_HELP, 28, 60));
  c.text(t, 160, 28, &fTiny, C_WHITE, DT_C);
  snprintf(t, sizeof(t), "%s ruft!", childName());
  char f2[64]; fit(c, t, 300, &fXL, f2, sizeof(f2));
  c.text(f2, 160, 70, &fXL, C_WHITE, DT_C);
  c.text("»Kann bitte jemand", 160, 106, &fTitle, C_WHITE, DT_C);
  c.text("nach mir schauen?«", 160, 126, &fTitle, C_WHITE, DT_C);
  button(c, 80, 148, 160, 46, "Ich komme", B_ACK, A_HELPACK, 0, nullptr, &fH2);
  if (S.helpRepeat) {
    if (H.n < 11) snprintf(t, sizeof(t), "Ruf %d · %d %% - %s", H.n + 1, helpVolPct(H.n), H.n < 3 ? "wird lauter, bis jemand kommt" : "wiederholt sich alle 10 s");
    else snprintf(t, sizeof(t), "Ruf %d · Ton aus - bitte nachsehen", H.n + 1);
  } else strcpy(t, "Ruf einmal abgespielt (70 %).");
  c.text(t, 160, 214, &fTiny, C_WHITE, DT_C);
}

static void drawConfirm(Cv& c) {
  if (c.collect) nHits = 0;   // darunterliegende Ansicht sperren
  c.alpha(0, 0, 320, 240, 150, C_INK2);
  c.rrect(24, 52, 272, 136, 14, C_WHITE);
  const char* q = U.confirm == 1 ? "Statistik zurücksetzen?"
                : U.confirm == 3 ? "Handy-Nachrichten ausschalten?" : "Auf Werkseinstellungen zurücksetzen?";
  const char* sub = U.confirm == 1 ? "Alle gespeicherten Aufwachzeiten werden gelöscht."
                  : U.confirm == 3 ? "Dann kommt auch ein Hilferuf nicht mehr aufs Handy."
                                   : "WLAN, Knopf-Kopplung, Einstellungen und Statistik werden gelöscht.";
  int h = wrap(c, q, 36, 74, 248, &fTitle, C_INK, 19, 1);
  wrap(c, sub, 36, 78 + h, 248, &fTinyR, C_TEXT, 13, 1);
  button(c, 36, 144, 116, 32, "Abbrechen", B_NORM, A_CNO);
  button(c, 168, 144, 116, 32, U.confirm == 1 ? "Löschen" : U.confirm == 3 ? "Ausschalten" : "Zurücksetzen", B_WARN, A_CYES);
}

static void drawToast(Cv& c) {
  if (!U.toast[0]) return;
  if (U.toastBig) {
    int h = wrap(c, U.toast, 50, 0, 220, &fH2, C_WHITE, 22, 1, false);
    int bh = h + 24, y = 120 - bh / 2;
    c.rrect(35, y, 250, bh, 14, C_TOAST);
    wrap(c, U.toast, 50, y + 12 + 11, 220, &fH2, C_WHITE, 22, 1);
  } else {
    int w = c.tw(U.toast, &fSm) + 28;
    if (w > 310) w = 310;
    c.rrect(160 - w / 2, 26, w, 26, 13, C_TOAST);
    c.text(U.toast, 160, 39, &fSm, C_WHITE, DT_C);
  }
}

// ------------------------------------------------------------------ Navigation & Aktionen
static bool portalScreen(uint8_t s) {
  return s == SC_HSQR || s == SC_HSWAIT || s == SC_CONNECTING || s == SC_CONNERR || s == SC_WLIST || s == SC_KB;
}

static void go(uint8_t s) {
  uint8_t old = U.scr;
  if (s == SC_SNTFY && old != SC_SNTFY && old != SC_NTFY) U.ntfyBack = old == SC_HOME ? SC_HOME : SC_SETTINGS;
  if (old == SC_PAIR && s != SC_PAIR) linkPairStop();
  if (s == SC_UPDATE && old != SC_UPDATE && old != SC_OTA) U.updBack = old == SC_HOME ? SC_HOME : SC_SETTINGS;
  if (s == SC_OTA && old != SC_OTA) U.otaBack = old == SC_UPDATE ? SC_UPDATE : SC_SYSTEM;
  if (s == SC_OTA && old != SC_OTA) otaStart();
  if (old == SC_OTA && s != SC_OTA) otaStop();
  if (s == SC_PAIR && old != SC_PAIR) linkPairStart();
  if (s == SC_HSQR && !netPortalActive()) netPortalStart();
  if (netPortalActive() && !portalScreen(s) && !(s == SC_WIFIOK)) netPortalStop();
  if (s == SC_WLIST && !netScanBusy() && (old != SC_KB && old != SC_CONNERR)) { U.listOff = 0; netScanStart(); }
  U.scr = s;
  U.scroll = 0; U.scrollMax = 0;
  U.gen++;
  U.pressValid = false;
  U.holding = false;
  U.dirty = true;
}

static void kbOpen(uint8_t target, uint8_t back, const char* init) {
  U.kbTarget = target; U.kbBack = back;
  strncpy(U.kbVal, init, sizeof(U.kbVal) - 1); U.kbVal[sizeof(U.kbVal) - 1] = 0;
  U.kbShift = target == 1 && init[0] == 0;
  U.kbSym = false; U.kbShow = target == 1;
  go(SC_KB);
}

static void kbAppend(const char* s) {
  bool isW = U.kbTarget == 0;
  if (isW) { if (strlen(U.kbVal) + strlen(s) > 63) return; }
  else if (utf8Len(U.kbVal) >= 16) return;
  strcat(U.kbVal, s);
  if (!U.kbSym) U.kbShift = false;
  U.dirty = true;
}

static void startScreenWifi() {
  go(SC_WLIST);
}

static void doAct(uint8_t act, int16_t arg) {
  switch (act) {
    case A_GO: go((uint8_t)arg); break;
    case A_WAKE: uiWake(); break;
    case A_PORTAL: go(SC_HSQR); break;
    case A_SCREENWIFI: startScreenWifi(); break;
    case A_RESCAN: U.listOff = 0; netScanStart(); U.dirty = true; break;
    case A_LISTUP: U.listOff -= 5; if (U.listOff < 0) U.listOff = 0; U.dirty = true; break;
    case A_LISTDN: U.listOff += 5; U.dirty = true; break;
    case A_PICKNET: {
      if (arg < 0 || arg >= netScanCount()) break;
      const ScanNet& sn = netScanGet(arg);
      strncpy(U.selSsid, sn.ssid, sizeof(U.selSsid) - 1);
      U.selLock = sn.lock;
      if (!sn.lock) { netConnect(U.selSsid, ""); go(SC_CONNECTING); }
      else kbOpen(0, SC_WLIST, "");
      break;
    }
    case A_KEY:
      if (arg == 99) kbAppend("ß");
      else if (arg >= 0 && arg < 30) kbAppend(kbKey(arg / 10, arg % 10));
      break;
    case A_SHIFT: U.kbShift = !U.kbShift; U.dirty = true; break;
    case A_SYM: U.kbSym = !U.kbSym; U.kbShift = false; U.dirty = true; break;
    case A_SPACE: kbAppend(" "); break;
    case A_BKSP: utf8Trim(U.kbVal); U.dirty = true; break;
    case A_EYE: U.kbShow = !U.kbShow; U.dirty = true; break;
    case A_KBOK:
      if (U.kbTarget == 0) {
        if (U.selLock && strlen(U.kbVal) < 8) { uiToast("WLAN-Passwörter haben mind. 8 Zeichen", false); break; }
        netConnect(U.selSsid, U.kbVal);
        go(SC_CONNECTING);
      } else {
        char* s = U.kbVal;
        while (*s == ' ') s++;
        size_t n = strlen(s);
        while (n && s[n - 1] == ' ') s[--n] = 0;
        strncpy(S.name, s, sizeof(S.name) - 1); S.name[sizeof(S.name) - 1] = 0;
        storeSaveSettings();
        go(SC_NAME);
      }
      break;
    case A_RETRYKB: kbOpen(0, SC_WLIST, ""); break;
    case A_PAIRLATER: go(SC_NTFY); break;
    case A_FINISH:
      S.setupDone = 1; storeSaveSettings();
      go(SC_HOME); uiToast("Fertig eingerichtet", false);
      break;
    case A_STEP: {
      int which = arg / 2, d = (arg % 2) ? 15 : -15;
      int16_t* v = which == 0 ? &S.wakeFrom : which == 1 ? &S.wakeTo : which == 2 ? &S.nightFrom : &S.nightTo;
      *v = (int16_t)(((*v + d) % 1440 + 1440) % 1440);
      if (which == 0 && S.wakeFrom >= S.wakeTo) S.wakeFrom = (int16_t)((S.wakeTo - 15 + 1440) % 1440);
      if (which == 1 && S.wakeTo <= S.wakeFrom) S.wakeTo = (int16_t)((S.wakeFrom + 15) % 1440);
      storeSaveSettings(); U.dirty = true;
      break;
    }
    case A_MEL: S.melody = (uint8_t)arg; storeSaveSettings(); audioMelody(S.melody, S.volume); U.dirty = true; break;
    case A_PREV: audioMelody(arg, S.volume); break;
    case A_VOL: {
      int v = S.volume + arg; if (v < 0) v = 0; if (v > 100) v = 100;
      S.volume = (uint8_t)v; storeSaveSettings(); audioMelody(S.melody, S.volume); U.dirty = true;
      break;
    }
    case A_HELPMODE: S.helpMode = (uint8_t)arg; storeSaveSettings(); U.dirty = true;
      uiToast("Knopf übernimmt es nach dem nächsten Drücken", false);
      break;
    case A_HELPREP: S.helpRepeat = !S.helpRepeat; storeSaveSettings(); U.dirty = true; break;
    case A_HELPPREV: audioHelp(0); break;
    case A_NTFYTOG:
      if (S.ntfyOn) { U.confirm = 3; U.dirty = true; break; }      // Ausschalten nur mit Rückfrage
      S.ntfyOn = 1; storeSaveSettings(); U.dirty = true; uiToast("Handy-Nachrichten an", false);
      break;
    case A_NTFYTEST:
      if (!netOnline()) { uiToast("Kein WLAN - Test geht gerade nicht", false); break; }
      ntfyTestReset();
      ntfySend("Test von der Wolkenstation", "Alles funktioniert. So sieht eine Nachricht aus.", 3, NTFY_TEST, "cloud");
      U.testPending = true; U.dirty = true;
      break;
    case A_REPAIR: go(SC_PAIR); break;
    case A_ASKRESET: U.confirm = 1; U.dirty = true; break;
    case A_ASKFACTORY: U.confirm = 2; U.dirty = true; break;
    case A_CNO: U.confirm = 0; U.dirty = true; break;
    case A_CYES:
      if (U.confirm == 1) {
        for (int i = 0; i < 7; i++) ST.week[i] = -1;
        ST.today = -1; ST.nightN = 0; storeSaveStats();
        U.confirm = 0; uiToast("Statistik gelöscht", false); go(SC_SSTATS);
      } else if (U.confirm == 2) {
        U.confirm = 0; storeFactoryReset();
      } else if (U.confirm == 3) {
        S.ntfyOn = 0; storeSaveSettings();
        U.confirm = 0; U.dirty = true; uiToast("Handy-Nachrichten aus", false);
      }
      break;
    case A_HELPACK: helpAck(false); break;
    case A_EDITNAME: kbOpen(1, SC_NAME, S.name); break;
    case A_ROTATE: S.rotation = S.rotation == 1 ? 3 : 1; storeSaveSettings(); sysSetRotation(S.rotation); U.dirty = true; break;
    case A_CALIB: sysCalibrateTouch(); U.dirty = true; break;
    case A_TESTWAKE: simulatePress(); break;
    case A_TESTHELP: onKnopfHelp(true); break;
    case A_STATS: U.statsBack = arg ? SC_SSTATS : SC_HOME; go(SC_STATS); break;
    case A_BRI: {
      bool night = arg >= 2;
      uint8_t* v = night ? &S.briNight : &S.briDay;
      int nv = *v + ((arg % 2) ? 10 : -10);
      int lo = night ? 10 : 20;
      if (nv < lo) nv = lo;
      if (nv > 100) nv = 100;
      *v = (uint8_t)nv;
      storeSaveSettings();
      U.briPreview = (uint8_t)(nv * 255 / 100);
      U.briPreviewUntil = msNow() + 2500;
      U.dirty = true;
      break;
    }
    case A_MUTE: {
      if (!toneMuted() && minuteNow() < 0) { uiToast("Geht erst, wenn die Uhrzeit bekannt ist", false); break; }
      char t[48], hm[6];
      if (toneMuteToggle()) { fmtHM(toneMuteEndMinute(), hm); snprintf(t, sizeof(t), "Morgen-Ton aus bis %s", hm); }
      else strcpy(t, "Morgen-Ton wieder an");
      uiToast(t, false);
      U.dirty = true;
      break;
    }
    case A_OTARETRY: otaStart(); U.dirty = true; break;
    case A_UPDCHECK: updCheckNow(); U.dirty = true; break;
    case A_UPDINSTALL:
      if (!updInstall()) uiToast((H.active && !H.acked) ? "Erst nach dem Hilferuf" : "Gerade nicht möglich (WLAN?)", false);
      U.dirty = true;
      break;
    default: break;
  }
}

// ------------------------------------------------------------------ Öffentliche Funktionen
void uiInit() {
  U.scr = S.setupDone ? SC_HOME : SC_WELCOME;
  U.awakeUntil = msNow() + 60000;
  U.lastNet = netState();
  U.dirty = true;
}

void uiRender(lgfx::LovyanGFX& g, int oy, bool collect) {
  Cv c{&g, oy, collect};
  if (collect) nHits = 0;
  U.clipTop = 0;
  if (screenOff()) {
    c.rect(0, 0, 320, 240, C_BLACK);
    c.text("Nachtmodus - tippen zum Einschalten", 160, 230, &fTinyR, C_GREYOFF, DT_C);
    hit(c, 0, 0, 320, 240, A_WAKE);
    return;
  }
  if (H.active && !H.acked) { scHelp(c); drawToast(c); return; }
  if (updState() == UPD_LOADING || updState() == UPD_DONE) { scUpdate(c); drawToast(c); return; }
  switch (U.scr) {
    case SC_WELCOME: scWelcome(c); break;
    case SC_HSQR: scHsQr(c); break;
    case SC_HSWAIT: scHsWait(c); break;
    case SC_WLIST: scWList(c); break;
    case SC_KB: scKb(c); break;
    case SC_CONNECTING: scConnecting(c); break;
    case SC_CONNERR: scConnErr(c); break;
    case SC_WIFIOK: scWifiOk(c); break;
    case SC_PAIR: scPair(c); break;
    case SC_PAIROK: scPairOk(c); break;
    case SC_NTFY: scNtfy(c); break;
    case SC_DONE: scDone(c); break;
    case SC_HOME: scHome(c); break;
    case SC_STATS: scStats(c); break;
    case SC_SETTINGS: scSettings(c); break;
    case SC_ZEITEN: scZeiten(c); break;
    case SC_TON: scTon(c); break;
    case SC_HILFE: scHilfe(c); break;
    case SC_KNOPF: scKnopf(c); break;
    case SC_WLAN: scWlan(c); break;
    case SC_WCHOOSE: scWChoose(c); break;
    case SC_SNTFY: scSNtfy(c); break;
    case SC_NAME: scName(c); break;
    case SC_SSTATS: scSStats(c); break;
    case SC_SYSTEM: scSystem(c); break;
    case SC_DISPLAY: scDisplay(c); break;
    case SC_OTA: scOta(c); break;
    case SC_UPDATE: scUpdate(c); break;
    default: scHome(c); break;
  }
  if (U.confirm) drawConfirm(c);
  drawToast(c);
}

static int findHit(int x, int y) {
  for (int i = nHits - 1; i >= 0; i--) {
    const Hit& h = hits[i];
    if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) return i;
  }
  return -1;
}

void uiTouch(bool down, int x, int y) {
  uint32_t now = msNow();
  if (down && !U.down) {
    U.down = true; U.downX = x; U.downY = y; U.dragging = false; U.dragStartScroll = U.scroll;
    if (screenOff()) { uiWake(); U.swallow = true; return; }
    U.swallow = false;
    uiWake();
    int i = findHit(x, y);
    if (i >= 0) {
      U.press = hits[i]; U.pressValid = true; U.pressGen = U.gen;
    } else U.pressValid = false;
    U.dirty = true;
  } else if (down && U.down) {
    if (U.swallow) return;
    if (U.scrollMax > 0 && !U.holding && (y - U.downY > 10 || U.downY - y > 10)) { U.dragging = true; U.pressValid = false; }
    if (U.dragging) {
      int s = U.dragStartScroll - (y - U.downY);
      if (s < 0) s = 0;
      if (s > U.scrollMax) s = U.scrollMax;
      if (s != U.scroll) { U.scroll = s; U.dirty = true; }
    } else if (U.pressValid) {
      const Hit& h = U.press;
      if (x < h.x - 12 || x > h.x + h.w + 12 || y < h.y - 12 || y > h.y + h.h + 12) {
        U.pressValid = false; U.holding = false; U.dirty = true;
      }
    }
  } else if (!down && U.down) {
    U.down = false;
    if (U.swallow) { U.swallow = false; return; }
    if (U.holding) {
      U.holding = false; U.pressValid = false;
      if (now - U.holdStart < 3000) uiToast("Zum Öffnen 3 Sekunden gedrückt halten", false);
      U.dirty = true;
      return;
    }
    if (U.pressValid && !U.dragging && U.pressGen == U.gen) {
      Hit h = U.press;
      U.pressValid = false;
      doAct(h.act, h.arg);
    }
    U.pressValid = false;
    U.dragging = false;
    U.dirty = true;
  }
}

bool uiTick() {
  uint32_t now = msNow();
  if (U.toast[0] && (int32_t)(now - U.toastUntil) > 0) { U.toast[0] = 0; U.dirty = true; }
  int m = minuteNow();
  if (m != U.lastMinute) { U.lastMinute = m; U.dirty = true; }
  if (U.holding && now - U.holdStart >= 3000) {
    U.holding = false; U.pressValid = false; U.down = true; U.swallow = true;   // Finger liegt noch: nicht doppelt auslösen
    go(SC_SETTINGS);
  }
  // Netz
  NetState ns = netState();
  if (ns != U.lastNet) {
    U.lastNet = ns;
    if (ns == NET_CONNECTING && netConnectViaPortal() && (U.scr == SC_HSQR || U.scr == SC_HSWAIT || U.scr == SC_CONNERR)) go(SC_CONNECTING);
    else if (U.scr == SC_CONNECTING) {
      if (ns == NET_CONNECTED) {
        if (S.setupDone) { go(SC_WLAN); uiToast("WLAN verbunden", false); }
        else go(SC_WIFIOK);
      } else if (ns == NET_ERR_PASS || ns == NET_ERR_NOTFOUND || ns == NET_ERR_OTHER) go(SC_CONNERR);
    }
    U.dirty = true;
  }
  bool pc = netPortalClient();
  if (pc != U.lastPortalClient) {
    U.lastPortalClient = pc;
    if (pc && U.scr == SC_HSQR) go(SC_HSWAIT);
    if (!pc && U.scr == SC_HSWAIT) go(SC_HSQR);
  }
  if (U.testPending) {
    int r = ntfyTestResult();
    if (r != 0) {
      U.testPending = false;
      if (r > 0) { S.ntfyTested = 1; storeSaveSettings(); uiToast("Gesendet - schau aufs Handy", false); }
      else uiToast("Senden hat nicht geklappt", false);
      U.dirty = true;
    }
  }
  bool anim = U.scr == SC_CONNECTING || U.scr == SC_PAIR || U.holding || (H.active && !H.acked) ||
              (U.scr == SC_WLIST && netScanBusy()) || U.testPending || updState() == UPD_CHECKING;
  if (anim && now - U.animMs >= (U.holding ? 70u : 120u)) { U.animMs = now; U.dirty = true; }
  bool off = screenOff();
  if (off != U.wasOff) { U.wasOff = off; U.dirty = true; }
  bool oa = otaActive();
  if (oa != U.lastOta) { U.lastOta = oa; U.dirty = true; }
  uint8_t us = (uint8_t)updState();
  int up = updProgress();
  if (us != U.lastUpd || (us == UPD_LOADING && up != U.lastUpdPct)) { U.lastUpd = us; U.lastUpdPct = up; U.dirty = true; }
  bool d = U.dirty;
  U.dirty = false;
  return d;
}

uint8_t uiBacklight() {
  if (screenOff()) return 0;
  if (H.active && !H.acked) return 255;
  if (U.briPreviewUntil && (int32_t)(msNow() - U.briPreviewUntil) < 0) return U.briPreview;
  int m = minuteNow();
  int pct = (S.setupDone && inNight(m)) ? S.briNight : S.briDay;
  return (uint8_t)(pct * 255 / 100);
}

void uiOnNetEvent() { U.dirty = true; }
void uiOnPaired() { if (U.scr == SC_PAIR) go(SC_PAIROK); U.dirty = true; }
void uiOnPortalClient() { U.dirty = true; }
void uiOnHelp() { U.confirm = 0; U.dirty = true; }

// Nur für den PC-Test: Ansicht direkt setzen
void uiDebugSet(int scr, int confirm, bool holding, int scroll) {
  U.scr = (uint8_t)scr; U.confirm = (uint8_t)confirm; U.holding = holding; U.holdStart = msNow() - 1500;
  U.scroll = scroll; U.toast[0] = 0; U.dirty = true;
}
void uiDebugKb(int target, const char* val, bool shift, bool sym, bool show) {
  U.kbTarget = (uint8_t)target; strncpy(U.kbVal, val, sizeof(U.kbVal) - 1); U.kbShift = shift; U.kbSym = sym; U.kbShow = show;
  strcpy(U.selSsid, "FRITZ!Box 7590 XY");
}
