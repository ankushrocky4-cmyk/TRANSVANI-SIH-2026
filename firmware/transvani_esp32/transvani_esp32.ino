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

#define TRANSFORMER_ID        1        // change per deployed unit
#define BASELINE_SAMPLES       20        // loop cycles used to learn the baseline (~40s @ 2s/loop)
#define HISTORY_LENGTH          5        // rolling window used for persistence/trend checks

// Deviation is flagged as "anomalous" once it crosses this percentage.
#define ANOMALY_DEVIATION_THRESHOLD   25.0

// Category codes broadcast over BLE (kept as a single byte).
#define CATEGORY_NORMAL          0
#define CATEGORY_MONITOR         1
#define CATEGORY_INSPECT_SOON    2
#define CATEGORY_CRITICAL        3
#define CATEGORY_BASELINING      4


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

bool analyzeMicrophone(
    float &rms,
    int32_t &peak,
    float &dominantFrequency,
    float &power100,
    float &power200
) {

  if (!readMicrophone()) {

    return false;
  }


  rms =
      calculateRMS();


  peak =
      calculatePeak();


  power100 =
      goertzelPower(100.0);


  power200 =
      goertzelPower(200.0);


  dominantFrequency =
      findDominantFrequency();


  return true;
}


// ============================================================
//         BASELINE + HEALTH SCORE ENGINE (NEW)
// ============================================================
//
// Phase 1 (BASELINING): the first BASELINE_SAMPLES loop cycles
// are spent averaging each feature to learn what "normal" looks
// like for THIS transformer. No alerts are raised in this phase.
//
// Phase 2 (MONITORING): every reading is compared against the
// learned baseline. A single deviating reading nudges the health
// score down but does NOT escalate the category — only a
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
float basePower100Accum   = 0;
float basePower200Accum   = 0;

// The learned baseline (valid once systemState == MONITORING).
float baselineVibration = 0;
float baselineRMS       = 0;
float baselinePower100  = 0;
float baselinePower200  = 0;

// Rolling history of the combined per-reading deviation score.
// Index HISTORY_LENGTH-1 is always the most recent reading.
float deviationHistory[HISTORY_LENGTH] = {0, 0, 0, 0, 0};

// Reference orientation used to compute the vibration "movement index".
// Captured once, on the very first accelerometer reading.
int16_t refX = 0, refY = 0, refZ = 0;
bool referenceSet = false;


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
// Human-readable label for a category code (Serial debugging).
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


// ============================================================
//                    BLE BROADCAST (NEW)
// ============================================================
//
// Advertising-only BLE: no pairing, no GATT connection required.
// Any nearby scanner (a phone, a laptop, nRF Connect) can read
// the status straight out of the advertisement packet.
//
// Manufacturer data payload (6 bytes):
//   [0-1] Company ID placeholder (0xFFFF = unallocated/test range)
//   [2-3] Transformer ID (uint16, little-endian)
//   [4]   Health score (0-100)
//   [5]   Category code (see CATEGORY_* constants)
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
    "       TRANSVANI EDGE NODE V2"
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
    "  - Acoustic signal"
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

  int16_t x, y, z;

  bool accelOK =
      readADXL(
        x,
        y,
        z
      );

  float movementValue = 0;

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
      "DHT11 | READ FAILED"
    );
  }


  // ========================================================
  // MICROPHONE
  // ========================================================

  float rms = 0;

  int32_t peak = 0;

  float dominantFrequency = 0;

  float power100 = 0;

  float power200 = 0;


  bool microphoneOK =
      analyzeMicrophone(
        rms,
        peak,
        dominantFrequency,
        power100,
        power200
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
      "MIC | 100 Hz Power: "
    );

    Serial.println(
      power100,
      2
    );


    Serial.print(
      "MIC | 200 Hz Power: "
    );

    Serial.println(
      power200,
      2
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

  if (systemState == BASELINING) {

    // ------------------------------------------------------
    // Accumulate readings to learn what "normal" looks like.
    // ------------------------------------------------------

    baseVibrationAccum += movementValue;
    baseRMSAccum        += rms;
    basePower100Accum   += power100;
    basePower200Accum   += power200;

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
      baselinePower100    = basePower100Accum / BASELINE_SAMPLES;
      baselinePower200    = basePower200Accum / BASELINE_SAMPLES;

      systemState = MONITORING;

      Serial.println(
        "----------------------------------------"
      );

      Serial.println(
        "BASELINE ESTABLISHED:"
      );

      Serial.print(
        "  Vibration:  "
      );
      Serial.println(baselineVibration);

      Serial.print(
        "  RMS:        "
      );
      Serial.println(baselineRMS);

      Serial.print(
        "  100Hz power: "
      );
      Serial.println(baselinePower100);

      Serial.print(
        "  200Hz power: "
      );
      Serial.println(baselinePower200);

      Serial.println(
        "SWITCHING TO MONITORING MODE"
      );

      Serial.println(
        "----------------------------------------"
      );
    }

    healthScore  = 100;
    categoryCode = CATEGORY_BASELINING;
  }

  else {

    // ------------------------------------------------------
    // MONITORING: compare this reading against the baseline.
    // ------------------------------------------------------

    float vibDeviation  = percentDeviation(movementValue, baselineVibration);
    float rmsDeviation  = percentDeviation(rms, baselineRMS);
    float p100Deviation = percentDeviation(power100, baselinePower100);
    float p200Deviation = percentDeviation(power200, baselinePower200);

    // Weighted combination -> a single "how off is this reading" score.
    float overallDeviation =
        (0.35 * vibDeviation) +
        (0.25 * rmsDeviation) +
        (0.20 * p100Deviation) +
        (0.20 * p200Deviation);

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
      "%  100Hz: "
    );
    Serial.print(p100Deviation, 1);

    Serial.print(
      "%  200Hz: "
    );
    Serial.print(p200Deviation, 1);

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
  // BLE BROADCAST
  // ========================================================

  broadcastHealthStatus(healthScore, categoryCode);


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