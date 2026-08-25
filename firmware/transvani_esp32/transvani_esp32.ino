#include <Wire.h>
#include <driver/i2s.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// ============================================================
//                    TRANSVANI PROTOTYPE
// ============================================================
//
// Sensors:
//   ADXL345  -> vibration
//   DHT11    -> temperature + humidity
//   INMP441  -> acoustic monitoring
//
// Outputs:
//   RGB LED  -> status
//   Buzzer   -> abnormal-event alert
//   BLE      -> advertises health score + category (no pairing)
//   Serial   -> JSON status line every loop (for collector.py / dashboard)
//
// ESP32
// ============================================================


// ============================================================
//                     PIN DEFINITIONS
// ============================================================

// ---------------- ADXL345 ----------------
#define SDA_PIN       21
#define SCL_PIN       22
#define ADXL_ADDR     0x53

// ---------------- DHT11 ----------------
#define DHT_PIN       4

// ---------------- RGB LED ----------------
#define BLUE_PIN      25
#define GREEN_PIN     26
#define RED_PIN       27

// ---------------- BUZZER ----------------
#define BUZZER_PIN    23


// ============================================================
//                    INMP441 I2S PINS
// ============================================================

#define I2S_PORT      I2S_NUM_0

#define I2S_BCLK      18
#define I2S_WS        19
#define I2S_SD        33

// INMP441 L/R -> GND
// This selects the LEFT channel.


// ============================================================
//                MICROPHONE PARAMETERS
// ============================================================

#define SAMPLE_RATE       16000
#define SAMPLE_COUNT      1024

int32_t audioSamples[SAMPLE_COUNT];


// ============================================================
//         TRANSVANI IDENTITY / HEALTH ENGINE PARAMETERS
// ============================================================

#define TRANSFORMER_ID          1        // change per deployed unit
#define BASELINE_SAMPLES         20        // loop cycles used to learn the baseline (~40s @ 2s/loop)
#define HISTORY_LENGTH            5        // rolling window used for persistence/trend checks

// --------------------------------------------------------------
// Static identity / location metadata -- NOT sensor readings.
// These are fixed per deployed unit; edit them for your actual
// site. Defaults below match the frontend's mockData.js exactly
// so the JSON output is a drop-in match for testing.
// --------------------------------------------------------------
#define TRANSFORMER_TX_ID     "TX-RUR-0941"
#define TRANSFORMER_SUBSTATION "Vellore 110/11kV Sub-02"
#define TRANSFORMER_DISTRICT   "Vellore"

// Deviation is flagged as "anomalous" once it crosses this percentage.
#define ANOMALY_DEVIATION_THRESHOLD   25.0

// Category codes broadcast over BLE / used internally (single byte).
#define CATEGORY_NORMAL          0
#define CATEGORY_MONITOR         1
#define CATEGORY_INSPECT_SOON    2
#define CATEGORY_CRITICAL        3
#define CATEGORY_BASELINING      4

// ADXL345 full-resolution sensitivity (typical datasheet value, ~3.9 mg/LSB).
// Used to express vibration as an approximate "g" magnitude for the JSON
// output. This is the magnitude of the RAW accelerometer vector (so at
// rest it reads ~1.0g from gravity alone) -- NOT a true time-averaged RMS,
// since we only take one accelerometer sample per loop. Recalibrate
// against your actual unit if you need this to be metrologically accurate.
#define ADXL345_LSB_TO_G       0.0039

// Display-only scaling for harmonic power -> a small human-readable
// intensity number (NOT a physical unit, NOT dB). Calibrated against
// real hardware readings (raw Goertzel power observed in the 0.7-7
// range during initial bench testing -> scale chosen so that range
// lands around 30-300 on the display, well under the 9999 clamp).
// Re-tune if your mic/enclosure/gain setup gives very different
// raw levels -- watch the "MIC | ___ Hz Power" Serial lines and
// adjust this until scaledHarmonic() output looks sensible.
#define HARMONIC_DISPLAY_SCALE   40.0
#define HARMONIC_DISPLAY_MAX     9999

// --------------------------------------------------------------
// NOTE: There is no current sensor wired in this prototype. The
// report/BOM treats it as optional hardware (e.g. a non-invasive
// CT clamp / ACS712) that hasn't been added yet. Until it exists,
// loadCurrent is honestly reported as "N/A" in the JSON output --
// set HAS_CURRENT_SENSOR to 1 and implement readLoadCurrentAmps()
// once the sensor is wired.
// --------------------------------------------------------------
#define HAS_CURRENT_SENSOR   0


// ============================================================
//                    RGB CONTROL
// ============================================================

void setRGB(bool red, bool green, bool blue) {

  digitalWrite(RED_PIN, red ? HIGH : LOW);
  digitalWrite(GREEN_PIN, green ? HIGH : LOW);
  digitalWrite(BLUE_PIN, blue ? HIGH : LOW);
}


