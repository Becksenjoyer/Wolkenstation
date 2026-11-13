// Funkprotokoll Wolkenknopf <-> Wolkenstation (ESP-NOW).
// Diese Datei ist für BEIDE Geräte gleich. Änderungen immer in beiden Projekten übernehmen.
//
// Ablauf Koppeln:
//   1. Station zeigt "Wolkenknopf koppeln" (nur dann werden Kopplungsanfragen angenommen).
//   2. Knopf sendet MSG_PAIR_REQ als Broadcast (FF:FF:FF:FF:FF:FF) nacheinander auf Kanal 1..13.
//   3. Station antwortet unverschlüsselt mit MSG_PAIR_ACK (neuer Zufallsschlüssel LMK + WLAN-Kanal).
//   4. Knopf speichert Station-MAC, LMK und Kanal, trägt die Station als verschlüsselten Peer ein,
//      wartet 500 ms und sendet dann MSG_HEARTBEAT (schon verschlüsselt). Ab jetzt ist alles verschlüsselt.
//
// Ablauf Drücken:
//   Knopf sendet MSG_PRESS / MSG_HELP / MSG_HEARTBEAT an die Station-MAC auf dem gespeicherten Kanal.
//   Station antwortet sofort mit MSG_ACK (Uhrzeit, Hilferuf-Modus, Kanal, Sekunden bis zum nächsten Herzschlag).
//   Kommt kein ACK: gleiche seq bis zu 10x wiederholen; danach Kanäle 1..13 durchprobieren
//   (die Station wechselt den Kanal, wenn der Router ihn wechselt).
//   Die Station erkennt Wiederholungen an derselben seq und zählt sie nicht doppelt.
//
// Nach MSG_HELP bleibt der Knopf ca. 2 Minuten wach und wartet auf MSG_HELP_ACK
// ("Ich komme" wurde gedrückt) -> LED blinkt 3x.
#pragma once
#include <stdint.h>

#define WOLKE_MAGIC 0x574Bu      // "WK"
#define WOLKE_PROTO 1

enum : uint8_t {
  MSG_PAIR_REQ  = 1,   // Knopf -> Station (Broadcast)      MsgReport
  MSG_PAIR_ACK  = 2,   // Station -> Knopf                  MsgPairAck
  MSG_PRESS     = 3,   // Knopf -> Station                  MsgReport
  MSG_HELP      = 4,   // Knopf -> Station                  MsgReport
  MSG_HEARTBEAT = 5,   // Knopf -> Station (täglich 05:30)  MsgReport
  MSG_ACK       = 6,   // Station -> Knopf                  MsgAck
  MSG_HELP_ACK  = 7,   // Station -> Knopf                  MsgHeader
};

struct __attribute__((packed)) MsgHeader {
  uint16_t magic;      // WOLKE_MAGIC
  uint8_t  proto;      // WOLKE_PROTO
  uint8_t  type;
  uint32_t seq;        // fortlaufend je Absender
};

struct __attribute__((packed)) MsgReport {
  MsgHeader h;
  uint16_t mvRest;     // Batteriespannung des Packs in Ruhe, mV (Diodenabfall schon herausgerechnet)
  uint16_t mvLoad;     // Batteriespannung beim Senden, mV
  uint16_t fw;         // Firmware-Version des Knopfs (z. B. 1 = 0.1)
};

struct __attribute__((packed)) MsgPairAck {
  MsgHeader h;
  uint8_t  lmk[16];    // Schlüssel für die verschlüsselte Verbindung
  uint8_t  channel;    // aktueller WLAN-Kanal der Station
  uint32_t epoch;      // Unix-Zeit (0 = unbekannt)
  uint8_t  helpMode;   // 0 = 2x drücken, 1 = 2 s halten
  uint32_t secToHeartbeat; // Sekunden bis zum nächsten Herzschlag (0 = unbekannt -> 24 h)
};

struct __attribute__((packed)) MsgAck {
  MsgHeader h;
  uint32_t ackSeq;     // seq der bestätigten Nachricht
  uint32_t epoch;
  uint8_t  helpMode;
  uint8_t  channel;
  uint32_t secToHeartbeat;
};

// Gemeinsamer Hauptschlüssel (PMK). Der eigentliche Schutz ist der zufällige LMK pro Kopplung.
static const uint8_t WOLKE_PMK[16] = {'W','o','l','k','e','n','s','t','a','t','i','o','n','P','M','K'};
