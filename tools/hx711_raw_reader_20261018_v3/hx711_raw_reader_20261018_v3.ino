/* =============================================================================
   File      : hx711_raw_reader_20261018_v3.ino
   Purpose   : Read raw HX711 counts from the bench weighing platform and give
               every sample a time stamp. Used to (a) measure the settling time
               after a load is placed or removed, (b) check the real sample
               rate, and (c) keep measuring the noise band and the drift.
   Thesis    : Developing a data-driven by-product tracking system to support
               consistent data collection and sharing (LHKK / HAMK)
   Section   : 4.6.2  Calibration and measurement behaviour
   Author    : Duxing Li, HAMK
   Date      : 2026-10-18
   Version   : v3   (replaces v2 for settling-time work; v2 stays valid)
   -----------------------------------------------------------------------------
   Changes from v2
     1. Every sample gets a time stamp in milliseconds (millis()), taken at the
        moment the HX711 reports that a conversion is ready.
     2. Fast mode ('f') now prints one line per sample as "ms,d" (time stamp,
        counts after zero). The settling time is calculated from the time
        stamps, not from "number of lines x 0.1 s", because the real rate is
        about 11 samples per second and not exactly 10.
     3. Each 100-sample block line ends with "sps=", the measured sample rate
        of that block.
     4. Slow mode, zeroing (z, mean of 20 samples) and the block statistics are
        otherwise unchanged.
   -----------------------------------------------------------------------------
   Hardware  : ESP32 dev module (Guition)
               M5Stack Unit Weight (HX711, 24-bit ADC)
               4 x M5Stack K121 half-bridge strain gauges (1.0 mV/V, 1000 ohm)
               Sensor Wiring Terminal (sockets labelled R / L)
               Bench platform: 2 x acacia board, 320 x 210 mm, gauges fixed
               with 3M 9448A tape (setup version 2)
   Layout    : R gauges at top-left and bottom-right, L gauges at top-right and
               bottom-left, each plugged into its matching R / L socket.
   Wiring    : DAT -> GPIO 22,  CLK -> GPIO 27,  5V,  GND
   Serial    : 115200 baud
   Commands  : z = set zero      f = fast mode ("ms,d" for every sample)
               s = slow mode (one line per second)
   -----------------------------------------------------------------------------
   Notes     : No external library is used. The HX711 protocol is implemented
               directly so that the behaviour can be described in the thesis.
               Channel A, gain 128, selected by sending 25 clock pulses.
               The time stamp can be wrong by up to 1 ms, because the code
               polls the DOUT pin once per millisecond.
   AI usage  : First draft written with AI assistance (Claude, 2026-10-18),
               then reviewed, compiled and tested by the author.
   ============================================================================= */

const int PIN_DAT = 22;
const int PIN_CLK = 27;

const int           STAT_N   = 100;    // samples per noise statistics block (about 9 s)
const int           ZERO_N   = 20;     // samples averaged when zeroing (about 2 s)
const unsigned long PRINT_MS = 1000;   // slow mode: one line per second

int32_t       zeroRef  = 0;
int32_t       lastRaw  = 0;
bool          fastMode = false;
unsigned long sampleMs = 0;            // time stamp of the latest sample

// noise statistics block
int32_t       sMin, sMax;
int64_t       sSum;
int           sCount = 0;
unsigned long sT0 = 0, sT1 = 0;        // time stamps of first and last sample in the block

// one-second window, used in slow mode
int32_t       wMin, wMax;
int64_t       wSum;
int           wCount    = 0;
unsigned long lastPrint = 0;

void resetStats() {
  sMin =  2147483647L;
  sMax = -2147483647L;
  sSum = 0;
  sCount = 0;
}

void resetWindow() {
  wMin =  2147483647L;
  wMax = -2147483647L;
  wSum = 0;
  wCount = 0;
}

