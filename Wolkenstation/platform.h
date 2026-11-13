// Nur auf dem ESP32: interne Funktionen zwischen den Hardware-Modulen.
#pragma once
#include <stdint.h>
#include <stddef.h>

// store.cpp
void storeLoad();
bool storeLoadWifi(char* ssid, size_t ssidLen, char* pass, size_t passLen);
void storeSaveWifi(const char* ssid, const char* pass);
bool storeLoadCal(uint16_t cal[8]);
void storeSaveCal(const uint16_t cal[8]);
bool storeLoadStr(const char* key, char* out, size_t n);
void storeSaveStr(const char* key, const char* v);

// update.cpp
void updLoop();

// station_main.cpp
void sysLoopWdt(bool on);                       // Watchdog für die Hauptschleife an/aus
void sysOtaScreen(int pct, const char* text);   // Anzeige während des Updates (pct <= 0: Seite neu)
extern uint32_t sysOtaMarker;                   // übersteht den Neustart: "Update wurde installiert"
static const uint32_t OTA_MARKER = 0x57A7E0A0;

// audio.cpp
void audioInit();

// net.cpp
void netInit();
void netLoop();
uint8_t netChannel();

// link.cpp
void linkInit();
void linkLoop();
