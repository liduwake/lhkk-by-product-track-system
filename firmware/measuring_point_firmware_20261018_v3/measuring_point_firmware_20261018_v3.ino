/* =============================================================================
   File      : measuring_point_firmware_20261018_v3.ino
   Purpose   : Firmware of one measuring point (bench version). Reads the
               weighing platform, converts counts to grams, runs the state
               machine of decision D3, prints one EVENT line to the serial
               port for every addition, removal and emptying, and sends every
               event (and every MAINT signal) to ThingsBoard as telemetry.
   Thesis    : Developing a data-driven by-product tracking system to support
               consistent data collection and sharing (LHKK / HAMK)
   Section   : 4.6.3  Firmware and state machine (Figure 7),  4.6.4  Uplink
   Author    : Duxing Li, HAMK
   Date      : 2026-10-18
   Version   : v3   (v2 with the secrets moved to secrets.h; v1 and v2 stay valid)
   -----------------------------------------------------------------------------
   Changes from measuring_point_firmware_20261018_v2
     1. The Wi-Fi name, the Wi-Fi password and the ThingsBoard device token are
        no longer in this file. They are in secrets.h, in the same folder.
        This file can be shown, printed or sent without secrets in it.
     2. Nothing else is changed. The state machine, the parameters, the serial
        output and the uplink are the same as in v2.

   Changes in v2 compared with measuring_point_firmware_20261018_v1
     1. The state machine, the parameters and the EVENT / MAINT lines on the
        serial port are NOT changed. v2 only adds the uplink.
     2. Wi-Fi station connection and NTP time (UTC). The time stamp of an event
        is calculated from its uptime_ms, so an event that happens before the
        clock is synchronised still gets the correct time later.
     3. Every EVENT and every MAINT signal is put in a queue (32 entries) and
        sent to ThingsBoard by a separate task on the second core, so a slow
        network never stops the weighing. If the network is down the entries
        wait in the queue and are sent later (in RAM only; lost at restart).
     4. Telemetry values: measuring_point_id, event_type, weight_change,
        content_after   (or: measuring_point_id, maintenance).
     5. New serial command n = network status.
     6. If secrets.h is not filled in, the uplink is off and the program
        works as v1.
   -----------------------------------------------------------------------------
   Hardware  : same as hx711_raw_reader_20261018_v3 (DAT -> GPIO 22, CLK -> GPIO 27)
   Serial    : 115200 baud
   Commands  : z = install zero (platform must be EMPTY, no box)
               v = switch the one-line-per-second status on / off
               i = print the parameters
               n = print the network status
   Bench use : The Red Label bottle (1 527 g) is the "box" (TARE_G). Content =
               the other bottles put on top of it. Lifting the Red Label (with
               everything on it) and putting it back alone = emptying.
   -----------------------------------------------------------------------------
   Known limits (to be written in 4.6.3 and 5.4)
     * A zero step of several hundred grams while a load is on the platform
       looks like a removal or an addition of that size. It cannot be told
       apart from a real event by the firmware. The threshold must be above the
       largest zero step seen on the bench (about 340 g).
     * A lift shorter than the 1-s window is not recognised as off-panel.
     * Changes smaller than CHANGE_THRESHOLD_G are not recorded (D3).
     * The zero reference is kept in RAM only. After a restart send z again.
     * The TLS connection is encrypted but the server certificate is NOT
       checked (setInsecure). The device token is in the URL path, as the
       ThingsBoard HTTP API requires. Pin the root certificate before any real
       use. Only plain WPA2 Wi-Fi works (no WPA2-Enterprise).
     * Queued entries are lost at a restart or when 32 are waiting.
     * The ThingsBoard HTTP telemetry API:
       https://thingsboard.io/docs/paas/eu/reference/http-api/telemetry/
   Parameters are provisional bench values. Final values come from the real
   panel and the 240 L container.
   AI usage  : First draft written with AI assistance (Claude, 2026-10-18),
               then reviewed, compiled and tested by the author. The state
               machine logic was also run against simulated sample streams on
               a PC before the first run on the board. The uplink part (v2)
               was written with AI assistance and syntax-checked against stubs;
               it has to be tested with a real network and a real ThingsBoard
               device.
   SECRETS   : The Wi-Fi password and the device token are typed into
               secrets.h by the author. Do not show secrets.h in the thesis, in
               a screenshot, in a chat or in a repository. Only this .ino file
               is part of the code listing.
   ============================================================================= */