int32_t hx711_read() {
  // Wait until DOUT goes low, which means a conversion is ready.
  unsigned long t0 = millis();
  while (digitalRead(PIN_DAT) == HIGH) {
    if (millis() - t0 > 500) return 0x7FFFFFFF;   // timeout marker
    delay(1);
  }
  sampleMs = millis();                             // time stamp of this sample

  uint32_t v = 0;
  noInterrupts();
  for (int i = 0; i < 24; i++) {
    digitalWrite(PIN_CLK, HIGH);
    delayMicroseconds(1);
    v = (v << 1) | (uint32_t)digitalRead(PIN_DAT);
    digitalWrite(PIN_CLK, LOW);
    delayMicroseconds(1);
  }
  // The 25th pulse selects channel A with gain 128 for the next reading.
  digitalWrite(PIN_CLK, HIGH);
  delayMicroseconds(1);
  digitalWrite(PIN_CLK, LOW);
  delayMicroseconds(1);
  interrupts();

  if (v & 0x800000UL) v |= 0xFF000000UL;   // sign extend 24 bit -> 32 bit
  return (int32_t)v;
}

void zeroNow() {
  int64_t sum = 0;
  int n = 0;
  for (int i = 0; i < ZERO_N; i++) {
    int32_t r = hx711_read();
    if (r == 0x7FFFFFFF) continue;         // skip a timed-out read
    sum += r;
    n++;
  }
  if (n == 0) {
    Serial.println("# zero FAILED - no data");
    return;
  }
  zeroRef = (int32_t)(sum / n);
  resetStats();
  resetWindow();
  lastPrint = millis();
  Serial.print("# zero set, reference = ");
  Serial.print(zeroRef);
  Serial.print("  (mean of ");
  Serial.print(n);
  Serial.println(" samples)");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_CLK, OUTPUT);
  pinMode(PIN_DAT, INPUT);
  digitalWrite(PIN_CLK, LOW);
  delay(500);
  Serial.println("# hx711_raw_reader_20261018_v3");
  Serial.println("# z = zero   f = fast (ms,d for every sample)   s = slow (one line per second)");
  resetStats();
  resetWindow();
  lastPrint = millis();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'z' || c == 'Z') {
      zeroNow();
    } else if (c == 'f' || c == 'F') {
      fastMode = true;
      Serial.println("# fast mode: ms,d for every sample");
    } else if (c == 's' || c == 'S') {
      fastMode = false;
      resetWindow();
      lastPrint = millis();
      Serial.println("# slow mode: one line per second");
    }
  }

  int32_t raw = hx711_read();

  if (raw == 0x7FFFFFFF) {
    Serial.println("# NO DATA - check DAT / CLK / 5V / GND");
    delay(500);
    return;
  }
  if (raw == 0x7FFFFF || raw == -8388608L) {
    Serial.println("# SATURATED - bridge not balanced, check the four plugs");
    delay(500);
    return;
  }

  lastRaw = raw;
  int32_t d = raw - zeroRef;

  // Noise statistics always use every sample.
  if (d < sMin) sMin = d;
  if (d > sMax) sMax = d;
  sSum += d;
  sCount++;
  if (sCount == 1) sT0 = sampleMs;
  sT1 = sampleMs;

  if (fastMode) {
    Serial.print(sampleMs);                // time stamp in ms
    Serial.print(',');
    Serial.println(d);                     // counts after zero
  } else {
    if (d < wMin) wMin = d;
    if (d > wMax) wMax = d;
    wSum += d;
    wCount++;
    if (millis() - lastPrint >= PRINT_MS && wCount > 0) {
      int32_t avg = (int32_t)(wSum / wCount);
      char buf[64];
      snprintf(buf, sizeof(buf), "avg=%+8ld  span=%5ld  n=%d",
               (long)avg, (long)(wMax - wMin), wCount);
      Serial.println(buf);
      resetWindow();
      lastPrint = millis();
    }
  }

  if (sCount >= STAT_N) {
    int32_t span = sMax - sMin;
    int32_t mean = (int32_t)(sSum / sCount);
    float   sps  = (sT1 > sT0) ? (sCount - 1) * 1000.0f / (float)(sT1 - sT0) : 0.0f;
    Serial.print("# n=");      Serial.print(sCount);
    Serial.print("  min=");    Serial.print(sMin);
    Serial.print("  max=");    Serial.print(sMax);
    Serial.print("  span=");   Serial.print(span);
    Serial.print("  mean=");   Serial.print(mean);
    Serial.print("  raw=");    Serial.print(raw);
    Serial.print("  sps=");    Serial.println(sps, 1);
    resetStats();
  }
}
