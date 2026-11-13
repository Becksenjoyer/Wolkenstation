// Wolkenstation – Funkverbindung zum Wolkenknopf (ESP-NOW, verschlüsselt nach dem Koppeln).
// Die Empfangs-Rückrufe laufen im WLAN-Task: dort nur Antwort senden und Ereignis in die Warteschlange legen.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include "app.h"
#include "platform.h"
#include "protocol.h"

enum EvType : uint8_t { EV_PRESS, EV_HELP, EV_REPORT, EV_PAIR_SEEN };
struct LinkEv { uint8_t type; uint8_t mac[6]; uint16_t mvRest, mvLoad; };

static QueueHandle_t evq;
static volatile bool pairing = false;
static volatile bool knopfKnown = false;
static uint8_t knopfMac[6];

static uint8_t pendMac[6], pendLmk[16];
static volatile bool pendActive = false;
static volatile bool pendSendDone = false, pendSendOk = false;
static uint16_t pendMvRest = 0, pendMvLoad = 0;
static uint32_t pendSeenMs = 0;

static uint32_t lastSeq = 0;
static bool seqValid = false;
static uint32_t mySeq = 1;

static uint32_t helpAckUntil = 0, helpAckNext = 0;
static volatile bool helpAckDelivered = false;
static volatile bool helpAckSending = false;
static bool ready = false;

static uint32_t secToHeartbeat() {
  if (!timeValid()) return 0;
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  int32_t cur = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
  int32_t d = (5 * 3600 + 30 * 60) - cur;      // täglich 05:30 – vor dem 6-Uhr-Bericht
  if (d <= 60) d += 86400;
  return (uint32_t)d;
}

static void fillHeader(MsgHeader& h, uint8_t type) {
  h.magic = WOLKE_MAGIC;
  h.proto = WOLKE_PROTO;
  h.type = type;
  h.seq = mySeq++;
}

static void setPeer(const uint8_t* mac, const uint8_t* lmk) {
  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;                 // = aktueller Kanal
  p.ifidx = WIFI_IF_STA;
  p.encrypt = lmk != nullptr;
  if (lmk) memcpy(p.lmk, lmk, 16);
  if (esp_now_is_peer_exist(mac)) esp_now_mod_peer(&p);
  else esp_now_add_peer(&p);
}

static void onSent(const uint8_t* mac, esp_now_send_status_t status) {
  if (pendActive && memcmp(mac, pendMac, 6) == 0) {
    pendSendOk = status == ESP_NOW_SEND_SUCCESS;
    pendSendDone = true;
  }
  if (helpAckSending && knopfKnown && memcmp(mac, knopfMac, 6) == 0) {
    helpAckSending = false;
    if (status == ESP_NOW_SEND_SUCCESS) helpAckDelivered = true;
  }
}

static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < (int)sizeof(MsgHeader)) return;
  MsgHeader h;
  memcpy(&h, data, sizeof(h));
  if (h.magic != WOLKE_MAGIC || h.proto != WOLKE_PROTO) return;

  if (h.type == MSG_PAIR_REQ) {
    if (!pairing || len < (int)sizeof(MsgReport)) return;
    MsgReport r;
    memcpy(&r, data, sizeof(r));
    if (!pendActive || memcmp(pendMac, mac, 6) != 0) {
      memcpy(pendMac, mac, 6);
      esp_fill_random(pendLmk, 16);
    }
    pendActive = true;
    pendSendDone = false;
    setPeer(mac, nullptr);                         // Antwort noch unverschlüsselt
    MsgPairAck a;
    memset(&a, 0, sizeof(a));
    fillHeader(a.h, MSG_PAIR_ACK);
    memcpy(a.lmk, pendLmk, 16);
    a.channel = netChannel();
    a.epoch = timeValid() ? epochNow() : 0;
    a.helpMode = S.helpMode;
    a.secToHeartbeat = secToHeartbeat();
    esp_now_send(mac, (const uint8_t*)&a, sizeof(a));
    LinkEv e;
    memset(&e, 0, sizeof(e));
    e.type = EV_PAIR_SEEN;
    memcpy(e.mac, mac, 6);
    e.mvRest = r.mvRest; e.mvLoad = r.mvLoad;
    xQueueSend(evq, &e, 0);
    return;
  }

  if (!knopfKnown || memcmp(mac, knopfMac, 6) != 0) return;
  if (h.type != MSG_PRESS && h.type != MSG_HELP && h.type != MSG_HEARTBEAT) return;
  if (len < (int)sizeof(MsgReport)) return;
  MsgReport r;
  memcpy(&r, data, sizeof(r));

  MsgAck a;                                       // sofort bestätigen, damit der Knopf schlafen kann
  memset(&a, 0, sizeof(a));
  fillHeader(a.h, MSG_ACK);
  a.ackSeq = h.seq;
  a.epoch = timeValid() ? epochNow() : 0;
  a.helpMode = S.helpMode;
  a.channel = netChannel();
  a.secToHeartbeat = secToHeartbeat();
  esp_now_send(mac, (const uint8_t*)&a, sizeof(a));

  if (seqValid && h.seq == lastSeq) return;       // Wiederholung derselben Nachricht
  lastSeq = h.seq;
  seqValid = true;
  LinkEv e;
  memset(&e, 0, sizeof(e));
  e.type = h.type == MSG_PRESS ? EV_PRESS : h.type == MSG_HELP ? EV_HELP : EV_REPORT;
  e.mvRest = r.mvRest; e.mvLoad = r.mvLoad;
  xQueueSend(evq, &e, 0);
}