#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <sys/time.h>

// ---- pins -------------------------------------------------------------------
const int PIN_DAT = 22;
const int PIN_CLK = 27;

// ---- configuration of this measuring point (Table 4) -----------------------
const char* MP_ID               = "MP-01";
const long  TARE_G              = 1527;   // weight of the empty box (bench: Red Label bottle)
const long  CHANGE_THRESHOLD_G  = 300;    // bench value; Table 4 provisional value is 1 000

// ---- network settings ----------------------------------------------------------
// The secret values are in secrets.h (same folder). Not shown in any listing.
#include "secrets.h"
const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* TB_TOKEN      = SECRET_TB_TOKEN;
const char* TB_HOST       = "eu.thingsboard.cloud";      // not a secret

// ---- calibration and measured behaviour (bench log) -------------------------
const float FACTOR              = 27.7f;  // counts per gram, bench calibration 2026-10-18
const long  STABLE_SPAN_COUNTS  = 600;    // 1-s window span still counted as stable (noise <= 260)
const long  ZERO_BAND_G         = 100;    // narrow band of rule (b), about 2 770 counts
const float DRIFT_G_PER_S       = 0.2f;   // fastest reference drift that is followed

// ---- timing ----------------------------------------------------------------
const int           WIN_N              = 11;      // samples in the window (about 1 s at 10.7 SPS)
const unsigned long SETTLE_TIMEOUT_MS  = 30000;
const unsigned long OFF_ZERO_WAIT_MS   = 3000;    // box off the panel this long before zero refresh
const unsigned long MIN_INTERVAL_MS    = 30;      // nominal interval is 93 ms
const int           BAD_INTERVAL_LIMIT = 5;
const int           ZERO_N             = 20;      // samples averaged for z
const unsigned long STATUS_MS          = 1000;
const int           UP_QUEUE_LEN       = 32;
const unsigned long WIFI_WAIT_MS       = 15000;
const unsigned long NTP_WAIT_MS        = 10000;
const unsigned long RETRY_MS           = 10000;

const long OFF_PANEL_G = TARE_G / 2;              // below this the box is off the panel

// ---- state -----------------------------------------------------------------
enum State { ST_INIT, ST_STABLE, ST_SETTLING, ST_OFF_PANEL, ST_FAULT };
State state = ST_INIT;

int32_t zeroRef = 0;
int32_t win[WIN_N];
int     winCount = 0;
int     winIdx   = 0;

float         refG           = 0;      // last accepted stable gross weight, grams
bool          haveBefore     = false;  // content before the lift is known
float         contentBeforeG = 0;
unsigned long settleStart    = 0;
unsigned long offStableSince = 0;
bool          offZeroDone    = false;
bool          maintSettle    = false;
bool          maintZeroBand  = false;

unsigned long sampleMs = 0;            // time stamp of the latest sample
unsigned long prevMs   = 0;
bool          havePrev = false;
int           badRun   = 0;

bool          verbose    = true;
unsigned long lastStatus = 0;
float         lastGrossG = 0;
long          lastSpan   = 0;
bool          lastValid  = false;

// ---- uplink: queue between the weighing loop (core 1) and the network task (core 0)
struct UpItem {
  uint8_t       kind;        // 0 = event, 1 = maintenance signal
  char          type[24];    // event_type, or the MAINT code
  long          change;      // weight_change, g
  long          after;       // content_after, g
  unsigned long uptimeMs;    // millis() when it happened
};
QueueHandle_t upQueue    = NULL;
bool          uplinkOn   = false;
bool          wifiUp     = false;
bool          timeOk     = false;
int64_t       bootEpochMs = 0;          // UTC ms at millis() == 0, set when NTP is ready
unsigned long upSent     = 0;
unsigned long upFailed   = 0;
int           upLastCode = 0;