// ============================================================
//                 ADXL345 REGISTER WRITE
// ============================================================

void adxlWrite(uint8_t reg, uint8_t value) {

  Wire.beginTransmission(ADXL_ADDR);

  Wire.write(reg);
  Wire.write(value);

  Wire.endTransmission();
}


// ============================================================
//                  ADXL345 REGISTER READ
// ============================================================

uint8_t adxlRead(uint8_t reg) {

  Wire.beginTransmission(ADXL_ADDR);

  Wire.write(reg);

  Wire.endTransmission(false);

  Wire.requestFrom(ADXL_ADDR, (uint8_t)1);

  if (Wire.available()) {
    return Wire.read();
  }

  return 0;
}


// ============================================================
//                    ADXL345 READ
// ============================================================

bool readADXL(int16_t &x, int16_t &y, int16_t &z) {

  Wire.beginTransmission(ADXL_ADDR);

  Wire.write(0x32);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(ADXL_ADDR, (uint8_t)6) != 6) {
    return false;
  }

  uint8_t data[6];

  for (int i = 0; i < 6; i++) {
    data[i] = Wire.read();
  }

  x = (int16_t)((data[1] << 8) | data[0]);
  y = (int16_t)((data[3] << 8) | data[2]);
  z = (int16_t)((data[5] << 8) | data[4]);

  return true;
}


// ============================================================
//                       DHT11 READ
// ============================================================

bool readDHT11(float &temperature, float &humidity) {

  uint8_t data[5] = {0, 0, 0, 0, 0};

  pinMode(DHT_PIN, OUTPUT);

  digitalWrite(DHT_PIN, LOW);

  delay(20);

  digitalWrite(DHT_PIN, HIGH);

  delayMicroseconds(40);

  pinMode(DHT_PIN, INPUT_PULLUP);


  unsigned long timeout = micros();

  while (digitalRead(DHT_PIN) == HIGH) {

    if (micros() - timeout > 100) {
      return false;
    }
  }


  timeout = micros();

  while (digitalRead(DHT_PIN) == LOW) {

    if (micros() - timeout > 100) {
      return false;
    }
  }


  timeout = micros();

  while (digitalRead(DHT_PIN) == HIGH) {

    if (micros() - timeout > 100) {
      return false;
    }
  }


  for (int i = 0; i < 40; i++) {

    timeout = micros();

    while (digitalRead(DHT_PIN) == LOW) {

      if (micros() - timeout > 100) {
        return false;
      }
    }

    unsigned long start = micros();

    timeout = micros();

    while (digitalRead(DHT_PIN) == HIGH) {

      if (micros() - timeout > 100) {
        return false;
      }
    }

    unsigned long duration = micros() - start;

    data[i / 8] <<= 1;

    if (duration > 40) {
      data[i / 8] |= 1;
    }
  }


  // Checksum

  if ((uint8_t)(
        data[0] +
        data[1] +
        data[2] +
        data[3]
      ) != data[4]) {

    return false;
  }


  humidity =
      data[0] +
      data[1] * 0.1;


  temperature =
      data[2] +
      data[3] * 0.1;


  return true;
}


// ============================================================
//                   I2S MICROPHONE SETUP
// ============================================================

void setupI2SMicrophone() {

  i2s_config_t i2s_config = {

    .mode =
        (i2s_mode_t)(
          I2S_MODE_MASTER |
          I2S_MODE_RX
        ),

    .sample_rate = SAMPLE_RATE,

    .bits_per_sample =
        I2S_BITS_PER_SAMPLE_32BIT,

    .channel_format =
        I2S_CHANNEL_FMT_ONLY_LEFT,

    .communication_format =
        I2S_COMM_FORMAT_I2S,

    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count = 8,

    .dma_buf_len = 256,

    .use_apll = false,

    .tx_desc_auto_clear = false,

    .fixed_mclk = 0
  };


  i2s_pin_config_t pin_config = {

    .bck_io_num = I2S_BCLK,

    .ws_io_num = I2S_WS,

    .data_out_num = I2S_PIN_NO_CHANGE,

    .data_in_num = I2S_SD
  };


  esp_err_t result;


  result = i2s_driver_install(
      I2S_PORT,
      &i2s_config,
      0,
      NULL
  );


  if (result != ESP_OK) {

    Serial.println(
      "ERROR: I2S driver installation failed!"
    );

    return;
  }


  result = i2s_set_pin(
      I2S_PORT,
      &pin_config
  );


  if (result != ESP_OK) {

    Serial.println(
      "ERROR: I2S pin configuration failed!"
    );

    return;
  }


  i2s_zero_dma_buffer(I2S_PORT);


  Serial.println(
    "INMP441: I2S INITIALIZED"
  );
}


