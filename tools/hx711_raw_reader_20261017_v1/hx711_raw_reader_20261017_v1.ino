/* =============================================================================
   File      : hx711_raw_reader_20261017_v1.ino
   Purpose   : Read raw HX711 counts from the bench weighing platform.
               Used to (a) verify that all four strain gauges act in the same
               direction, (b) measure the noise band, and (c) measure the
               settling time.
   Thesis    : Developing a data-driven by-product tracking system to support
               consistent data collection and sharing (LHKK / HAMK)
   Section   : 4.6.2  Calibration and measurement behaviour
   Author    : Duxing Li, HAMK
   Date      : 2026-10-17
   Version   : v1
   -----------------------------------------------------------------------------
   Hardware  : ESP32 dev module (Guition)
               M5Stack Unit Weight (HX711, 24-bit ADC)
               4 x M5Stack K121 half-bridge strain gauges (1.0 mV/V, 1000 ohm)
               Sensor Wiring Terminal (sockets labelled R / L)
               Bench platform: 2 x acacia board, 320 x 210 x 15 mm
   Layout    : R gauges on one diagonal, L gauges on the other diagonal,
               each plugged into its matching R / L socket.
   Wiring    : DAT -> GPIO 22,  CLK -> GPIO 27,  5V,  GND
   Serial    : 115200 baud
   -----------------------------------------------------------------------------
   Notes     : No external library is used. The HX711 protocol is implemented
               directly so that the behaviour can be described in the thesis.
               Channel A, gain 128, selected by sending 25 clock pulses.
               Nominal output rate 10 SPS, so one printed line = 0.1 s.
               Send 'z' over the serial port to set the zero reference.
   AI usage  : First draft written with AI assistance (Claude, 2026-10-17),
               then reviewed, compiled and tested by the author.
   ============================================================================= */

const int PIN_DAT = 22;
const int PIN_CLK = 27;

const int STAT_N = 100;        // 100 samples = about 10 s at 10 SPS

int32_t zeroRef = 0;
int32_t lastRaw = 0;

int32_t sMin, sMax;
int64_t sSum;
int     sCount = 0;

void resetStats() {
  sMin =  2147483647L;
  sMax = -2147483647L;
  sSum = 0;
  sCount = 0;
}

int32_t hx711_read() {
  // Wait until DOUT goes low, which means a conversion is ready.
  unsigned long t0 = millis();
  while (digitalRead(PIN_DAT) == HIGH) {
    if (millis() - t0 > 500) return 0x7FFFFFFF;   // timeout marker
    delay(1);
  }

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

void setup() {
  Serial.begin(115200);
  pinMode(PIN_CLK, OUTPUT);
  pinMode(PIN_DAT, INPUT);
  digitalWrite(PIN_CLK, LOW);
  delay(500);
  Serial.println("# hx711_raw_reader_20261017_v1");
  Serial.println("# send 'z' to set the zero reference");
  resetStats();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'z' || c == 'Z') {
      zeroRef = lastRaw;
      resetStats();
      Serial.println("# zero set");
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
  Serial.println(d);                 // one number per line

  if (d < sMin) sMin = d;
  if (d > sMax) sMax = d;
  sSum += d;
  sCount++;

  if (sCount >= STAT_N) {
    int32_t span = sMax - sMin;
    int32_t mean = (int32_t)(sSum / sCount);
    Serial.print("# n=");      Serial.print(sCount);
    Serial.print("  min=");    Serial.print(sMin);
    Serial.print("  max=");    Serial.print(sMax);
    Serial.print("  span=");   Serial.print(span);
    Serial.print("  mean=");   Serial.print(mean);
    Serial.print("  raw=");    Serial.println(raw);
    resetStats();
  }
}