// ---- small helpers -----------------------------------------------------------
const char* stateName(State s) {
  switch (s) {
    case ST_INIT:      return "INIT";
    case ST_STABLE:    return "STABLE";
    case ST_SETTLING:  return "SETTLING";
    case ST_OFF_PANEL: return "OFF_PANEL";
    case ST_FAULT:     return "FAULT";
  }
  return "?";
}

void upPush(const UpItem& it) {
  if (!uplinkOn) return;
  if (xQueueSend(upQueue, &it, 0) != pdTRUE) {
    Serial.println("# uplink queue full: entry dropped");
  }
}

void maint(const char* code, const char* text) {
  Serial.print("MAINT ");
  Serial.print(code);
  Serial.print(" - ");
  Serial.println(text);
  UpItem it;
  it.kind = 1;
  strncpy(it.type, code, sizeof(it.type) - 1);
  it.type[sizeof(it.type) - 1] = 0;
  it.change = 0;
  it.after = 0;
  it.uptimeMs = millis();
  upPush(it);
}

void note(const char* text) {
  Serial.print("# ");
  Serial.println(text);
}

void emitEvent(const char* type, long change, long after, unsigned long ms) {
  char buf[200];
  snprintf(buf, sizeof(buf),
           "EVENT {\"measuring_point_id\":\"%s\",\"event_type\":\"%s\","
           "\"weight_change\":%ld,\"content_after\":%ld,\"uptime_ms\":%lu}",
           MP_ID, type, change, after, ms);
  Serial.println(buf);
  UpItem it;
  it.kind = 0;
  strncpy(it.type, type, sizeof(it.type) - 1);
  it.type[sizeof(it.type) - 1] = 0;
  it.change = change;
  it.after = after;
  it.uptimeMs = ms;
  upPush(it);
}

void winReset() {
  winCount = 0;
  winIdx = 0;
  lastValid = false;
}

void winPush(int32_t raw) {
  win[winIdx] = raw;
  winIdx = (winIdx + 1) % WIN_N;
  if (winCount < WIN_N) winCount++;
}

bool winFull() { return winCount >= WIN_N; }

int32_t winSpan() {
  int32_t mn = win[0], mx = win[0];
  for (int i = 1; i < WIN_N; i++) {
    if (win[i] < mn) mn = win[i];
    if (win[i] > mx) mx = win[i];
  }
  return mx - mn;
}

int32_t winMeanRaw() {
  int64_t s = 0;
  for (int i = 0; i < WIN_N; i++) s += win[i];
  return (int32_t)(s / WIN_N);
}

float rawToGrams(int32_t raw) { return (float)(raw - zeroRef) / FACTOR; }

// After a fault or a restart the new level becomes the reference. No event.
void takeAsReference(float g) {
  if (g < OFF_PANEL_G) {
    state = ST_OFF_PANEL;
    haveBefore = false;
    offStableSince = 0;
    offZeroDone = false;
    note("level below the box weight: waiting for the box");
  } else {
    state = ST_STABLE;
    refG = g;
  }
}

void enterFault(const char* code, const char* text) {
  if (state != ST_FAULT) {
    maint(code, text);
    state = ST_FAULT;
    haveBefore = false;
  }
  winReset();
}