// ============================================================
//                READ MICROPHONE SAMPLES
// ============================================================

bool readMicrophone() {

  size_t bytesRead = 0;

  esp_err_t result;


  result = i2s_read(
      I2S_PORT,
      (void *)audioSamples,
      sizeof(audioSamples),
      &bytesRead,
      pdMS_TO_TICKS(500)
  );


  if (result != ESP_OK) {

    return false;
  }


  if (bytesRead != sizeof(audioSamples)) {

    return false;
  }


  // Convert 32-bit I2S samples to useful signed values.

  for (int i = 0; i < SAMPLE_COUNT; i++) {

    audioSamples[i] =
        audioSamples[i] >> 14;
  }


  return true;
}


// ============================================================
//                 CALCULATE RMS AUDIO LEVEL
// ============================================================

float calculateRMS() {

  double sumSquares = 0.0;


  for (int i = 0; i < SAMPLE_COUNT; i++) {

    double sample =
        (double)audioSamples[i];

    sumSquares +=
        sample * sample;
  }


  double mean =
      sumSquares / SAMPLE_COUNT;


  return sqrt(mean);
}


// ============================================================
//                 CALCULATE PEAK AUDIO LEVEL
// ============================================================

int32_t calculatePeak() {

  int32_t peak = 0;


  for (int i = 0; i < SAMPLE_COUNT; i++) {

    int32_t value =
        abs(audioSamples[i]);


    if (value > peak) {
      peak = value;
    }
  }


  return peak;
}


// ============================================================
//             GOERTZEL POWER CALCULATION
// ============================================================

float goertzelPower(float targetFrequency) {

  double realPart = 0.0;
  double imagPart = 0.0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {

    // Normalize microphone sample
    double sample =
        (double)audioSamples[i] / 131072.0;

    double angle =
        2.0 * PI *
        targetFrequency *
        i /
        SAMPLE_RATE;

    realPart += sample * cos(angle);
    imagPart -= sample * sin(angle);
  }

  double power =
      (realPart * realPart) +
      (imagPart * imagPart);

  return (float)power;
}


// ============================================================
//              FIND APPROXIMATE DOMINANT FREQUENCY
// ============================================================

float findDominantFrequency() {

  float bestFrequency = 0;

  float bestPower = 0;


  // Search between 50 Hz and 500 Hz.

  for (
      float frequency = 50;
      frequency <= 500;
      frequency += 10
  ) {

    float power =
        goertzelPower(frequency);


    if (power > bestPower) {

      bestPower = power;

      bestFrequency = frequency;
    }
  }


  return bestFrequency;
}


// ============================================================
//                     MICROPHONE ANALYSIS
// ============================================================
//
// Now also extracts 50 Hz and 300 Hz harmonic power (in addition
// to the original 100 Hz / 200 Hz) so the JSON output can report
// a 4-point harmonic spectrum, matching the dashboard's schema.
// ============================================================

bool analyzeMicrophone(
    float &rms,
    int32_t &peak,
    float &dominantFrequency,
    float &power50,
    float &power100,
    float &power200,
    float &power300
) {

  if (!readMicrophone()) {

    return false;
  }


  rms =
      calculateRMS();


  peak =
      calculatePeak();


  power50 =
      goertzelPower(50.0);


  power100 =
      goertzelPower(100.0);


  power200 =
      goertzelPower(200.0);


  power300 =
      goertzelPower(300.0);


  dominantFrequency =
      findDominantFrequency();


  return true;
}


// ============================================================
//         BASELINE + HEALTH SCORE ENGINE
// ============================================================
//
// Phase 1 (BASELINING): the first BASELINE_SAMPLES loop cycles
// are spent averaging each feature to learn what "normal" looks
// like for THIS transformer. No alerts are raised in this phase.
//
// Phase 2 (MONITORING): every reading is compared against the
// learned baseline. A single deviating reading nudges the health
// score down but does NOT escalate the category -- only a
// persistent (repeated) or worsening pattern does that.
// ============================================================

enum SystemState {
  BASELINING,
  MONITORING
};

SystemState systemState = BASELINING;

int baselineSampleCount = 0;

// Accumulators used while learning the baseline.
float baseVibrationAccum  = 0;
float baseRMSAccum        = 0;
float basePower50Accum    = 0;
float basePower100Accum   = 0;
float basePower200Accum   = 0;
float basePower300Accum   = 0;

// The learned baseline (valid once systemState == MONITORING).
float baselineVibration = 0;
float baselineRMS       = 0;
float baselinePower50   = 0;
float baselinePower100  = 0;
float baselinePower200  = 0;
float baselinePower300  = 0;

// Rolling history of the combined per-reading deviation score.
// Index HISTORY_LENGTH-1 is always the most recent reading.
float deviationHistory[HISTORY_LENGTH] = {0, 0, 0, 0, 0};

