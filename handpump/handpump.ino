/*
  GEOTEK MONITOR
  HAND PUMP MOVEMENT MONITOR

  Author:
  GEOTEK WATER SOLUTIONS LTD

  Hardware:
  - ESP32
  - SIM800L GSM/GPRS module
  - MPU6050 accelerometer and gyroscope

  Data flow:
  MPU6050 -> ESP32 -> SIM800L -> HTTPS
  -> Supabase Edge Function -> Supabase Database

  Reporting behaviour:
  - Movement is monitored continuously.
  - Data is aggregated for eight hours.
  - A report is sent only when movement occurred.
  - No report is sent for an inactive eight-hour window.
  - Failed uploads are retried every fifteen minutes.
  - Movement data is reset only after successful upload.

  Installation requirement:
  The MPU6050 must be mounted so its Y-axis corresponds
  consistently with hand-pump handle movement.
*/

#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <float.h>

// ============================================================
// MOBILE NETWORK CONFIGURATION
// ============================================================

const char SIM_PIN[] = "";

const char APN[] = "YOUR_NETWORK_APN";
const char GPRS_USER[] = "";
const char GPRS_PASSWORD[] = "";

// ============================================================
// SUPABASE CONFIGURATION
// ============================================================

const char SUPABASE_HOST[] =
  "YOUR_PROJECT_REFERENCE.supabase.co";

const char SUPABASE_FUNCTION_PATH[] =
  "/functions/v1/ingest-sensor-data";

const char SUPABASE_PUBLISHABLE_KEY[] =
  "YOUR_SUPABASE_PUBLISHABLE_KEY";

const char DEVICE_ID[] =
  "GM-HANDPUMP-001";

const char DEVICE_KEY[] =
  "YOUR_UNIQUE_DEVICE_SECRET";

const char WATER_POINT_ID[] =
  "YOUR_WATER_POINT_UUID";

// ============================================================
// ESP32 PIN CONFIGURATION
// ============================================================

const int SIM800_RX_PIN = 16;
const int SIM800_TX_PIN = 17;

const int MPU6050_SDA_PIN = 21;
const int MPU6050_SCL_PIN = 22;

// ============================================================
// MOVEMENT DETECTION CONFIGURATION
// ============================================================

/*
  Acceleration is measured in metres per second squared.

  Gyroscope readings are measured in radians per second.

  These values must be calibrated after installing the sensor
  on the actual hand pump.
*/
const float ACCELERATION_TRIGGER_THRESHOLD =
  1.50F;

const float ACCELERATION_RELEASE_THRESHOLD =
  0.60F;

const float GYROSCOPE_TRIGGER_THRESHOLD =
  0.60F;

const float GYROSCOPE_RELEASE_THRESHOLD =
  0.20F;

/*
  Prevents one physical handle movement from being counted
  repeatedly while the signal remains above the threshold.
*/
const unsigned long MINIMUM_EVENT_GAP_MS =
  350UL;

/*
  The signal must remain below the release threshold for this
  period before a new movement can be counted.
*/
const unsigned long RELEASE_CONFIRMATION_MS =
  150UL;

const float BASELINE_LEARNING_RATE =
  0.005F;

// ============================================================
// TIMING CONFIGURATION
// ============================================================

const unsigned long SENSOR_SAMPLE_INTERVAL_MS =
  50UL;

const unsigned long REPORTING_INTERVAL_MS =
  8UL * 60UL * 60UL * 1000UL;

const unsigned long FAILED_UPLOAD_RETRY_MS =
  15UL * 60UL * 1000UL;

// ============================================================
// MODEM AND SENSOR OBJECTS
// ============================================================

HardwareSerial SerialAT(2);

TinyGsm modem(SerialAT);

TinyGsmClientSecure secureClient(modem);

Adafruit_MPU6050 mpu6050;

// ============================================================
// RESTING BASELINE
// ============================================================

float restingAccelerationY = 0.0F;
float restingGyroscopeY = 0.0F;

// ============================================================
// EIGHT-HOUR MOVEMENT AGGREGATION
// ============================================================

uint32_t movementEventCount = 0.0F;

