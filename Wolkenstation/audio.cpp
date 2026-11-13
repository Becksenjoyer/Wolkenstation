// Wolkenstation – Töne über den eingebauten DAC (GPIO26 -> Verstärker -> SPEAK-Buchse).
// Kleiner Synthesizer: Sinus/Dreieck-Stimmen mit Hüllkurve, gleiche Klänge wie im Klick-Prototyp.
// WICHTIG: Nur DAC-Kanal 2 (GPIO26) einschalten. GPIO25 (DAC 1) ist beim CYD die Touch-Taktleitung!
//
// Gegen Rauschen und Brummen (ab 0.1.4):
//  - Der Verstärker auf dem Board ist immer an und verstärkt alles, was an GPIO26 ankommt.
//  - Bis 0.1.3 hat der DAC in den Pausen fest die Mitte (128) gehalten. Diese Mitte ist aber nur so sauber wie
//    die 3,3-V-Versorgung (WLAN, Display, Licht ziehen dort Stromstöße), und der Pin fängt den Touch-Takt von
//    GPIO25 direkt daneben ein -> dauerndes Rauschen/Brummen aus dem Lautsprecher.
//  - Jetzt ist GPIO26 ohne Ton ein normaler Ausgang fest auf LOW (wie Diagnose 2). Das ist niederohmig und ruhig.
//  - Damit es beim Umschalten nicht knackt, fährt der Ausgang vor jedem Ton sanft von 0 V auf die Mitte und
//    danach sanft wieder auf 0 V (ca. 0,19 s, so langsam, dass der Lautsprecher es nicht hörbar wiedergibt).
//  - Beim eingebauten DAC bedeutet der Abtastwert 0 auch 0 V. Darum passen die Null-Puffer der I2S-Treiber
//    jetzt zum Ruhepegel; früher gab es dadurch beim Start jedes Tons einen Sprung 128 -> 0 -> 128 (Knacken).
#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/dac.h>
#include <math.h>
#include "app.h"
#include "platform.h"

static const int   PIN_AUDIO   = 26;
static const int   SAMPLE_RATE = 22050;               // Abtastrate
static const int   FRAMES      = 256;                 // Abtastwerte pro Block
static const int   DMA_BUFS    = 6;
static const int   RAMP        = 16 * FRAMES;         // sanftes Hoch-/Runterfahren, ca. 0,19 s
static const int   HOLD_BLOCKS = 43;                  // ca. 0,5 s Stille abwarten, bevor runtergefahren wird
static const int   TAIL_BLOCKS = DMA_BUFS + 2;        // so viele Null-Blöcke nachschieben, bis sicher alles ausgespielt ist
#ifndef WOLKE_AUDIO_GAIN
#define WOLKE_AUDIO_GAIN 1.0f
#endif
static const float DEV_GAIN = WOLKE_AUDIO_GAIN;   // Anpassung an den 8-Bit-DAC
static const int   MAXV     = 16;
// Pegel so eingemessen, dass bei 100 % jede Melodie ca. 90 % und der Hilferuf ca. 95 % Vollaussteuerung erreicht
static const float MEL_PEAK[3] = {0.64f, 1.17f, 0.76f};   // Glöckchen, Vogelzwitschern, Sonnenaufgang
static const float HELP_PEAK   = 0.95f;

enum Wave : uint8_t { W_SINE, W_TRI };
struct Voice {
  bool     on;
  uint8_t  wave;
  uint32_t start;      // Abtastzeitpunkt (absolut)
  uint32_t len;        // Länge in Abtastwerten
  uint32_t atk;        // Anstieg in Abtastwerten
  uint32_t glide;      // Tonhöhen-Gleiten in Abtastwerten
  float    phase, inc, incMul;
  float    env, envMulA, envMulD;
};

static Voice voices[MAXV];
static SemaphoreHandle_t mtx;
static volatile uint32_t sampleClock = 0;
static float sineLut[1024];
static bool ready = false;
static TaskHandle_t audioTaskHandle = nullptr;

// Morgen-Ton: -2,5 dB je 10 % (der 8-Bit-DAC hat wenig Dynamik, sonst wäre 30 % kaum hörbar)
static inline float melodyGain(int v) { return v <= 0 ? 0.0f : powf(10.0f, (v - 100) / 80.0f); }
// Hilferuf: -3 dB je Stufe, damit 70 -> 80 -> 90 -> 100 % hörbar lauter wird
static inline float helpGain(int v) { return v <= 0 ? 0.0f : powf(10.0f, (v - 100) / 66.67f); }