// Reference orientation used to compute the vibration "movement index"
// that anomaly detection runs on (delta from rest position).
int16_t refX = 0, refY = 0, refZ = 0;
bool referenceSet = false;

// Last known-good DHT11 reading, so a single failed read doesn't
// blank out the JSON output.
float lastGoodTemperature = 0;
float lastGoodHumidity    = 0;


// ------------------------------------------------------------
// Percentage deviation of a live value from its learned baseline.
// Guards against divide-by-zero when the baseline is ~0.
// ------------------------------------------------------------

float percentDeviation(float current, float baseline) {

  float safeBaseline = baseline;

  if (safeBaseline < 1.0) {
    safeBaseline = 1.0;
  }

  float deviation =
      fabs(current - safeBaseline) / safeBaseline * 100.0;

  return deviation;
}


// ------------------------------------------------------------
// Push a new deviation reading into the rolling history buffer,
// dropping the oldest one.
// ------------------------------------------------------------

void pushDeviationHistory(float value) {

  for (int i = 0; i < HISTORY_LENGTH - 1; i++) {
    deviationHistory[i] = deviationHistory[i + 1];
  }

  deviationHistory[HISTORY_LENGTH - 1] = value;
}


// ------------------------------------------------------------
// Count how many entries in the rolling window are flagged as
// anomalous (i.e. above the deviation threshold).
// ------------------------------------------------------------

int countPersistentAnomalies() {

  int count = 0;

  for (int i = 0; i < HISTORY_LENGTH; i++) {

    if (deviationHistory[i] >= ANOMALY_DEVIATION_THRESHOLD) {
      count++;
    }
  }

  return count;
}


// ------------------------------------------------------------
// Trend check: is the deviation at the end of the window clearly
// worse than at the start of the window?
// ------------------------------------------------------------

bool isTrendWorsening() {

  float oldest = deviationHistory[0];
  float newest = deviationHistory[HISTORY_LENGTH - 1];

  return (newest > oldest * 1.2) && (newest >= ANOMALY_DEVIATION_THRESHOLD);
}


// ------------------------------------------------------------
// Human-readable label for a category code (Serial debug logs).
// ------------------------------------------------------------

const char* categoryName(uint8_t code) {

  switch (code) {
    case CATEGORY_NORMAL:       return "NORMAL";
    case CATEGORY_MONITOR:      return "MONITOR";
    case CATEGORY_INSPECT_SOON: return "INSPECT SOON";
    case CATEGORY_CRITICAL:     return "CRITICAL";
    case CATEGORY_BASELINING:   return "BASELINING";
    default:                    return "UNKNOWN";
  }
}


// ------------------------------------------------------------
// Title-case status label, matching the JSON schema's "status"
// field exactly (e.g. "Critical", not "CRITICAL").
// ------------------------------------------------------------

String jsonStatusName(uint8_t code) {

  switch (code) {
    case CATEGORY_NORMAL:       return "Normal";
    case CATEGORY_MONITOR:      return "Monitor";
    case CATEGORY_INSPECT_SOON: return "Inspect Soon";
    case CATEGORY_CRITICAL:     return "Critical";
    case CATEGORY_BASELINING:   return "Baselining";
    default:                    return "Unknown";
  }
}


// ------------------------------------------------------------
// Short human-readable explanation matching the category, in the
// same tone as the frontend's mockData.js. Templated, not sensor
// data -- feel free to reword these to match your team's voice.
// ------------------------------------------------------------

String jsonExplanation(uint8_t code) {

  switch (code) {

    case CATEGORY_NORMAL:
      return "Acoustic harmonics align with healthy baseline. Vibration and thermal context stable.";

    case CATEGORY_MONITOR:
      return "Isolated acoustic spike detected. Persistence filter active; no fault declared.";

    case CATEGORY_INSPECT_SOON:
      return "Repeated harmonic deviation detected across recent readings. Inspection priority raised.";

    case CATEGORY_CRITICAL:
      return "Persistent harmonic and structural vibration escalation. Immediate dispatch required.";

    case CATEGORY_BASELINING:
      return "Learning this transformer's normal acoustic baseline. No status yet.";

    default:
      return "";
  }
}


// ------------------------------------------------------------
// Scale a raw Goertzel power value into a small human-readable
// integer for the JSON "harmonics" block. Display-only -- see
// HARMONIC_DISPLAY_SCALE comment near the top of this file.
// ------------------------------------------------------------

int scaledHarmonic(float rawPower) {

  long scaled = (long)(rawPower * HARMONIC_DISPLAY_SCALE);

  if (scaled < 0) {
    scaled = 0;
  }

  if (scaled > HARMONIC_DISPLAY_MAX) {
    scaled = HARMONIC_DISPLAY_MAX;
  }

  return (int)scaled;
}