void linkInit() {
  evq = xQueueCreate(12, sizeof(LinkEv));
  if (esp_now_init() != ESP_OK) { Serial.println("[Funk] ESP-NOW Start fehlgeschlagen"); return; }
  esp_now_set_pmk(WOLKE_PMK);
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  if (K.paired) {
    memcpy(knopfMac, K.mac, 6);
    setPeer(K.mac, K.lmk);
    knopfKnown = true;
    Serial.printf("[Funk] Wolkenknopf %02X:%02X:%02X:%02X:%02X:%02X bekannt\n", K.mac[0], K.mac[1], K.mac[2], K.mac[3], K.mac[4], K.mac[5]);
  }
  ready = true;
  Serial.printf("[Funk] bereit, Station-MAC %s\n", WiFi.macAddress().c_str());
}

void linkPairStart() { pendActive = false; pairing = true; Serial.println("[Funk] Koppeln: warte auf Wolkenknopf"); }
void linkPairStop() {
  pairing = false;
  if (pendActive && ready) {     // abgebrochene Kopplung aufräumen
    if (K.paired && memcmp(pendMac, K.mac, 6) == 0) setPeer(K.mac, K.lmk);
    else esp_now_del_peer(pendMac);
  }
  pendActive = false;
}
bool linkPairing() { return pairing; }

void linkUnpair() {
  if (!K.paired) return;
  if (ready) esp_now_del_peer(K.mac);
  knopfKnown = false;
  K.paired = 0;
  storeSaveKnopf();
}

void linkSendHelpAck() {
  helpAckUntil = millis() + 60000;
  helpAckNext = 0;
  helpAckDelivered = false;
}

void linkLoop() {
  if (!ready) return;
  LinkEv e;
  while (xQueueReceive(evq, &e, 0) == pdTRUE) {
    switch (e.type) {
      case EV_PAIR_SEEN:
        pendMvRest = e.mvRest; pendMvLoad = e.mvLoad;
        pendSeenMs = millis();
        Serial.println("[Funk] Kopplungsanfrage, Antwort gesendet");
        break;
      case EV_PRESS:
        Serial.printf("[Funk] Knopf gedrückt (%u mV)\n", e.mvRest);
        onKnopfReport(e.mvRest, e.mvLoad);
        onKnopfPress();
        break;
      case EV_HELP:
        Serial.printf("[Funk] HILFERUF (%u mV)\n", e.mvRest);
        onKnopfReport(e.mvRest, e.mvLoad);
        onKnopfHelp(false);
        break;
      case EV_REPORT:
        Serial.printf("[Funk] Herzschlag: %u mV Ruhe, %u mV Senden\n", e.mvRest, e.mvLoad);
        onKnopfReport(e.mvRest, e.mvLoad);
        break;
    }
  }
  // Kopplung abschließen, sobald die Antwort sicher draußen ist
  if (pairing && pendActive && pendSendDone && millis() - pendSeenMs > 300) {
    if (pendSendOk) {
      if (K.paired && memcmp(K.mac, pendMac, 6) != 0) esp_now_del_peer(K.mac);   // alten Knopf entfernen
      setPeer(pendMac, pendLmk);
      memcpy(knopfMac, pendMac, 6);
      knopfKnown = true;
      seqValid = false;
      pairing = false;
      pendActive = false;
      Serial.printf("[Funk] Wolkenknopf gekoppelt: %02X:%02X:%02X:%02X:%02X:%02X\n", pendMac[0], pendMac[1], pendMac[2], pendMac[3], pendMac[4], pendMac[5]);
      onKnopfPaired(pendMac, pendLmk, pendMvRest, pendMvLoad);
    } else {
      pendSendDone = false;     // Knopf hat die Antwort nicht bekommen – er fragt erneut
    }
  }
  // "Ich komme" an den Knopf melden (er wartet nach einem Hilferuf ca. 2 Minuten darauf)
  if (helpAckUntil) {
    if ((int32_t)(millis() - helpAckUntil) >= 0 || helpAckDelivered) {
      if (helpAckDelivered) Serial.println("[Funk] \"Ich komme\" beim Knopf angekommen");
      helpAckUntil = 0;
    } else if ((int32_t)(millis() - helpAckNext) >= 0 && knopfKnown) {
      MsgHeader m;
      fillHeader(m, MSG_HELP_ACK);
      helpAckSending = true;
      esp_now_send(knopfMac, (const uint8_t*)&m, sizeof(m));
      helpAckNext = millis() + 2000;
    }
  }
}
