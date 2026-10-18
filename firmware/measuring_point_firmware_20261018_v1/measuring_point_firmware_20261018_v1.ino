/* =============================================================================
   File      : measuring_point_firmware_20261018_v1.ino
   Purpose   : Firmware of one measuring point (bench version). Reads the
               weighing platform, converts counts to grams, runs the state
               machine of decision D3 and prints one EVENT line to the serial
               port for every addition, removal and emptying. No network yet.
   Thesis    : Developing a data-driven by-product tracking system to support
               consistent data collection and sharing (LHKK / HAMK)
   Section   : 4.6.3  Firmware and state machine   (Figure 7)
   Author    : Duxing Li, HAMK
   Date      : 2026-10-18
   Version   : v1   (first version; built on hx711_raw_reader_20261018_v3)
   -----------------------------------------------------------------------------
   Changes from hx711_raw_reader_20261018_v3
     1. Counts are converted to grams with the bench factor 27.7 counts/g
        (second bench calibration, 2026-10-18).
     2. State machine: INIT, STABLE, SETTLING, OFF_PANEL, FAULT.
     3. Events are printed as "EVENT {...}" lines (addition, removal, emptying).
     4. Zero is refreshed only when the box is off the panel and the reading is
        inside a narrow band (rule b). Outside the band the firmware sends a
        MAINT signal and does not re-zero.
     5. Faults (no data, saturation, sample rate not plausible) give a MAINT
        signal and never an event. After a fault the new level is taken as the
        new reference without an event.
     6. Slow drift of the reference is followed (max 0.2 g/s) so that drift
        does not become an event.
     6b. An event that would leave the content below -threshold is not sent
        (MAINT NEGATIVE_CONTENT): the zero or the tare is wrong.
     7. The 'f' / 's' raw modes of v3 are removed. Use v3 for raw readings.
   -----------------------------------------------------------------------------
   Hardware  : same as hx711_raw_reader_20261018_v3 (DAT -> GPIO 22, CLK -> GPIO 27)
   Serial    : 115200 baud
   Commands  : z = install zero (platform must be EMPTY, no box)
               v = switch the one-line-per-second status on / off
               i = print the parameters
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
     * "uptime_ms" is time since boot. measured_at needs NTP time and comes
       with the uplink program.
   Parameters are provisional bench values. Final values come from the real
   panel and the 240 L container.
   AI usage  : First draft written with AI assistance (Claude, 2026-10-18),
               then reviewed, compiled and tested by the author. The state
               machine logic was also run against simulated sample streams on
               a PC before the first run on the board.
   ============================================================================= */

#include <math.h>

// ---- pins -------------------------------------------------------------------
const int PIN_DAT = 22;
const int PIN_CLK = 27;

// ---- configuration of this measuring point (Table 4) -----------------------
const char* MP_ID               = "MP-01";
const long  TARE_G              = 1527;   // weight of the empty box (bench: Red Label bottle)
const long  CHANGE_THRESHOLD_G  = 300;    // bench value; Table 4 provisional value is 1 000

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

void maint(const char* code, const char* text) {
  Serial.print("MAINT ");
  Serial.print(code);
  Serial.print(" - ");
  Serial.println(text);
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

// ---- Arduino entry points ----------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_CLK, OUTPUT);
  pinMode(PIN_DAT, INPUT);
  digitalWrite(PIN_CLK, LOW);
  delay(500);
  note("measuring_point_firmware_20261018_v1");
  printParameters();
  note("Platform EMPTY (no box), then send z.   v = status on/off   i = parameters");
  winReset();
  lastStatus = millis();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'z' || c == 'Z') zeroNow();
    else if (c == 'v' || c == 'V') { verbose = !verbose; note(verbose ? "status on" : "status off"); }
    else if (c == 'i' || c == 'I') printParameters();
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