static void addVoice(uint8_t wave, float f, float f2, uint32_t base, float t, float dur, float g) {
  if (g < 0.0002f) return;
  for (int i = 0; i < MAXV; i++) {
    Voice& v = voices[i];
    if (v.on) continue;
    v.on = true;
    v.wave = wave;
    v.start = base + (uint32_t)(t * SAMPLE_RATE);
    v.len = (uint32_t)(dur * SAMPLE_RATE);
    v.atk = (uint32_t)(0.01f * SAMPLE_RATE);
    if (v.len <= v.atk + 1) v.len = v.atk + 2;
    v.phase = 0;
    v.inc = f / SAMPLE_RATE;
    if (f2 > 0) {
      v.glide = (uint32_t)(dur * 0.6f * SAMPLE_RATE);
      v.incMul = powf(f2 / f, 1.0f / v.glide);
    } else { v.glide = 0; v.incMul = 1; }
    v.env = 0.0001f;
    v.envMulA = powf(g / 0.0001f, 1.0f / v.atk);
    v.envMulD = powf(0.0001f / g, 1.0f / (v.len - v.atk));
    return;
  }
}

static void clearVoices() { for (int i = 0; i < MAXV; i++) voices[i].on = false; }
static bool anyVoice() { for (int i = 0; i < MAXV; i++) if (voices[i].on) return true; return false; }

void audioMelody(int mel, int vol) {
  if (!ready || vol <= 0) return;
  float g = MEL_PEAK[mel < 0 || mel > 2 ? 0 : mel] * melodyGain(vol);
  xSemaphoreTake(mtx, portMAX_DELAY);
  clearVoices();
  uint32_t base = sampleClock + (uint32_t)(0.03f * SAMPLE_RATE);
  if (mel == 0) {            // Glöckchen
    const float freq[3] = {1318.5f, 1046.5f, 1568.0f}, dly[3] = {0, 0.2f, 0.4f};
    for (int i = 0; i < 3; i++) {
      addVoice(W_SINE, freq[i], 0, base, dly[i], 1.2f, g);
      addVoice(W_SINE, freq[i] * 2.76f, 0, base, dly[i], 0.5f, g * 0.25f);
    }
  } else if (mel == 1) {     // Vogelzwitschern
    const float dly[5] = {0, 0.12f, 0.24f, 0.52f, 0.64f};
    for (int i = 0; i < 5; i++) addVoice(W_SINE, 2500.0f + i * 150, 4300.0f, base, dly[i], 0.09f, g * 0.8f);
  } else {                   // Sonnenaufgang
    const float freq[5] = {523.25f, 659.25f, 783.99f, 1046.5f, 1318.5f};
    for (int i = 0; i < 5; i++) addVoice(W_TRI, freq[i], 0, base, i * 0.12f, 0.9f, g * 0.9f);
  }
  xSemaphoreGive(mtx);
  if (audioTaskHandle) xTaskNotifyGive(audioTaskHandle);
}

// Hilferuf: zweimal "di-di-DING" (E6, E6, A6). Ruf 1 = 70 %, dann 80, 90, ab Ruf 4 = 100 %.
void audioHelp(int level) {
  if (!ready) return;
  int v = 70 + 10 * level;
  if (v > 100) v = 100;
  float g = HELP_PEAK * helpGain(v);
  xSemaphoreTake(mtx, portMAX_DELAY);
  clearVoices();
  uint32_t base = sampleClock + (uint32_t)(0.03f * SAMPLE_RATE);
  const float freq[3] = {1318.5f, 1318.5f, 1760.0f}, dly[3] = {0, 0.17f, 0.34f}, dur[3] = {0.14f, 0.14f, 0.62f};
  for (int o = 0; o < 2; o++) {
    for (int i = 0; i < 3; i++) {
      addVoice(W_TRI, freq[i], 0, base, o * 1.0f + dly[i], dur[i], g);
      addVoice(W_SINE, freq[i] * 2, 0, base, o * 1.0f + dly[i], dur[i] * 0.6f, g * 0.25f);
    }
  }
  xSemaphoreGive(mtx);
  if (audioTaskHandle) xTaskNotifyGive(audioTaskHandle);
}

void audioStop() {
  if (!ready) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  clearVoices();
  xSemaphoreGive(mtx);
}

// Begrenzer: bis 85 % linear, darüber weich bis 100 % (kein hartes Übersteuern)
static inline float limit(float x) {
  float a = fabsf(x);
  if (a <= 0.85f) return x;
  float y = 0.85f + 0.15f * tanhf((a - 0.85f) / 0.15f);
  return x < 0 ? -y : y;
}

// Ausgabe an: Pin vom festen LOW zurück an den DAC (beide 0 V -> kein Sprung), DAC wird vom I2S-Takt gespeist.
static void outputOn() {
  dac_output_voltage(DAC_CHANNEL_2, 0);
  dac_output_enable(DAC_CHANNEL_2);
  i2s_zero_dma_buffer(I2S_NUM_0);          // Nullen = 0 V beim eingebauten DAC = Startpunkt der Rampe
  dac_i2s_enable();
  i2s_start(I2S_NUM_0);
}