// ------------------------------------------------------------
// Load current: NOT a real sensor reading (see HAS_CURRENT_SENSOR
// note near the top of this file). Returns "N/A" until a real
// current sensor is wired in and this function is replaced with
// an actual ADC read + calibration.
// ------------------------------------------------------------

String readLoadCurrentString() {

#if HAS_CURRENT_SENSOR

  // TODO: replace with a real analog read + calibration once a
  // current sensor (e.g. ACS712 / non-invasive CT clamp) is wired.
  float amps = 0.0;

  return String(amps, 1) + " A";

#else

  return "N/A";

#endif
}


// ============================================================
//                    BLE BROADCAST
// ============================================================
//
// Advertising-only BLE: no pairing, no GATT connection required.
// Any nearby scanner (a phone, a laptop, nRF Connect) can read
// the status straight out of the advertisement packet.
//
// Manufacturer data payload (6 bytes) -- kept intentionally small,
// this is just enough for a passing scanner to relay:
//   [0-1] Company ID placeholder (0xFFFF = unallocated/test range)
//   [2-3] Transformer ID (uint16, little-endian)
//   [4]   Health score (0-100)
//   [5]   Category code (see CATEGORY_* constants)
//
// The full feature set (harmonics, temp, humidity, etc.) is sent
// over Serial as JSON instead -- BLE advertisement payloads are
// too small (~20-25 usable bytes) to carry all of that.
// ============================================================

BLEAdvertising *pAdvertising;


void setupBLE() {

  BLEDevice::init("TRANSVANI-01");

  pAdvertising = BLEDevice::getAdvertising();

  pAdvertising->setScanResponse(true);

  Serial.println("BLE: ADVERTISER INITIALIZED");
}


void broadcastHealthStatus(uint8_t healthScore, uint8_t categoryCode) {

  BLEAdvertisementData advData;

  String payload;

  payload += (char)0xFF;                              // Company ID low byte
  payload += (char)0xFF;                              // Company ID high byte
  payload += (char)(TRANSFORMER_ID & 0xFF);            // Transformer ID low byte
  payload += (char)((TRANSFORMER_ID >> 8) & 0xFF);     // Transformer ID high byte
  payload += (char)healthScore;                        // 0-100
  payload += (char)categoryCode;                        // CATEGORY_*

  advData.setManufacturerData(payload);
  advData.setName("TRANSVANI-01");

  pAdvertising->stop();
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start();
}


// ============================================================
//                    JSON STATUS OUTPUT
// ============================================================
//
// Printed once per loop over Serial, matching the frontend's
// mockData.js shape exactly:
//
// {"txId":"TX-RUR-0941","substation":"Vellore 110/11kV Sub-02",
//  "district":"Vellore","healthScore":41,"status":"Critical",
//  "harmonicDev":68.2,"vibRMS":1.15,"temp":33.8,"humidity":58.0,
//  "loadCurrent":"4.9 A","explanation":"...",
//  "harmonics":{"50Hz":88,"100Hz":110,"200Hz":72,"300Hz":55},
//  "baseline":{"50Hz":80,"100Hz":94,"200Hz":28,"300Hz":14}}
//
// (all on one line -- wrapped above only for readability here)
// ============================================================

void printJSONStatus(
    uint8_t  healthScore,
    uint8_t  categoryCode,
    float    harmonicDev,
    float    vibRMS,
    float    temperature,
    float    humidity,
    String   loadCurrent,
    float    power50,
    float    power100,
    float    power200,
    float    power300,
    float    baseline50,
    float    baseline100,
    float    baseline200,
    float    baseline300
) {

  String json = "{";

  json += "\"txId\":\"";
  json += TRANSFORMER_TX_ID;
  json += "\",";

  json += "\"substation\":\"";
  json += TRANSFORMER_SUBSTATION;
  json += "\",";

  json += "\"district\":\"";
  json += TRANSFORMER_DISTRICT;
  json += "\",";

  json += "\"healthScore\":";
  json += healthScore;
  json += ",";

  json += "\"status\":\"";
  json += jsonStatusName(categoryCode);
  json += "\",";

  json += "\"harmonicDev\":";
  json += String(harmonicDev, 1);
  json += ",";

  json += "\"vibRMS\":";
  json += String(vibRMS, 2);
  json += ",";

  json += "\"temp\":";
  json += String(temperature, 1);
  json += ",";

  json += "\"humidity\":";
  json += String(humidity, 1);
  json += ",";

  json += "\"loadCurrent\":\"";
  json += loadCurrent;
  json += "\",";

  json += "\"explanation\":\"";
  json += jsonExplanation(categoryCode);
  json += "\",";

  json += "\"harmonics\":{";
  json += "\"50Hz\":";
  json += scaledHarmonic(power50);
  json += ",\"100Hz\":";
  json += scaledHarmonic(power100);
  json += ",\"200Hz\":";
  json += scaledHarmonic(power200);
  json += ",\"300Hz\":";
  json += scaledHarmonic(power300);
  json += "},";

  json += "\"baseline\":{";
  json += "\"50Hz\":";
  json += scaledHarmonic(baseline50);
  json += ",\"100Hz\":";
  json += scaledHarmonic(baseline100);
  json += ",\"200Hz\":";
  json += scaledHarmonic(baseline200);
  json += ",\"300Hz\":";
  json += scaledHarmonic(baseline300);
  json += "}";   // close baseline

  json += "}";   // close root object

  Serial.println(json);
}