// ============================================================
// E0;
uint32_t totalSensorSampleCount = 0;
uint32_t activeSensorSampleCount = 0;

double accumulatedAccelerationDeviation = 0.0;
double accumulatedGyroscopeMagnitude = 0.0;

float peakAccelerationYDeviation = 0.0F;
float peakGyroscopeYMagnitude = 0.0F;

float latestAccelerationY = 0.0F;
float latestGyroscopeY = 0.0F;

unsigned long firstMovementAt = 0;
unsigned long lastMovementAt = 0;

// ============================================================
// MOVEMENT STATE
// ============================================================

bool movementIsLatched = false;

unsigned long lastMovementEventAt = 0;
unsigned long releaseConditionStartedAt = 0;

// ============================================================
// TIMING STATE
// ============================================================

unsigned long reportingWindowStartedAt = 0;
unsigned long lastSensorSampleAt = 0;
unsigned long lastUploadAttemptAt = 0;

// ============================================================
// CELLULAR CONNECTION
// ============================================================

bool ensureCellularConnection() {
  if (!modem.isNetworkConnected()) {
    Serial.println(
      "Waiting for cellular network registration..."
    );

    if (!modem.waitForNetwork(60000L, true)) {
      Serial.println(
        "Cellular network registration failed."
      );

      return false;
    }
  }

  if (!modem.isGprsConnected()) {
    Serial.println(
      "Connecting to mobile data..."
    );

    if (
      !modem.gprsConnect(
        APN,
        GPRS_USER,
        GPRS_PASSWORD
      )
    ) {
      Serial.println(
        "Mobile data connection failed."
      );

      return false;
    }
  }

  Serial.println(
    "Cellular data connection is ready."
  );

  return true;
}

// ============================================================
// HTTPS TRANSMISSION
// ============================================================

bool sendJsonToSupabase(
  const String &jsonPayload
) {
  if (!ensureCellularConnection()) {
    return false;
  }

  HttpClient httpClient(
    secureClient,
    SUPABASE_HOST,
    443
  );

  httpClient.setHttpResponseTimeout(
    60000
  );

  Serial.println(
    "Sending HTTPS request to Supabase..."
  );

  httpClient.beginRequest();

  httpClient.post(
    SUPABASE_FUNCTION_PATH
  );

  httpClient.sendHeader(
    "Content-Type",
    "application/json"
  );

  httpClient.sendHeader(
    "apikey",
    SUPABASE_PUBLISHABLE_KEY
  );

  httpClient.sendHeader(
    "x-device-id",
    DEVICE_ID
  );

  httpClient.sendHeader(
    "x-device-key",
    DEVICE_KEY
  );

  httpClient.sendHeader(
    "Content-Length",
    jsonPayload.length()
  );

  httpClient.beginBody();
  httpClient.print(jsonPayload);
  httpClient.endRequest();

  int statusCode =
    httpClient.responseStatusCode();

  String responseBody =
    httpClient.responseBody();

  Serial.print("HTTP status: ");
  Serial.println(statusCode);

  if (responseBody.length() > 0) {
    Serial.print("Server response: ");
    Serial.println(responseBody);
  }

  httpClient.stop();
  secureClient.stop();

  return (
    statusCode >= 200 &&
    statusCode < 300
  );
}

// ============================================================
// MPU6050 BASELINE CALIBRATION
// ============================================================

void calibrateRestingBaseline() {
  const uint16_t CALIBRATION_SAMPLE_COUNT =
    250;

  double accelerationYSum = 0.0;
  double gyroscopeYSum = 0.0;

  Serial.println(
    "Calibrating MPU6050."
  );

  Serial.println(
    "Keep the hand pump completely stationary."
  );

  delay(2000);

  for (
    uint16_t sampleIndex = 0;
    sampleIndex <
      CALIBRATION_SAMPLE_COUNT;
    sampleIndex++
  ) {
    sensors_event_t acceleration;
    sensors_event_t gyroscope;
    sensors_event_t temperature;

    mpu6050.getEvent(
      &acceleration,
      &gyroscope,
      &temperature
    );

    accelerationYSum +=
      acceleration.acceleration.y;

    gyroscopeYSum +=
      gyroscope.gyro.y;

    delay(20);
  }

  restingAccelerationY =
    accelerationY gyroscope.gyro.y;

    delay(20);
  }