// ---- HX711 (same protocol as hx711_raw_reader_20261018_v3) -------------------
int32_t hx711_read() {
  unsigned long t0 = millis();
  while (digitalRead(PIN_DAT) == HIGH) {
    if (millis() - t0 > 500) return 0x7FFFFFFF;   // timeout marker
    delay(1);
  }
  sampleMs = millis();

  uint32_t v = 0;
  noInterrupts();
  for (int i = 0; i < 24; i++) {
    digitalWrite(PIN_CLK, HIGH);
    delayMicroseconds(1);
    v = (v << 1) | (uint32_t)digitalRead(PIN_DAT);
    digitalWrite(PIN_CLK, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(PIN_CLK, HIGH);                    // 25th pulse: channel A, gain 128
  delayMicroseconds(1);
  digitalWrite(PIN_CLK, LOW);
  delayMicroseconds(1);
  interrupts();

  if (v & 0x800000UL) v |= 0xFF000000UL;
  return (int32_t)v;
}

// ---- state machine -----------------------------------------------------------
void processSample(int32_t raw, unsigned long ms) {
  // 1. Sample-rate plausibility. A floating DAT line gives thousands of
  //    "samples" per second; the real HX711 gives about 11.
  unsigned long dtMs = havePrev ? (ms - prevMs) : 93;
  if (havePrev) {
    if (dtMs < MIN_INTERVAL_MS) badRun++; else badRun = 0;
  }
  prevMs = ms;
  havePrev = true;
  if (badRun >= BAD_INTERVAL_LIMIT) {
    enterFault("RATE", "sample rate not plausible - check the DAT wire");
    return;
  }
  if (badRun > 0) return;                         // discard suspicious sample

  winPush(raw);
  if (!winFull()) return;

  long  span = winSpan();
  float g    = rawToGrams(winMeanRaw());
  lastGrossG = g;
  lastSpan   = span;
  lastValid  = true;

  switch (state) {

    case ST_INIT:
      break;                                      // waiting for z

    case ST_FAULT:
      if (span <= STABLE_SPAN_COUNTS) {
        note("signal back: level taken as the new reference, no event");
        takeAsReference(g);
      }
      break;

    case ST_STABLE: {
      float diff = g - refG;
      if (fabsf(diff) > (float)CHANGE_THRESHOLD_G) {
        state = ST_SETTLING;
        settleStart = ms;
        maintSettle = false;
      } else {
        // follow slow drift, but never faster than DRIFT_G_PER_S
        float step = DRIFT_G_PER_S * (float)dtMs / 1000.0f;
        if (diff >  step) diff =  step;
        if (diff < -step) diff = -step;
        refG += diff;
      }
      break;
    }

    case ST_SETTLING:
      if (span <= STABLE_SPAN_COUNTS) {
        if (g < (float)OFF_PANEL_G && refG >= (float)OFF_PANEL_G) {
          // the box has left the panel
          haveBefore = true;
          contentBeforeG = refG - (float)TARE_G;
          offStableSince = ms;
          offZeroDone = false;
          maintZeroBand = false;
          state = ST_OFF_PANEL;
          note("box off the panel");
        } else {
          float delta = g - refG;
          long  after = lroundf(g - (float)TARE_G);
          if (after < -CHANGE_THRESHOLD_G) {
            maint("NEGATIVE_CONTENT", "content below zero - zero or tare is wrong, no event");
          } else if (delta >= (float)CHANGE_THRESHOLD_G) {
            emitEvent("addition", lroundf(delta), after, ms);
          } else if (delta <= -(float)CHANGE_THRESHOLD_G) {
            emitEvent("removal", lroundf(delta), after, ms);
          } else {
            note("change below the threshold, no event");
          }
          refG = g;
          state = ST_STABLE;
        }
      } else if (ms - settleStart > SETTLE_TIMEOUT_MS && !maintSettle) {
        maint("SETTLE_TIMEOUT", "no stable reading for 30 s");
        maintSettle = true;
      }
      break;

    case ST_OFF_PANEL:
      if (span > STABLE_SPAN_COUNTS) {
        offStableSince = 0;                       // box is being moved
        break;
      }
      if (g < (float)OFF_PANEL_G) {
        if (offStableSince == 0) offStableSince = ms;
        if (!offZeroDone && ms - offStableSince >= OFF_ZERO_WAIT_MS) {
          if (fabsf(g) <= (float)ZERO_BAND_G) {
            int32_t oldZero = zeroRef;
            zeroRef = winMeanRaw();
            char buf[64];
            snprintf(buf, sizeof(buf), "zero refreshed, shift %ld counts", (long)(zeroRef - oldZero));
            note(buf);
            offZeroDone = true;
            maintZeroBand = false;
          } else if (!maintZeroBand) {
            maint("ZERO_OUT_OF_BAND", "empty platform reads outside the narrow band - zero NOT refreshed");
            maintZeroBand = true;
          }
        }
      } else {
        // the box is back
        float after = g - (float)TARE_G;
        if (haveBefore) {
          float drop = contentBeforeG - after;
          if (after < -(float)CHANGE_THRESHOLD_G) {
            maint("NEGATIVE_CONTENT", "content below zero - zero or tare is wrong, no event");
          } else if (drop >= (float)CHANGE_THRESHOLD_G) {
            emitEvent("emptying", lroundf(after - contentBeforeG), lroundf(after), ms);
          } else {
            note("box lifted and put back, no event");
          }
        } else {
          note("box placed, reference set, no event");
        }
        refG = g;
        haveBefore = false;
        state = ST_STABLE;
      }
      break;
  }
}

// ---- zero command ------------------------------------------------------------
void zeroNow() {
  if (state == ST_FAULT) {
    note("z refused: a fault is active. Fix the fault first");
    return;
  }
  int64_t sum = 0;
  int n = 0;
  for (int i = 0; i < ZERO_N; i++) {
    int32_t r = hx711_read();
    if (r == 0x7FFFFFFF || r == 0x7FFFFF || r == -8388608L) continue;
    sum += r;
    n++;
  }
  if (n < ZERO_N / 2) {
    note("z FAILED: not enough valid samples");
    return;
  }
  zeroRef = (int32_t)(sum / n);
  winReset();
  havePrev = false;
  badRun = 0;
  haveBefore = false;
  offStableSince = 0;
  offZeroDone = true;                              // z is the installation zero
  state = ST_OFF_PANEL;
  char buf[100];
  snprintf(buf, sizeof(buf), "zero set, reference = %ld (mean of %d samples). Put the box on now",
           (long)zeroRef, n);
  note(buf);
}

void printParameters() {
  char buf[200];
  snprintf(buf, sizeof(buf), "# %s  tare=%ld g  threshold=%ld g  factor=%.1f counts/g  zero band=%ld g  off-panel below %ld g",
           MP_ID, TARE_G, CHANGE_THRESHOLD_G, (double)FACTOR, ZERO_BAND_G, OFF_PANEL_G);
  Serial.println(buf);
}

void printStatus() {
  char buf[120];
  if (lastValid) {
    long gross = lroundf(lastGrossG);
    snprintf(buf, sizeof(buf), "t=%lus  %-9s  gross=%+7ld g  content=%+7ld g  span=%4ld c",
             millis() / 1000UL, stateName(state), gross, gross - TARE_G, lastSpan);
  } else {
    snprintf(buf, sizeof(buf), "t=%lus  %-9s  (filling the window)", millis() / 1000UL, stateName(state));
  }
  Serial.println(buf);
}

// ---- uplink task (runs on core 0) ---------------------------------------------
bool uplinkConfigured() {
  return strncmp(WIFI_SSID, "YOUR_", 5) != 0 &&
         strncmp(TB_TOKEN, "YOUR_", 5) != 0;
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) { wifiUp = true; return true; }
  wifiUp = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  wifiUp = (WiFi.status() == WL_CONNECTED);
  if (wifiUp) note("uplink: Wi-Fi connected");
  return wifiUp;
}

bool syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");     // UTC
  unsigned long t0 = millis();
  while (millis() - t0 < NTP_WAIT_MS) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec > 1700000000L) {                         // clock is set (after Nov 2023)
      bootEpochMs = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000 - (int64_t)millis();
      timeOk = true;
      note("uplink: time synchronised (NTP)");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  return false;
}