// ============================================================
//                        SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(1500);


  Serial.println();
  Serial.println(
    "========================================"
  );

  Serial.println(
    "       TRANSVANI EDGE NODE V3"
  );

  Serial.println(
    "========================================"
  );


  // ========================================================
  // RGB
  // ========================================================

  pinMode(RED_PIN, OUTPUT);

  pinMode(GREEN_PIN, OUTPUT);

  pinMode(BLUE_PIN, OUTPUT);


  // Start WHITE (baselining)

  setRGB(
    true,
    true,
    true
  );


  // ========================================================
  // BUZZER
  // ========================================================

  pinMode(
    BUZZER_PIN,
    OUTPUT
  );

  digitalWrite(
    BUZZER_PIN,
    LOW
  );


  // ========================================================
  // I2C / ADXL345
  // ========================================================

  Wire.begin(
    SDA_PIN,
    SCL_PIN
  );


  uint8_t deviceID =
      adxlRead(0x00);


  Serial.print(
    "ADXL345 ID: 0x"
  );

  Serial.println(
    deviceID,
    HEX
  );


  if (deviceID == 0xE5) {

    Serial.println(
      "ADXL345: CONNECTED"
    );


    // Measurement mode

    adxlWrite(
      0x2D,
      0x08
    );


    // Full resolution ±2g

    adxlWrite(
      0x31,
      0x08
    );

  }

  else {

    Serial.println(
      "ADXL345: NOT DETECTED"
    );
  }


  // ========================================================
  // INMP441
  // ========================================================

  setupI2SMicrophone();


  // ========================================================
  // BLE
  // ========================================================

  setupBLE();


  // ========================================================
  // READY
  // ========================================================

  Serial.println(
    "----------------------------------------"
  );

  Serial.println(
    "TRANSVANI SYSTEM READY"
  );

  Serial.println(
    "Monitoring:"
  );

  Serial.println(
    "  - Vibration"
  );

  Serial.println(
    "  - Temperature"
  );

  Serial.println(
    "  - Humidity"
  );

  Serial.println(
    "  - Acoustic signal (50/100/200/300 Hz)"
  );

  Serial.print(
    "Learning baseline over "
  );

  Serial.print(BASELINE_SAMPLES);

  Serial.println(
    " readings..."
  );

  Serial.println(
    "----------------------------------------"
  );


  delay(1000);
}


// ============================================================
//                         MAIN LOOP
// ============================================================