Sum /
    static_cast<float>(
      CALIBRATION_SAMPLE_COUNT
    );

  restingGyroscopeY =
    gyroscopeYSum /
    static_cast<float>(
      CALIBRATION_SAMPLE_COUNT
    );

  Serial.print(
    "Resting acceleration Y: "
  );

  Serial.println(
    restingAccelerationY,
    4
  );

  Serial.print(
    "Resting gyroscope Y: "
  );

  Serial.println(
    restingGyroscopeY,
    4
  );
}

// ============================================================
// MOVEMENT PROCESSING
// ============================================================

void processMovementSample(
  unsigned long currentTime
) {
  sensors_event_t acceleration;
  sensors_event_t gyroscope;
  sensors_event_t temperature;

  mpu6050.getEvent(
    &acceleration,
    &gyroscope,
    &temperature
  );

  latestAccelerationY =
    acceleration.acceleration.y;

  latestGyroscopeY =
    gyroscope.gyro.y;

  float accelerationDeviation =
    fabs(
      latestAccelerationY -
      restingAccelerationY
    );

  float gyroscopeDeviation =
    fabs(
      latestGyroscopeY -
      restingGyroscopeY
    );

  totalSensorSampleCount++;

  accumulatedAccelerationDeviation +=
    accelerationDeviation;

  accumulatedGyroscopeMagnitude +=
    gyroscopeDeviation;

  if (
    accelerationDeviation >
    peakAccelerationYDeviation
  ) {
    peakAccelerationYDeviation =
      accelerationDeviation;
  }

  if (
    gyroscopeDeviation >
    peakGyroscopeYMagnitude
  ) {
    peakGyroscopeYMagnitude =
      gyroscopeDeviation;
  }

  bool triggerCondition =
    accelerationDeviation >=
      ACCELERATION_TRIGGER_THRESHOLD
    ||
    gyroscopeDeviation >=
      GYROSCOPE_TRIGGER_THRESHOLD;

  bool releaseCondition =
    accelerationDeviation <=
      ACCELERATION_RELEASE_THRESHOLD
    &&
    gyroscopeDeviation <=
      GYROSCOPE_RELEASE_THRESHOLD;

  if (triggerCondition) {
    activeSensorSampleCount++;

    releaseConditionStartedAt = 0;

    if (
      !movementIsLatched &&
      currentTime -
        lastMovementEventAt >=
        MINIMUM_EVENT_GAP_MS
    ) {
      movementEventCount++;

      movementIsLatched = true;

      lastMovementEventAt =
        currentTime;

      lastMovementAt =
        currentTime;

      if (firstMovementAt == 0) {
        firstMovementAt =
          currentTime;
      }

      Serial.print(
        "Handle movement detected. Count: "
      );

      Serial.println(
        movementEventCount
      );
    }
  }
  else if (releaseCondition) {
    if (
      releaseConditionStartedAt == 0
    ) {
      releaseConditionStartedAt =
        currentTime;
    }

    if (
      currentTime -
        releaseConditionStartedAt >=
        RELEASE_CONFIRMATION_MS
    ) {
      movementIsLatched = false;

      /*
        Slowly compensate for temperature and mounting drift
        while the pump is stationary.
      */
      restingAccelerationY =
        restingAccelerationY *
        (
          1.0F -
          BASELINE_LEARNING_RATE
        )
        +
        latestAccelerationY *
        BASELINE_LEARNING_RATE;

      restingGyroscopeY =
        restingGyroscopeY *
        (
          1.0F -
          BASELINE_LEARNING_RATE
        )
        +
        latestGyroscopeY *
        BASELINE_LEARNING_RATE;
    }
  }
  else {
    releaseConditionStartedAt = 0;
  }
}

// ============================================================
// REPORT CALCULATIONS
// ============================================================