bool postItem(const UpItem& it) {
  long long ts = (long long)(bootEpochMs + (int64_t)it.uptimeMs);
  char body[256];
  if (it.kind == 0) {
    snprintf(body, sizeof(body),
             "{\"ts\":%lld,\"values\":{\"measuring_point_id\":\"%s\",\"event_type\":\"%s\","
             "\"weight_change\":%ld,\"content_after\":%ld}}",
             ts, MP_ID, it.type, it.change, it.after);
  } else {
    snprintf(body, sizeof(body),
             "{\"ts\":%lld,\"values\":{\"measuring_point_id\":\"%s\",\"maintenance\":\"%s\"}}",
             ts, MP_ID, it.type);
  }
  char url[200];
  snprintf(url, sizeof(url), "https://%s/api/v1/%s/telemetry", TB_HOST, TB_TOKEN);

  WiFiClientSecure client;
  client.setInsecure();                                    // see "Known limits"
  HTTPClient http;
  http.setTimeout(8000);
  int code = -1;
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST((uint8_t*)body, strlen(body));
    http.end();
  }
  upLastCode = code;
  return code >= 200 && code < 300;
}

void uplinkTask(void*) {
  for (;;) {
    if (!connectWifi()) { vTaskDelay(pdMS_TO_TICKS(RETRY_MS)); continue; }
    if (!timeOk && !syncTime()) { vTaskDelay(pdMS_TO_TICKS(RETRY_MS)); continue; }
    UpItem it;
    if (xQueuePeek(upQueue, &it, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (postItem(it)) {
        xQueueReceive(upQueue, &it, 0);                    // sent: remove it
        upSent++;
        char buf[64];
        snprintf(buf, sizeof(buf), "uplink: sent (HTTP %d), %lu sent in total", upLastCode, upSent);
        note(buf);
      } else {
        upFailed++;
        char buf[64];
        snprintf(buf, sizeof(buf), "uplink: send failed (code %d), will retry", upLastCode);
        note(buf);
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
      }
    }
  }
}

void printNetwork() {
  char buf[160];
  snprintf(buf, sizeof(buf), "# uplink: %s  wifi=%s  time=%s  waiting=%d  sent=%lu  failed=%lu  last code=%d",
           uplinkOn ? "on" : "OFF (secrets.h not filled in)",
           wifiUp ? "up" : "down", timeOk ? "ok" : "not yet",
           uplinkOn ? (int)uxQueueMessagesWaiting(upQueue) : 0, upSent, upFailed, upLastCode);
  Serial.println(buf);
}

// ---- Arduino entry points ----------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_CLK, OUTPUT);
  pinMode(PIN_DAT, INPUT);
  digitalWrite(PIN_CLK, LOW);
  delay(500);
  note("measuring_point_firmware_20261018_v3");
  printParameters();
  note("Platform EMPTY (no box), then send z.   v = status on/off   i = parameters   n = network");
  winReset();
  if (uplinkConfigured()) {
    upQueue = xQueueCreate(UP_QUEUE_LEN, sizeof(UpItem));
    uplinkOn = (upQueue != NULL);
  }
  if (uplinkOn) {
    xTaskCreatePinnedToCore(uplinkTask, "uplink", 12288, NULL, 1, NULL, 0);
    note("uplink on: Wi-Fi and ThingsBoard");
  } else {
    note("uplink OFF: fill in secrets.h");
  }
  lastStatus = millis();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'z' || c == 'Z') zeroNow();
    else if (c == 'v' || c == 'V') { verbose = !verbose; note(verbose ? "status on" : "status off"); }
    else if (c == 'i' || c == 'I') printParameters();
    else if (c == 'n' || c == 'N') printNetwork();
  }

  int32_t raw = hx711_read();
  if (raw == 0x7FFFFFFF) {
    enterFault("NO_DATA", "HX711 does not answer - check DAT / CLK / 5V / GND");
    delay(500);
    return;
  }
  if (raw == 0x7FFFFF || raw == -8388608L) {
    enterFault("SATURATED", "bridge not balanced - check the four plugs, do not press z");
    delay(500);
    return;
  }

  processSample(raw, sampleMs);

  if (verbose && millis() - lastStatus >= STATUS_MS) {
    printStatus();
    lastStatus = millis();
  }
}