void loop() {

  // ========================================================
  // ADXL345
  // ========================================================

  int16_t x = 0, y = 0, z = 0;

  bool accelOK =
      readADXL(
        x,
        y,
        z
      );

  float movementValue = 0;   // used for anomaly detection (delta from rest reference)
  float vibRMS_g       = 0;   // used for JSON display (raw acceleration magnitude, in g)

  if (accelOK) {

    Serial.print(
      "ACC | X: "
    );

    Serial.print(x);

    Serial.print(
      " Y: "
    );

    Serial.print(y);

    Serial.print(
      " Z: "
    );

    Serial.println(z);


    if (!referenceSet) {

      refX = x;

      refY = y;

      refZ = z;

      referenceSet = true;
    }


    movementValue =

        abs(x - refX)

        +

        abs(y - refY)

        +

        abs(z - refZ);


    Serial.print(
      "Vibration index: "
    );

    Serial.println(
      movementValue
    );


    // Raw acceleration magnitude in g (includes gravity -- reads
    // ~1.0g at rest). This is what gets reported as "vibRMS" in
    // the JSON output; it is a single-sample magnitude, not a
    // true time-averaged RMS.

    vibRMS_g =
        sqrt(
          ((float)x * (float)x) +
          ((float)y * (float)y) +
          ((float)z * (float)z)
        ) * ADXL345_LSB_TO_G;
  }

  else {

    Serial.println(
      "ACC | READ FAILED"
    );
  }


  // ========================================================
  // DHT11
  // ========================================================

  float temperature = 0;

  float humidity = 0;


  bool dhtOK =
      readDHT11(
        temperature,
        humidity
      );


  if (dhtOK) {

    lastGoodTemperature = temperature;

    lastGoodHumidity = humidity;

    Serial.print(
      "DHT11 | Temperature: "
    );

    Serial.print(
      temperature,
      2
    );

    Serial.print(
      " C | Humidity: "
    );

    Serial.print(
      humidity,
      2
    );

    Serial.println(
      " %"
    );
  }

  else {

    Serial.println(
      "DHT11 | READ FAILED (using last known-good values)"
    );
  }


  // ========================================================
  // MICROPHONE
  // ========================================================

  float rms = 0;

  int32_t peak = 0;

  float dominantFrequency = 0;

  float power50 = 0;

  float power100 = 0;

  float power200 = 0;

  float power300 = 0;


  bool microphoneOK =
      analyzeMicrophone(
        rms,
        peak,
        dominantFrequency,
        power50,
        power100,
        power200,
        power300
      );


  if (microphoneOK) {

    Serial.print(
      "MIC | RMS: "
    );

    Serial.println(
      rms,
      2
    );


    Serial.print(
      "MIC | Peak: "
    );

    Serial.println(
      peak
    );


    Serial.print(
      "MIC | Dominant Frequency: "
    );

    Serial.print(
      dominantFrequency,
      1
    );

    Serial.println(
      " Hz"
    );


    Serial.print(
      "MIC | 50 Hz Power: "
    );

    Serial.println(
      power50,
      4
    );


    Serial.print(
      "MIC | 100 Hz Power: "
    );

    Serial.println(
      power100,
      4
    );


    Serial.print(
      "MIC | 200 Hz Power: "
    );

    Serial.println(
      power200,
      4
    );


    Serial.print(
      "MIC | 300 Hz Power: "
    );

    Serial.println(
      power300,
      4
    );
  }

  else {

    Serial.println(
      "MIC | READ FAILED"
    );
  }


  // ========================================================
  // BASELINE / HEALTH SCORE ENGINE
  // ========================================================

  uint8_t healthScore   = 100;
  uint8_t categoryCode  = CATEGORY_BASELINING;
  float   harmonicDev   = 0;    // average % deviation across 50/100/200/300 Hz

  if (systemState == BASELINING) {

    // ------------------------------------------------------
    // Accumulate readings to learn what "normal" looks like.
    // ------------------------------------------------------

    baseVibrationAccum += movementValue;
    baseRMSAccum        += rms;
    basePower50Accum     += power50;
    basePower100Accum    += power100;
    basePower200Accum    += power200;
    basePower300Accum    += power300;

    baselineSampleCount++;

    Serial.print(
      "BASELINING | sample "
    );

    Serial.print(baselineSampleCount);

    Serial.print(
      " / "
    );

    Serial.println(BASELINE_SAMPLES);


    if (baselineSampleCount >= BASELINE_SAMPLES) {

      baselineVibration = baseVibrationAccum / BASELINE_SAMPLES;
      baselineRMS        = baseRMSAccum / BASELINE_SAMPLES;
      baselinePower50     = basePower50Accum / BASELINE_SAMPLES;
      baselinePower100    = basePower100Accum / BASELINE_SAMPLES;
      baselinePower200    = basePower200Accum / BASELINE_SAMPLES;
      baselinePower300    = basePower300Accum / BASELINE_SAMPLES;

      systemState = MONITORING;

      Serial.println(
        "----------------------------------------"
      );

      Serial.println(
        "BASELINE ESTABLISHED:"
      );

      Serial.print(
        "  Vibration:   "
      );
      Serial.println(baselineVibration);

      Serial.print(
        "  RMS:         "
      );
      Serial.println(baselineRMS);

      Serial.print(
        "  50Hz power:  "
      );
      Serial.println(baselinePower50, 4);

      Serial.print(
        "  100Hz power: "
      );
      Serial.println(baselinePower100, 4);

      Serial.print(
        "  200Hz power: "
      );
      Serial.println(baselinePower200, 4);

      Serial.print(
        "  300Hz power: "
      );
      Serial.println(baselinePower300, 4);

      Serial.println(
        "SWITCHING TO MONITORING MODE"
      );

      Serial.println(
        "----------------------------------------"
      );
    }

    healthScore  = 100;
    categoryCode = CATEGORY_BASELINING;
    harmonicDev  = 0;
  }

  else {

    // ------------------------------------------------------
    // MONITORING: compare this reading against the baseline.
    // ------------------------------------------------------

    float vibDeviation  = percentDeviation(movementValue, baselineVibration);
    float rmsDeviation  = percentDeviation(rms, baselineRMS);
    float p50Deviation  = percentDeviation(power50, baselinePower50);
    float p100Deviation = percentDeviation(power100, baselinePower100);
    float p200Deviation = percentDeviation(power200, baselinePower200);
    float p300Deviation = percentDeviation(power300, baselinePower300);

    // Average deviation across all four harmonics -- this is the
    // number reported as "harmonicDev" in the JSON output.
    harmonicDev =
        (p50Deviation + p100Deviation + p200Deviation + p300Deviation) / 4.0;

    // Weighted combination -> a single "how off is this reading" score,
    // used for the health score and the persistence/trend engine.
    //   30% vibration, 20% acoustic RMS (loudness), 50% harmonic content
    float overallDeviation =
        (0.30 * vibDeviation) +
        (0.20 * rmsDeviation) +
        (0.50 * harmonicDev);

    pushDeviationHistory(overallDeviation);

    int  persistenceCount = countPersistentAnomalies();
    bool trendWorsening    = isTrendWorsening();

    Serial.print(
      "DEVIATION | vib: "
    );
    Serial.print(vibDeviation, 1);

    Serial.print(
      "%  rms: "
    );
    Serial.print(rmsDeviation, 1);

    Serial.print(
      "%  50Hz: "
    );
    Serial.print(p50Deviation, 1);

    Serial.print(
      "%  100Hz: "
    );
    Serial.print(p100Deviation, 1);

    Serial.print(
      "%  200Hz: "
    );
    Serial.print(p200Deviation, 1);

    Serial.print(
      "%  300Hz: "
    );
    Serial.print(p300Deviation, 1);

    Serial.println(
      "%"
    );

    Serial.print(
      "PERSISTENCE | flagged in last "
    );
    Serial.print(HISTORY_LENGTH);
    Serial.print(
      " readings: "
    );
    Serial.print(persistenceCount);

    Serial.print(
      "   Trend worsening: "
    );
    Serial.println(trendWorsening ? "YES" : "no");


    // --------------------------------------------------
    // Health score: starts at 100, deducted by how far off
    // this reading is, plus extra penalties for a pattern
    // that is repeating and/or actively getting worse.
    // --------------------------------------------------

    float score = 100.0 - overallDeviation;

    if (persistenceCount >= 3) {
      score -= 10;
    }

    if (trendWorsening) {
      score -= 15;
    }

    if (score < 0)   score = 0;
    if (score > 100) score = 100;

    healthScore = (uint8_t)score;


    // --------------------------------------------------
    // Category: driven by PERSISTENCE, not a single reading.
    // A one-off deviation never escalates past MONITOR.
    // --------------------------------------------------

    if (persistenceCount >= 4 && trendWorsening) {
      categoryCode = CATEGORY_CRITICAL;
    }
    else if (persistenceCount >= 3) {
      categoryCode = CATEGORY_INSPECT_SOON;
    }
    else if (persistenceCount >= 1) {
      categoryCode = CATEGORY_MONITOR;
    }
    else {
      categoryCode = CATEGORY_NORMAL;
    }
  }


  Serial.print(
    "HEALTH SCORE: "
  );
  Serial.print(healthScore);

  Serial.print(
    "   CATEGORY: "
  );
  Serial.println(categoryName(categoryCode));


  // ========================================================
  // BLE BROADCAST (score + category only -- see comment above
  // broadcastHealthStatus() for why the full feature set isn't
  // stuffed into the tiny advertisement payload)
  // ========================================================

  broadcastHealthStatus(healthScore, categoryCode);


  // ========================================================
  // JSON STATUS OUTPUT (Serial -- for collector.py / dashboard)
  // ========================================================

  printJSONStatus(
    healthScore,
    categoryCode,
    harmonicDev,
    vibRMS_g,
    lastGoodTemperature,
    lastGoodHumidity,
    readLoadCurrentString(),
    power50,
    power100,
    power200,
    power300,
    baselinePower50,
    baselinePower100,
    baselinePower200,
    baselinePower300
  );


  // ========================================================
  // RGB + BUZZER
  // ========================================================

  switch (categoryCode) {

    case CATEGORY_BASELINING:
      setRGB(true, true, true);       // white
      digitalWrite(BUZZER_PIN, LOW);
      break;

    case CATEGORY_NORMAL:
      setRGB(false, true, false);     // green
      digitalWrite(BUZZER_PIN, LOW);
      break;

    case CATEGORY_MONITOR:
      setRGB(false, false, true);     // blue
      digitalWrite(BUZZER_PIN, LOW);
      break;

    case CATEGORY_INSPECT_SOON:
      setRGB(true, true, false);      // amber-ish (red+green)
      digitalWrite(BUZZER_PIN, LOW);
      break;

    case CATEGORY_CRITICAL:
      setRGB(true, false, false);     // red
      digitalWrite(BUZZER_PIN, HIGH);
      delay(300);
      digitalWrite(BUZZER_PIN, LOW);
      break;
  }


  Serial.println(
    "----------------------------------------"
  );


  // DHT11 requires approximately
  // 1-2 seconds between readings.

  delay(2000);
}