float calculateAverageAccelerationDeviation() {
  if (totalSensorSampleCount == 0) {
    return 0.0F;
  }

  return static_cast<float>(
    accumulatedAccelerationDeviation /
    static_cast<double>(
      totalSensorSampleCount
    )
  );
}

float calculateAverageGyroscopeMagnitude() {
  if (totalSensorSampleCount == 0) {
    return 0.0F;
  }

  return static_cast<float>(
    accumulatedGyroscopeMagnitude /
    static_cast<double>(
      totalSensorSampleCount
    )
  );
}

float calculateActiveDurationSeconds() {
  return (
    static_cast<float>(
      activeSensorSampleCount
    ) *
    static_cast<float>(
      SENSOR_SAMPLE_INTERVAL_MS
    )
  ) /
  1000.0F;
}

float calculateMovementsPerHour(
  unsigned long reportingDurationMs
) {
  if (
    reportingDurationMs == 0 ||
    movementEventCount == 0
  ) {
    return 0.0F;
  }

  float reportingHours =
    static_cast<float>(
      reportingDurationMs
    ) /
    3600000.0F;

  return
    static_cast<float>(
      movementEventCount
    ) /
    reportingHours;
}

// ============================================================
// PAYLOAD CREATION AND UPLOAD
// ============================================================

bool uploadEightHourActivityReport(
  unsigned long currentTime
) {
  if (movementEventCount == 0) {
    return true;
  }

  unsigned long reportingDurationMs =
    currentTime -
    reportingWindowStartedAt;

  float reportingDurationHours =
    static_cast<float>(
      reportingDurationMs
    ) /
    3600000.0F;

  float averageAccelerationDeviation =
    calculateAverageAccelerationDeviation();

  float averageGyroscopeMagnitude =
    calculateAverageGyroscopeMagnitude();

  float activeDurationSeconds =
    calculateActiveDurationSeconds();

  float movementsPerHour =
    calculateMovementsPerHour(
      reportingDurationMs
    );

  JsonDocument document;

  document["schema_version"] = 1;
  document["device_id"] = DEVICE_ID;

  document["water_point_id"] =
    WATER_POINT_ID;

  document["infrastructure_type"] =
    "handpump";

  document["reporting_period_hours"] =
    reportingDurationHours;

  document["overall_status"] =
    "movement_detected";

  JsonObject movement =
    document["movement"]
      .to<JsonObject>();

  movement["handle_movement_count"] =
    movementEventCount;

  movement["movements_per_hour"] =
    movementsPerHour;

  movement["active_duration_seconds"] =
    activeDurationSeconds;

  movement[
    "peak_acceleration_y_deviation"
  ] = peakAccelerationYDeviation;

  movement[
    "average_acceleration_y_deviation"
  ] = averageAccelerationDeviation;

  movement[
    "peak_gyroscope_y_magnitude"
  ] = peakGyroscopeYMagnitude;

  movement[
    "average_gyroscope_y_magnitude"
  ] = averageGyroscopeMagnitude;

  movement["latest_acceleration_y"] =
    latestAccelerationY;

  movement["latest_gyroscope_y"] =
    latestGyroscopeY;

  JsonObject diagnostics =
    document["diagnostics"]
      .to<JsonObject>();

  diagnostics[
    "total_sensor_samples"
  ] = totalSensorSampleCount;

  diagnostics[
    "active_sensor_samples"
  ] = activeSensorSampleCount;

  diagnostics[
    "sensor_sample_interval_ms"
  ] = SENSOR_SAMPLE_INTERVAL_MS;

  diagnostics[
    "resting_acceleration_y"
  ] = restingAccelerationY;

  diagnostics[
    "resting_gyroscope_y"
  ] = restingGyroscopeY;

  diagnostics[
    "acceleration_trigger_threshold"
  ] = ACCELERATION_TRIGGER_THRESHOLD;

  diagnostics[
    "gyroscope_trigger_threshold"
  ] = GYROSCOPE_TRIGGER_THRESHOLD;

  if (firstMovementAt > 0) {
    diagnostics[
      "first_movement_offset_seconds"
    ] =
      (
        firstMovementAt -
        reportingWindowStartedAt
      ) /
      1000UL;

    diagnostics[
      "last_movement_offset_seconds"
    ] =
      (
        lastMovementAt -
        reportingWindowStartedAt
      ) /
      1000UL;
  }

  String jsonPayload;

  serializeJson(
    document,
    jsonPayload
  );

  Serial.println(
    "Eight-hour hand pump report:"
  );

  Serial.println(jsonPayload);

  return sendJsonToSupabase(
    jsonPayload
  );
}

