// Wolkenstation – kleiner JSON-Leser für die Release-Antwort von GitHub.
// Liest nur, was die Updates brauchen (ohne Zusatz-Bibliothek). Reines C++, wird am PC mitgetestet.
#pragma once
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

// Zeigt auf den Wert hinter "key": (Leerzeichen übersprungen), sonst nullptr.
static const char* jValue(const char* s, const char* end, const char* key) {
  size_t kl = strlen(key);
  for (const char* p = s; p + kl + 2 < end; p++) {
    if (*p != '"' || strncmp(p + 1, key, kl) != 0 || p[kl + 1] != '"') continue;
    const char* q = p + kl + 2;
    while (q < end && isspace((unsigned char)*q)) q++;
    if (q >= end || *q != ':') continue;
    q++;
    while (q < end && isspace((unsigned char)*q)) q++;
    return q < end ? q : nullptr;
  }
  return nullptr;
}

// Text-Wert lesen (mit \n, \", \uXXXX). allowCut = zu lange Werte abschneiden statt als Fehler werten.
static bool jStr(const char* s, const char* end, const char* key, char* out, size_t n, bool allowCut) {
  const char* v = jValue(s, end, key);
  if (!v || *v != '"' || n == 0) return false;
  size_t o = 0;
  bool cut = false;
  for (const char* p = v + 1; p < end; p++) {
    char ch = *p;
    if (ch == '"') { out[o] = 0; return allowCut || !cut; }
    if (ch == '\\' && p + 1 < end) {
      char e = *++p;
      if (e == 'n') ch = '\n';
      else if (e == 't') ch = ' ';
      else if (e == 'r' || e == 'b' || e == 'f') continue;
      else if (e == 'u') {
        if (p + 4 >= end) break;
        char hx[5] = {p[1], p[2], p[3], p[4], 0};
        unsigned cp = (unsigned)strtoul(hx, nullptr, 16);
        p += 4;
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;        // Emoji usw. kann das Display nicht zeigen
        char u[3];
        size_t k = 0;
        if (cp < 0x80) u[k++] = (char)cp;
        else if (cp < 0x800) { u[k++] = (char)(0xC0 | (cp >> 6)); u[k++] = (char)(0x80 | (cp & 0x3F)); }
        else { u[k++] = (char)(0xE0 | (cp >> 12)); u[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[k++] = (char)(0x80 | (cp & 0x3F)); }
        if (o + k < n) { memcpy(out + o, u, k); o += k; } else cut = true;
        continue;
      } else ch = e;                                       // \" \\ \/
    }
    if (o + 1 < n) out[o++] = ch; else cut = true;
  }
  out[o] = 0;
  return false;                                            // Text endet nicht
}

static long jNum(const char* s, const char* end, const char* key) {
  const char* v = jValue(s, end, key);
  return (v && isdigit((unsigned char)*v)) ? strtol(v, nullptr, 10) : -1;
}

// Erste Datei in "assets", deren Name auf suffix endet: Download-Adresse, Größe, SHA256 (hex, ohne "sha256:").
// Verschachtelte Teile (z. B. "uploader") werden übersprungen, damit deren Felder nicht verwechselt werden.
static bool jAsset(const char* s, const char* end, const char* suffix, char* url, size_t urlN, long* size,
                   char* sha, size_t shaN) {
  const char* p = jValue(s, end, "assets");
  if (!p || *p != '[') return false;
  static char flat[1600];
  size_t fl = 0, sl = strlen(suffix);
  int depth = 0;
  bool inStr = false, esc = false;
  for (p++; p < end; p++) {
    char ch = *p;
    if (inStr) {
      if (depth == 1 && fl + 1 < sizeof(flat)) flat[fl++] = ch;
      if (esc) esc = false;
      else if (ch == '\\') esc = true;
      else if (ch == '"') inStr = false;
      continue;
    }
    if (ch == '"') { inStr = true; if (depth == 1 && fl + 1 < sizeof(flat)) flat[fl++] = ch; continue; }
    if (ch == '{' || ch == '[') { if (depth == 0) fl = 0; depth++; continue; }
    if (ch == '}' || ch == ']') {
      if (depth == 0) return false;                        // Ende der Liste
      if (--depth == 0) {                                  // ein Eintrag ist komplett
        flat[fl] = 0;
        char name[96];
        if (jStr(flat, flat + fl, "name", name, sizeof(name), false)) {
          size_t nl = strlen(name);
          if (nl >= sl && strcmp(name + nl - sl, suffix) == 0 &&
              jStr(flat, flat + fl, "browser_download_url", url, urlN, false)) {
            *size = jNum(flat, flat + fl, "size");
            char dg[80];
            sha[0] = 0;
            if (jStr(flat, flat + fl, "digest", dg, sizeof(dg), false) && strncmp(dg, "sha256:", 7) == 0 &&
                strlen(dg + 7) == 64 && shaN > 64)
              strcpy(sha, dg + 7);
            return true;
          }
        }
      }
      continue;
    }
    if (depth == 1 && fl + 1 < sizeof(flat)) flat[fl++] = ch;
  }
  return false;
}

// Versionsnummern wie "v0.1.10" oder "0.2" vergleichen: true, wenn a neuer ist als b.
static void verParse(const char* s, int v[3]) {
  v[0] = v[1] = v[2] = 0;
  while (*s && !isdigit((unsigned char)*s)) s++;
  for (int i = 0; i < 3 && *s; i++) {
    v[i] = atoi(s);
    while (isdigit((unsigned char)*s)) s++;
    if (*s != '.') break;
    s++;
  }
}
static bool verNewer(const char* a, const char* b) {
  int x[3], y[3];
  verParse(a, x);
  verParse(b, y);
  for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
  return false;
}

// Release-Text fürs Display: Markdown-Zeichen und Leerzeilen raus, höchstens maxLines Zeilen, sauberes UTF-8-Ende.
static void notesClean(const char* in, char* out, size_t n, int maxLines) {
  size_t o = 0;
  int lines = 1;
  bool nl = true;                                          // am Anfang keine Leerzeilen
  for (const char* p = in; *p && o + 1 < n; p++) {
    char ch = *p;
    if (ch == '#' || ch == '*' || ch == '`' || ch == '\r' || ch == '_') continue;
    if (ch == '\n') {
      if (nl) continue;
      if (++lines > maxLines) break;
      nl = true;
    } else {
      if (nl && ch == ' ') continue;
      nl = false;
    }
    out[o++] = ch;
  }
  size_t k = o;                                            // abgeschnittenes UTF-8-Zeichen am Ende entfernen
  while (k > 0 && ((uint8_t)out[k - 1] & 0xC0) == 0x80) k--;
  if (k > 0) {
    uint8_t lead = (uint8_t)out[k - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (o - (k - 1) < need) o = k - 1;
  }
  while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == ' ' || out[o - 1] == '-')) o--;
  out[o] = 0;
}