// Ausgabe aus: Takt stoppen, DAC abschalten, Verstärker-Eingang hart auf Masse -> nimmt kein Rauschen mehr auf.
static void outputOff() {
  i2s_stop(I2S_NUM_0);
  dac_i2s_disable();
  dac_output_voltage(DAC_CHANNEL_2, 0);
  dac_output_disable(DAC_CHANNEL_2);
  pinMode(PIN_AUDIO, OUTPUT);              // wie Diagnose 2 / Start: normaler Ausgang, LOW
  digitalWrite(PIN_AUDIO, LOW);
}

// Einen Block Töne mischen (Mitte = 128). Nur mit gesperrtem mtx aufrufen.
static void mixBlock(uint16_t* buf) {
  uint32_t t0 = sampleClock;
  for (int n = 0; n < FRAMES; n++) {
    uint32_t t = t0 + n;
    float mix = 0;
    for (int i = 0; i < MAXV; i++) {
      Voice& v = voices[i];
      if (!v.on) continue;
      int32_t k = (int32_t)(t - v.start);
      if (k < 0) continue;
      if ((uint32_t)k >= v.len) { v.on = false; continue; }
      if ((uint32_t)k < v.atk) v.env *= v.envMulA; else v.env *= v.envMulD;
      if ((uint32_t)k < v.glide) v.inc *= v.incMul;
      float s;
      if (v.wave == W_SINE) s = sineLut[(int)(v.phase * 1024) & 1023];
      else s = 1.0f - 4.0f * fabsf(v.phase - 0.5f);
      mix += s * v.env;
      v.phase += v.inc;
      if (v.phase >= 1.0f) v.phase -= 1.0f;
    }
    float y = limit(mix * DEV_GAIN);
    int u = 128 + (int)lroundf(y * 127.0f);
    u = u < 0 ? 0 : (u > 255 ? 255 : u);
    uint16_t w = (uint16_t)(u << 8);     // DAC nimmt nur die oberen 8 Bit
    buf[n * 2] = w;
    buf[n * 2 + 1] = w;
  }
  sampleClock = t0 + FRAMES;
}

// Einen Block Rampe: Ruhepegel Schritt für Schritt Richtung Mitte (dir = +1) oder Richtung 0 V (dir = -1).
static void rampBlock(uint16_t* buf, int& ramp, int dir) {
  for (int n = 0; n < FRAMES; n++) {
    ramp += dir;
    if (ramp < 0) ramp = 0; else if (ramp > RAMP) ramp = RAMP;
    float x = (float)ramp / RAMP;
    int u = (int)lroundf(x * x * (3.0f - 2.0f * x) * 128.0f);   // weiche S-Kurve 0 .. 128
    uint16_t w = (uint16_t)(u << 8);
    buf[n * 2] = w;
    buf[n * 2 + 1] = w;
  }
}

static void audioTask(void*) {
  static uint16_t buf[FRAMES * 2];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);       // schlafen, bis ein Ton gespielt werden soll
    xSemaphoreTake(mtx, portMAX_DELAY);
    bool any = anyVoice();
    xSemaphoreGive(mtx);
    if (!any) continue;                            // alte Benachrichtigung, Ton ist schon gespielt

    outputOn();
    int ramp = 0;        // 0 = 0 V (Ruhe), RAMP = Mitte (bereit für Töne)
    int quiet = 0;       // Stille-Blöcke in der Mitte
    int tail = 0;        // Null-Blöcke am Ende
    for (;;) {
      xSemaphoreTake(mtx, portMAX_DELAY);
      bool pending = anyVoice();                   // auch Töne, die erst gleich starten
      if (ramp == RAMP && (pending || quiet < HOLD_BLOCKS)) {
        mixBlock(buf);                             // Töne spielen (die Zeit läuft nur hier weiter)
        quiet = anyVoice() ? 0 : quiet + 1;
      } else {
        rampBlock(buf, ramp, pending ? 1 : -1);    // Ton wartet -> hochfahren, sonst runter
        if (pending) quiet = 0;
      }
      xSemaphoreGive(mtx);
      size_t written = 0;
      i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
      if (ramp == 0 && !pending) { if (++tail >= TAIL_BLOCKS) break; }
      else tail = 0;
    }
    outputOff();
  }
}

void audioInit() {
  for (int i = 0; i < 1024; i++) sineLut[i] = sinf(2.0f * (float)M_PI * i / 1024.0f);
  mtx = xSemaphoreCreateMutex();
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = DMA_BUFS;
  cfg.dma_buf_len = FRAMES;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;               // bei Aussetzern 0 V statt alter Daten
  esp_err_t e = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  if (e != ESP_OK) { Serial.printf("[Ton] I2S-Fehler %d\n", (int)e); return; }
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);   // nur GPIO26 (DAC 2) – niemals BOTH (GPIO25 = Touch)
  outputOff();                                 // Ruhe (Pin fest auf LOW), bis der erste Ton kommt
  ready = true;
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 3, &audioTaskHandle, 1);
  Serial.println("[Ton] bereit (DAC an GPIO26, in Pausen fest auf LOW)");
}