// ============================================================
// REPORTING WINDOW RESET
// ============================================================

void resetReportingWindow(
  unsigned long currentTime
) {
  movementEventCount = 0;
  totalSensorSampleCount = 0;
  activeSensorSampleCount = 0;

  accumulatedAccelerationDeviation = 0.0;
  accumulatedGyroscopeMagnitude = 0.0;

  peakAccelerationYDeviation = 0.0F;
  peakGyroscopeYMagnitude = 0.0F;

  latestAccelerationY = 0.0F;
  latestGyroscopeY = 0.0F;

  firstMovementAt = 0;
  lastMovementAt = 0;

  movementIsLatched = false;

  lastMovementEventAt = 0;
  releaseConditionStartedAt = 0;

  reportingWindowStartedAt =
    currentTime;

  lastSensorSampleAt =
    currentTime;

  lastUploadAttemptAt = 0;

  Serial.println(
    "New eight-hour reporting window started."
  );
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println(
    "Starting Geotek Hand Pump Monitor"
  );

  Wire.begin(
    MPU6050_SDA_PIN,
    MPU6050_SCL_PIN
  );

  if (!mpu6050.begin()) {
    Serial.println(
      "MPU6050 was not detected."
    );

    while (true) {
      delay(1000);
    }
  }

  mpu6050.setAccelerometerRange(
    MPU6050_RANGE_4_G
  );

  mpu6050.setGyroRange(
    MPU6050_RANGE_500_DEG
  );

  mpu6050.setFilterBandwidth(
    MPU6050_BAND_21_HZ
  );

  calibrateRestingBaseline();

  SerialAT.begin(
    9600,
    SERIAL_8N1,
    SIM800_RX_PIN,
    SIM800_TX_PIN
  );

  delay(3000);

  Serial.println(
    "Restarting SIM800L..."
  );

  modem.restart();

  if (strlen(SIM_PIN) > 0) {
    modem.simUnlock(SIM_PIN);
  }

  ensureCellularConnection();

  unsigned long currentTime =
    millis();

  reportingWindowStartedAt =
    currentTime;

  lastSensorSampleAt =
    currentTime;

  lastUploadAttemptAt = 0;
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  unsigned long currentTime =
    millis();

  if (
    currentTime -
    lastSensorSampleAt >=
    SENSOR_SAMPLE_INTERVAL_MS
  ) {
    lastSensorSampleAt =
      currentTime;

    processMovementSample(
      currentTime
    );
  }

  unsigned long reportingDuration =
    currentTime -
    reportingWindowStartedAt;

  bool reportingPeriodComplete =
    reportingDuration >=
    REPORTING_INTERVAL_MS;

  if (reportingPeriodComplete) {
    /*
      No data is transmitted when no handle movement
      occurred during the complete eight-hour period.
    */
    if (movementEventCount == 0) {
      Serial.println(
        "No hand pump use detected during this period."
      );

      Serial.println(
        "No movement report will be transmitted."
      );

      resetReportingWindow(
        currentTime
      );

      delay(20);
      return;
    }

    bool uploadRetryIsDue =
      lastUploadAttemptAt == 0 ||
      currentTime -
        lastUploadAttemptAt >=
        FAILED_UPLOAD_RETRY_MS;

    if (uploadRetryIsDue) {
      lastUploadAttemptAt =
        currentTime;

      bool uploadSucceeded =
        uploadEightHourActivityReport(
          currentTime
        );

      if (uploadSucceeded) {
        Serial.println(
          "Hand pump report uploaded successfully."
        );

        resetReportingWindow(
          millis()
        );
      }
      else {
        Serial.println(
          "Upload failed. Movement data retained for retry."
        );
      }
    }
  }

  delay(20);
}
