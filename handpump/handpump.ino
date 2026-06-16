/*
  GEOTEK MONITOR
  HAND PUMP MONITOR

  Author:
  GEOTEK WATER SOLUTIONS LTD

  ============================================================

  Hardware:
  - ESP32
  - SIM800L GSM/GPRS module
  - MPU6050 accelerometer and gyroscope
  - Protected handle-touch or contact sensor output

  Core operation:
  1. ESP32 enters light sleep while the pump is inactive.
  2. Touching the handle pulls the wake pin LOW.
  3. MPU6050 measures movement on the Y-axis.
  4. Every upward handle movement is detected once.
  5. The peak Y-axis deviation for each upward movement is stored.
  6. After 500 upward movements, their average is calculated.
  7. The average is rounded to two decimal places.
  8. Each completed average is stored in LittleFS.
  9. All completed averages are uploaded after eight hours.
  10. Delivered reports are deleted after Supabase confirms receipt.

  Required libraries:
  - TinyGSM
  - ArduinoHttpClient
  - ArduinoJson version 7
  - Adafruit MPU6050
  - Adafruit Unified Sensor
  - Adafruit BusIO

  Important:
  The handle must not be connected directly to the GPIO without
  a suitable protected contact, transistor, optoisolator, or
  capacitive-touch interface.
*/

#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include <Arduino.h>
#include <Wire.h>

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>

#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <esp_sleep.h>
#include <driver/gpio.h>

#include <float.h>

// ============================================================
// MOBILE NETWORK CONFIGURATION
// ============================================================

const char SIM_PIN[] = "";

const char APN[] =
  "YOUR_NETWORK_APN";

const char GPRS_USER[] =
  "";

const char GPRS_PASSWORD[] =
  "";

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

/*
  The protected touch or contact-sensor output connects here.

  This code assumes:
  - Pin is normally HIGH.
  - Touching the handle pulls the pin LOW.
*/
const int HANDLE_TOUCH_WAKE_PIN = 33;

const int HANDLE_TOUCH_ACTIVE_LEVEL = LOW;

// ============================================================
// MOVEMENT ORIENTATION
// ============================================================

/*
  Set this according to the MPU6050 mounting direction.

  Use 1 if an upward handle movement produces a positive
  Y-axis deviation.

  Use -1 if an upward handle movement produces a negative
  Y-axis deviation.
*/
const int UPWARD_DIRECTION = 1;

// ============================================================
// MOVEMENT DETECTION SETTINGS
// ============================================================

/*
  Acceleration values are in metres per second squared.

  These thresholds must be calibrated after mounting the
  sensor on the actual handpump.
*/
const float UP_MOVEMENT_START_THRESHOLD =
  1.20F;

const float UP_MOVEMENT_RELEASE_THRESHOLD =
  0.25F;

/*
  Baseline is adjusted only when movement is very small.
*/
const float BASELINE_UPDATE_THRESHOLD =
  0.30F;

const float BASELINE_LEARNING_RATE =
  0.003F;

/*
  Prevents vibration or signal bouncing from producing
  repeated movement counts.
*/
const unsigned long MINIMUM_STROKE_GAP_MS =
  300UL;

/*
  Sampling every 40 milliseconds gives 25 samples per second.
*/
const unsigned long SENSOR_SAMPLE_INTERVAL_MS =
  40UL;

/*
  After this period without an upward movement, the system
  returns to sleep if the touch signal is no longer active.
*/
const unsigned long PUMP_INACTIVITY_TIMEOUT_MS =
  90UL * 1000UL;

// ============================================================
// MOVEMENT BATCH CONFIGURATION
// ============================================================

const uint16_t MOVEMENTS_PER_AVERAGE =
  500;

// ============================================================
// REPORTING CONFIGURATION
// ============================================================

const unsigned long REPORTING_INTERVAL_MS =
  8UL * 60UL * 60UL * 1000UL;

const unsigned long FAILED_UPLOAD_RETRY_MS =
  15UL * 60UL * 1000UL;

const unsigned long MINIMUM_SLEEP_DURATION_MS =
  1000UL;

// ============================================================
// FLASH FILE PATHS
// ============================================================

/*
  Each completed 500-movement average is stored as one
  newline-delimited JSON record in this file.
*/
const char ACTIVE_AVERAGES_FILE[] =
  "/active_handpump_averages.ndjson";

const char REPORT_FILE_SUFFIX[] =
  ".json";

const size_t MAX_REPORT_FILE_BYTES =
  60UL * 1024UL;

// ============================================================
// MODEM AND SENSOR OBJECTS
// ============================================================

HardwareSerial SerialAT(2);

TinyGsm modem(SerialAT);

TinyGsmClientSecure secureClient(modem);

Adafruit_MPU6050 mpu6050;

Preferences preferences;

// ============================================================
// PERSISTENT REPORT SEQUENCE
// ============================================================

uint32_t nextReportSequence = 1;

// ============================================================
// SENSOR BASELINE AND FILTERING
// ============================================================

float restingAccelerationY = 0.0F;
float filteredAccelerationY = 0.0F;
float latestAccelerationY = 0.0F;

// ============================================================
// STROKE DETECTION STATE
// ============================================================

bool upwardStrokeIsActive = false;

float currentStrokePeakY = 0.0F;

unsigned long lastCompletedStrokeAt = 0;
unsigned long lastDetectedMovementAt = 0;

// ============================================================
// ACTIVE MONITORING STATE
// ============================================================

bool monitoringIsActive = false;

unsigned long monitoringStartedAt = 0;

uint32_t activeSensorSampleCount = 0;

// ============================================================
// CURRENT 500-MOVEMENT BATCH
// ============================================================

uint16_t currentBatchMovementCount = 0;

double currentBatchYSum = 0.0;

// ============================================================
// EIGHT-HOUR WINDOW TOTALS
// ============================================================

uint32_t totalUpMovements = 0;
uint32_t completedAverageCount = 0;

double totalStrokePeakYSum = 0.0;

float overallPeakY = 0.0F;
float latestCompletedStrokePeakY = 0.0F;

// ============================================================
// TIME STATE
// ============================================================

unsigned long reportingWindowStartedAt = 0;
unsigned long lastSensorSampleAt = 0;
unsigned long lastUploadAttemptAt = 0;

// ============================================================
// UTILITY FUNCTIONS
// ============================================================

float roundToTwoDecimals(float value) {
  return roundf(value * 100.0F) / 100.0F;
}

bool handleTouchIsActive() {
  return
    digitalRead(HANDLE_TOUCH_WAKE_PIN) ==
    HANDLE_TOUCH_ACTIVE_LEVEL;
}

// ============================================================
// FLASH STORAGE
// ============================================================

bool initializeFlashStorage() {
  /*
    Do not automatically format the filesystem in production.
    Automatic formatting could delete unsent field records.
  */
  if (!LittleFS.begin(false)) {
    Serial.println(
      "LittleFS could not be mounted."
    );

    Serial.println(
      "Format the LittleFS partition before deployment."
    );

    return false;
  }

  if (!LittleFS.exists(ACTIVE_AVERAGES_FILE)) {
    File file =
      LittleFS.open(
        ACTIVE_AVERAGES_FILE,
        FILE_WRITE
      );

    if (!file) {
      Serial.println(
        "Active averages file could not be created."
      );

      return false;
    }

    file.close();
  }

  return true;
}

bool clearActiveAveragesFile() {
  LittleFS.remove(
    ACTIVE_AVERAGES_FILE
  );

  File file =
    LittleFS.open(
      ACTIVE_AVERAGES_FILE,
      FILE_WRITE
    );

  if (!file) {
    Serial.println(
      "Active averages file could not be cleared."
    );

    return false;
  }

  file.close();

  return true;
}

// ============================================================
// REPORT SEQUENCE
// ============================================================

void loadReportSequence() {
  preferences.begin(
    "geotek-handpump",
    false
  );

  nextReportSequence =
    preferences.getUInt(
      "report-seq",
      1
    );
}

uint32_t allocateReportSequence() {
  uint32_t allocatedSequence =
    nextReportSequence;

  nextReportSequence++;

  preferences.putUInt(
    "report-seq",
    nextReportSequence
  );

  return allocatedSequence;
}

String createReportFilePath(
  uint32_t reportSequence
) {
  char filePath[48];

  snprintf(
    filePath,
    sizeof(filePath),
    "/handpump_report_%010lu.json",
    static_cast<unsigned long>(
      reportSequence
    )
  );

  return String(filePath);
}

// ============================================================
// MPU6050 CALIBRATION
// ============================================================

void calibrateRestingBaseline() {
  const uint16_t CALIBRATION_SAMPLE_COUNT =
    250;

  double accelerationYTotal = 0.0;

  Serial.println(
    "Calibrating the MPU6050."
  );

  Serial.println(
    "Keep the handpump stationary."
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

    accelerationYTotal +=
      acceleration.acceleration.y;

    delay(20);
  }

  restingAccelerationY =
    static_cast<float>(
      accelerationYTotal /
      CALIBRATION_SAMPLE_COUNT
    );

  filteredAccelerationY =
    restingAccelerationY;

  latestAccelerationY =
    restingAccelerationY;

  Serial.print(
    "Resting Y-axis baseline: "
  );

  Serial.println(
    restingAccelerationY,
    4
  );
}

// ============================================================
// MONITORING CONTROL
// ============================================================

void beginMovementMonitoring(
  unsigned long currentTime
) {
  if (monitoringIsActive) {
    return;
  }

  monitoringIsActive = true;

  monitoringStartedAt =
    currentTime;

  lastDetectedMovementAt =
    currentTime;

  lastSensorSampleAt =
    currentTime;

  upwardStrokeIsActive = false;
  currentStrokePeakY = 0.0F;

  Serial.println(
    "Handle touch detected."
  );

  Serial.println(
    "Movement monitoring started."
  );
}

void stopMovementMonitoring() {
  monitoringIsActive = false;

  upwardStrokeIsActive = false;
  currentStrokePeakY = 0.0F;

  Serial.println(
    "No recent pump movement detected."
  );

  Serial.println(
    "Movement monitoring stopped."
  );
}

// ============================================================
// SAVE ONE COMPLETED 500-MOVEMENT AVERAGE
// ============================================================

bool appendCompletedAverageToFlash(
  float averageY,
  unsigned long currentTime
) {
  completedAverageCount++;

  unsigned long windowOffsetSeconds =
    (
      currentTime -
      reportingWindowStartedAt
    ) /
    1000UL;

  JsonDocument averageRecord;

  averageRecord["batch_index"] =
    completedAverageCount;

  averageRecord["movement_count"] =
    MOVEMENTS_PER_AVERAGE;

  averageRecord["average_y"] =
    averageY;

  averageRecord["unit"] =
    "metres_per_second_squared";

  averageRecord["window_offset_seconds"] =
    windowOffsetSeconds;

  File file =
    LittleFS.open(
      ACTIVE_AVERAGES_FILE,
      FILE_APPEND
    );

  if (!file) {
    Serial.println(
      "The completed average could not be stored."
    );

    completedAverageCount--;

    return false;
  }

  serializeJson(
    averageRecord,
    file
  );

  file.print('\n');
  file.flush();
  file.close();

  Serial.print(
    "Average of 500 upward movements stored: "
  );

  Serial.println(
    averageY,
    2
  );

  return true;
}

// ============================================================
// COMPLETE ONE UPWARD MOVEMENT
// ============================================================

void completeUpwardMovement(
  float strokePeakY,
  unsigned long currentTime
) {
  /*
    Store only meaningful positive peaks.
  */
  if (
    strokePeakY <
    UP_MOVEMENT_START_THRESHOLD
  ) {
    return;
  }

  totalUpMovements++;

  currentBatchMovementCount++;

  currentBatchYSum +=
    strokePeakY;

  totalStrokePeakYSum +=
    strokePeakY;

  latestCompletedStrokePeakY =
    strokePeakY;

  if (strokePeakY > overallPeakY) {
    overallPeakY =
      strokePeakY;
  }

  lastCompletedStrokeAt =
    currentTime;

  lastDetectedMovementAt =
    currentTime;

  Serial.print(
    "Upward movement recorded. Total: "
  );

  Serial.println(
    totalUpMovements
  );

  if (
    currentBatchMovementCount >=
    MOVEMENTS_PER_AVERAGE
  ) {
    float calculatedAverage =
      static_cast<float>(
        currentBatchYSum /
        MOVEMENTS_PER_AVERAGE
      );

    calculatedAverage =
      roundToTwoDecimals(
        calculatedAverage
      );

    bool stored =
      appendCompletedAverageToFlash(
        calculatedAverage,
        currentTime
      );

    if (stored) {
      currentBatchMovementCount = 0;
      currentBatchYSum = 0.0;
    }
  }
}

// ============================================================
// PROCESS ONE MPU6050 SAMPLE
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

  /*
    Basic low-pass filtering to reduce high-frequency noise.
  */
  const float FILTER_ALPHA = 0.35F;

  filteredAccelerationY =
    FILTER_ALPHA *
    latestAccelerationY
    +
    (
      1.0F -
      FILTER_ALPHA
    ) *
    filteredAccelerationY;

  float rawYDeviation =
    filteredAccelerationY -
    restingAccelerationY;

  float upwardYDeviation =
    rawYDeviation *
    static_cast<float>(
      UPWARD_DIRECTION
    );

  activeSensorSampleCount++;

  if (!upwardStrokeIsActive) {
    bool strokeGapSatisfied =
      currentTime -
      lastCompletedStrokeAt >=
      MINIMUM_STROKE_GAP_MS;

    if (
      upwardYDeviation >=
        UP_MOVEMENT_START_THRESHOLD
      &&
      strokeGapSatisfied
    ) {
      upwardStrokeIsActive = true;

      currentStrokePeakY =
        upwardYDeviation;
    }
    else if (
      fabs(rawYDeviation) <=
      BASELINE_UPDATE_THRESHOLD
    ) {
      /*
        Slowly adapt to temperature drift and minor changes in
        mounting position while the handle is stationary.
      */
      restingAccelerationY =
        restingAccelerationY *
        (
          1.0F -
          BASELINE_LEARNING_RATE
        )
        +
        filteredAccelerationY *
        BASELINE_LEARNING_RATE;
    }
  }
  else {
    if (
      upwardYDeviation >
      currentStrokePeakY
    ) {
      currentStrokePeakY =
        upwardYDeviation;
    }

    if (
      upwardYDeviation <=
      UP_MOVEMENT_RELEASE_THRESHOLD
    ) {
      completeUpwardMovement(
        currentStrokePeakY,
        currentTime
      );

      upwardStrokeIsActive = false;
      currentStrokePeakY = 0.0F;
    }
  }
}

// ============================================================
// REPORT STATISTICS
// ============================================================

float calculateOverallAverageY() {
  if (totalUpMovements == 0) {
    return 0.0F;
  }

  float average =
    static_cast<float>(
      totalStrokePeakYSum /
      totalUpMovements
    );

  return roundToTwoDecimals(
    average
  );
}

float calculateMovementsPerHour(
  unsigned long reportingDurationMs
) {
  if (
    reportingDurationMs == 0 ||
    totalUpMovements == 0
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
      totalUpMovements
    ) /
    reportingHours;
}

float calculateActiveDurationSeconds() {
  return
    (
      static_cast<float>(
        activeSensorSampleCount
      ) *
      static_cast<float>(
        SENSOR_SAMPLE_INTERVAL_MS
      )
    ) /
    1000.0F;
}

// ============================================================
// CREATE THE FINAL EIGHT-HOUR REPORT
// ============================================================

bool finalizeEightHourReport(
  unsigned long currentTime
) {
  unsigned long reportingDurationMs =
    currentTime -
    reportingWindowStartedAt;

  /*
    If no actual upward movement occurred, no database report
    is generated.
  */
  if (totalUpMovements == 0) {
    Serial.println(
      "No upward movements occurred during this period."
    );

    clearActiveAveragesFile();

    return true;
  }

  uint32_t reportSequence =
    allocateReportSequence();

  String reportFilePath =
    createReportFilePath(
      reportSequence
    );

  String reportId =
    String(DEVICE_ID) +
    "-HP-R-" +
    String(reportSequence);

  float reportingPeriodHours =
    static_cast<float>(
      reportingDurationMs
    ) /
    3600000.0F;

  float movementsPerHour =
    calculateMovementsPerHour(
      reportingDurationMs
    );

  float activeDurationSeconds =
    calculateActiveDurationSeconds();

  float overallAverageY =
    calculateOverallAverageY();

  JsonDocument metadata;

  metadata["schema_version"] = 2;

  metadata["report_id"] =
    reportId;

  metadata["device_id"] =
    DEVICE_ID;

  metadata["water_point_id"] =
    WATER_POINT_ID;

  metadata["infrastructure_type"] =
    "handpump";

  metadata["reporting_period_hours"] =
    reportingPeriodHours;

  metadata["overall_status"] =
    "movement_detected";

  JsonObject movement =
    metadata["movement"]
      .to<JsonObject>();

  movement["handle_movement_count"] =
    totalUpMovements;

  movement["movements_per_hour"] =
    movementsPerHour;

  movement["active_duration_seconds"] =
    activeDurationSeconds;

  movement[
    "peak_acceleration_y_deviation"
  ] = roundToTwoDecimals(
    overallPeakY
  );

  movement[
    "average_acceleration_y_deviation"
  ] = overallAverageY;

  /*
    The present analysis is based on the accelerometer Y-axis.
    Gyroscope values are retained as zero to remain compatible
    with the existing Supabase payload validator.
  */
  movement[
    "peak_gyroscope_y_magnitude"
  ] = 0.0F;

  movement[
    "average_gyroscope_y_magnitude"
  ] = 0.0F;

  movement["latest_acceleration_y"] =
    roundToTwoDecimals(
      latestAccelerationY
    );

  movement["latest_gyroscope_y"] =
    0.0F;

  JsonObject diagnostics =
    metadata["diagnostics"]
      .to<JsonObject>();

  diagnostics[
    "movements_per_completed_average"
  ] = MOVEMENTS_PER_AVERAGE;

  diagnostics[
    "completed_average_count"
  ] = completedAverageCount;

  diagnostics[
    "partial_batch_movement_count"
  ] = currentBatchMovementCount;

  diagnostics[
    "partial_batch_y_sum"
  ] = currentBatchYSum;

  diagnostics[
    "total_up_movements"
  ] = totalUpMovements;

  diagnostics[
    "sensor_sample_interval_ms"
  ] = SENSOR_SAMPLE_INTERVAL_MS;

  diagnostics[
    "upward_direction"
  ] = UPWARD_DIRECTION;

  diagnostics[
    "up_movement_start_threshold"
  ] = UP_MOVEMENT_START_THRESHOLD;

  diagnostics[
    "up_movement_release_threshold"
  ] = UP_MOVEMENT_RELEASE_THRESHOLD;

  /*
    Convert the metadata to text and remove its final closing
    brace. The stored averages array will then be appended.
  */
  String metadataText;

  serializeJson(
    metadata,
    metadataText
  );

  if (metadataText.endsWith("}")) {
    metadataText.remove(
      metadataText.length() - 1
    );
  }

  File reportFile =
    LittleFS.open(
      reportFilePath.c_str(),
      FILE_WRITE
    );

  if (!reportFile) {
    Serial.println(
      "The final report file could not be created."
    );

    return false;
  }

  reportFile.print(
    metadataText
  );

  reportFile.print(
    ",\"averages_of_500\":["
  );

  File averagesFile =
    LittleFS.open(
      ACTIVE_AVERAGES_FILE,
      FILE_READ
    );

  bool firstAverage = true;

  if (averagesFile) {
    while (averagesFile.available()) {
      String averageLine =
        averagesFile.readStringUntil('\n');

      averageLine.trim();

      if (
        averageLine.length() == 0
      ) {
        continue;
      }

      if (!firstAverage) {
        reportFile.print(',');
      }

      reportFile.print(
        averageLine
      );

      firstAverage = false;
    }

    averagesFile.close();
  }

  reportFile.print("]}");
  reportFile.flush();

  size_t reportSize =
    reportFile.size();

  reportFile.close();

  if (
    reportSize >
    MAX_REPORT_FILE_BYTES
  ) {
    Serial.println(
      "Warning: the final report exceeds the recommended size."
    );
  }

  /*
    Completed averages are now safely stored in the finalized
    report file. The active averages file can be cleared for
    the next eight-hour period.
  */
  clearActiveAveragesFile();

  Serial.print(
    "Eight-hour handpump report queued: "
  );

  Serial.println(
    reportFilePath
  );

  return true;
}

// ============================================================
// RESET THE ACTIVE EIGHT-HOUR WINDOW
// ============================================================

void resetEightHourWindow(
  unsigned long currentTime
) {
  monitoringIsActive = false;
  upwardStrokeIsActive = false;

  currentStrokePeakY = 0.0F;

  currentBatchMovementCount = 0;
  currentBatchYSum = 0.0;

  totalUpMovements = 0;
  completedAverageCount = 0;

  totalStrokePeakYSum = 0.0;

  overallPeakY = 0.0F;
  latestCompletedStrokePeakY = 0.0F;

  activeSensorSampleCount = 0;

  monitoringStartedAt = 0;
  lastDetectedMovementAt = 0;
  lastCompletedStrokeAt = 0;

  reportingWindowStartedAt =
    currentTime;

  lastSensorSampleAt =
    currentTime;

  lastUploadAttemptAt = 0;

  Serial.println(
    "New eight-hour handpump monitoring period started."
  );
}

// ============================================================
// FIND PENDING REPORTS
// ============================================================

bool isHandpumpReportFile(
  const String &fileName
) {
  return
    fileName.indexOf(
      "handpump_report_"
    ) >= 0
    &&
    fileName.endsWith(
      REPORT_FILE_SUFFIX
    );
}

bool findOldestPendingReport(
  String &oldestReportPath
) {
  oldestReportPath = "";

  File rootDirectory =
    LittleFS.open("/");

  if (!rootDirectory) {
    return false;
  }

  File currentFile =
    rootDirectory.openNextFile();

  while (currentFile) {
    String currentFileName =
      currentFile.name();

    if (
      !currentFile.isDirectory()
      &&
      isHandpumpReportFile(
        currentFileName
      )
    ) {
      if (
        oldestReportPath.length() == 0
        ||
        currentFileName <
          oldestReportPath
      ) {
        oldestReportPath =
          currentFileName;
      }
    }

    currentFile.close();

    currentFile =
      rootDirectory.openNextFile();
  }

  rootDirectory.close();

  return
    oldestReportPath.length() > 0;
}

bool pendingReportsExist() {
  String reportPath;

  return findOldestPendingReport(
    reportPath
  );
}

// ============================================================
// CELLULAR CONNECTION
// ============================================================

bool ensureCellularConnection() {
  if (!modem.isNetworkConnected()) {
    Serial.println(
      "Waiting for cellular network registration."
    );

    if (!modem.waitForNetwork(
      60000L,
      true
    )) {
      Serial.println(
        "Cellular network registration failed."
      );

      return false;
    }
  }

  if (!modem.isGprsConnected()) {
    Serial.println(
      "Connecting to mobile data."
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

  return true;
}

// ============================================================
// SEND ONE REPORT TO SUPABASE
// ============================================================

bool sendReportFileToSupabase(
  const String &reportFilePath
) {
  File reportFile =
    LittleFS.open(
      reportFilePath.c_str(),
      FILE_READ
    );

  if (!reportFile) {
    Serial.println(
      "The pending report could not be opened."
    );

    return false;
  }

  if (
    reportFile.size() >
    MAX_REPORT_FILE_BYTES
  ) {
    Serial.println(
      "The pending report is too large to transmit."
    );

    reportFile.close();

    return false;
  }

  String reportPayload;

  reportPayload.reserve(
    reportFile.size() + 1
  );

  while (reportFile.available()) {
    reportPayload +=
      static_cast<char>(
        reportFile.read()
      );
  }

  reportFile.close();

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
    reportPayload.length()
  );

  httpClient.beginBody();
  httpClient.print(
    reportPayload
  );
  httpClient.endRequest();

  int responseStatus =
    httpClient.responseStatusCode();

  String responseBody =
    httpClient.responseBody();

  Serial.print(
    "Supabase response status: "
  );

  Serial.println(
    responseStatus
  );

  if (
    responseBody.length() > 0
  ) {
    Serial.println(
      responseBody
    );
  }

  httpClient.stop();
  secureClient.stop();

  bool requestSucceeded =
    responseStatus >= 200
    &&
    responseStatus < 300;

  if (requestSucceeded) {
    /*
      Erase the delivered report only after Supabase confirms
      successful receipt.
    */
    if (
      LittleFS.remove(
        reportFilePath.c_str()
      )
    ) {
      Serial.print(
        "Delivered report erased from memory: "
      );

      Serial.println(
        reportFilePath
      );
    }
  }

  return requestSucceeded;
}

// ============================================================
// UPLOAD ALL PENDING REPORTS
// ============================================================

void uploadPendingReports() {
  String pendingReportPath;

  while (
    findOldestPendingReport(
      pendingReportPath
    )
  ) {
    Serial.print(
      "Uploading pending report: "
    );

    Serial.println(
      pendingReportPath
    );

    bool uploadSucceeded =
      sendReportFileToSupabase(
        pendingReportPath
      );

    if (!uploadSucceeded) {
      Serial.println(
        "Upload failed. Report remains stored."
      );

      break;
    }
  }

  if (modem.isGprsConnected()) {
    modem.gprsDisconnect();
  }
}

// ============================================================
// LIGHT-SLEEP MANAGEMENT
// ============================================================

unsigned long calculateSleepDuration(
  unsigned long currentTime
) {
  unsigned long windowElapsed =
    currentTime -
    reportingWindowStartedAt;

  if (
    windowElapsed >=
    REPORTING_INTERVAL_MS
  ) {
    return
      MINIMUM_SLEEP_DURATION_MS;
  }

  unsigned long timeUntilWindowEnd =
    REPORTING_INTERVAL_MS -
    windowElapsed;

  unsigned long requestedSleepDuration =
    timeUntilWindowEnd;

  if (pendingReportsExist()) {
    unsigned long timeUntilRetry;

    if (
      lastUploadAttemptAt == 0
      ||
      currentTime -
        lastUploadAttemptAt >=
        FAILED_UPLOAD_RETRY_MS
    ) {
      timeUntilRetry =
        MINIMUM_SLEEP_DURATION_MS;
    }
    else {
      timeUntilRetry =
        FAILED_UPLOAD_RETRY_MS -
        (
          currentTime -
          lastUploadAttemptAt
        );
    }

    if (
      timeUntilRetry <
      requestedSleepDuration
    ) {
      requestedSleepDuration =
        timeUntilRetry;
    }
  }

  if (
    requestedSleepDuration <
    MINIMUM_SLEEP_DURATION_MS
  ) {
    requestedSleepDuration =
      MINIMUM_SLEEP_DURATION_MS;
  }

  return requestedSleepDuration;
}

bool enterHandleTouchLightSleep(
  unsigned long sleepDurationMs
) {
  /*
    Do not enter sleep while the touch output is still active.
    A level-triggered wake signal that remains active would
    cause immediate repeated wake-ups.
  */
  if (handleTouchIsActive()) {
    return false;
  }

  gpio_wakeup_enable(
    static_cast<gpio_num_t>(
      HANDLE_TOUCH_WAKE_PIN
    ),
    GPIO_INTR_LOW_LEVEL
  );

  esp_sleep_enable_gpio_wakeup();

  esp_sleep_enable_timer_wakeup(
    static_cast<uint64_t>(
      sleepDurationMs
    ) *
    1000ULL
  );

  Serial.print(
    "Handpump inactive. Entering light sleep for up to "
  );

  Serial.print(
    sleepDurationMs /
    1000UL
  );

  Serial.println(
    " seconds."
  );

  Serial.flush();

  esp_light_sleep_start();

  esp_sleep_wakeup_cause_t wakeCause =
    esp_sleep_get_wakeup_cause();

  gpio_wakeup_disable(
    static_cast<gpio_num_t>(
      HANDLE_TOUCH_WAKE_PIN
    )
  );

  esp_sleep_disable_wakeup_source(
    ESP_SLEEP_WAKEUP_GPIO
  );

  esp_sleep_disable_wakeup_source(
    ESP_SLEEP_WAKEUP_TIMER
  );

  if (
    wakeCause ==
    ESP_SLEEP_WAKEUP_GPIO
  ) {
    Serial.println(
      "ESP32 woke because the handpump handle was touched."
    );

    return true;
  }

  Serial.println(
    "ESP32 woke because the reporting timer expired."
  );

  return false;
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println(
    "Starting Geotek Handpump Monitor."
  );

  if (!initializeFlashStorage()) {
    while (true) {
      delay(1000);
    }
  }

  loadReportSequence();

  pinMode(
    HANDLE_TOUCH_WAKE_PIN,
    INPUT_PULLUP
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
    "Initializing SIM800L."
  );

  modem.restart();

  if (strlen(SIM_PIN) > 0) {
    modem.simUnlock(
      SIM_PIN
    );
  }

  unsigned long currentTime =
    millis();

  reportingWindowStartedAt =
    currentTime;

  lastSensorSampleAt =
    currentTime;

  lastUploadAttemptAt = 0;

  if (pendingReportsExist()) {
    Serial.println(
      "Previously queued reports were found."
    );
  }
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  unsigned long currentTime =
    millis();

  // ----------------------------------------------------------
  // START MONITORING WHEN THE HANDLE TOUCH INPUT IS ACTIVE
  // ----------------------------------------------------------

  if (
    !monitoringIsActive
    &&
    handleTouchIsActive()
  ) {
    beginMovementMonitoring(
      currentTime
    );
  }

  // ----------------------------------------------------------
  // READ MPU6050 WHILE MONITORING IS ACTIVE
  // ----------------------------------------------------------

  if (
    monitoringIsActive
    &&
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

  // ----------------------------------------------------------
  // STOP MONITORING AFTER PROLONGED INACTIVITY
  // ----------------------------------------------------------

  if (monitoringIsActive) {
    bool movementHasTimedOut =
      currentTime -
      lastDetectedMovementAt >=
      PUMP_INACTIVITY_TIMEOUT_MS;

    if (
      movementHasTimedOut
      &&
      !handleTouchIsActive()
    ) {
      stopMovementMonitoring();
    }
  }

  // ----------------------------------------------------------
  // FINALIZE THE EIGHT-HOUR REPORT
  // ----------------------------------------------------------

  unsigned long reportingWindowElapsed =
    currentTime -
    reportingWindowStartedAt;

  if (
    reportingWindowElapsed >=
    REPORTING_INTERVAL_MS
  ) {
    bool reportFinalized =
      finalizeEightHourReport(
        currentTime
      );

    if (reportFinalized) {
      /*
        Begin the next reporting period immediately.

        All active movement counters are cleared. Finalized
        reports remain in flash until successfully delivered.
      */
      resetEightHourWindow(
        millis()
      );

      uploadPendingReports();
    }
    else {
      Serial.println(
        "The eight-hour report could not be finalized."
      );
    }
  }

  // ----------------------------------------------------------
  // RETRY REPORTS THAT FAILED TO UPLOAD
  // ----------------------------------------------------------

  if (
    pendingReportsExist()
    &&
    (
      lastUploadAttemptAt == 0
      ||
      currentTime -
        lastUploadAttemptAt >=
        FAILED_UPLOAD_RETRY_MS
    )
  ) {
    lastUploadAttemptAt =
      currentTime;

    uploadPendingReports();
  }

  // ----------------------------------------------------------
  // ENTER LIGHT SLEEP WHEN THE HANDPUMP IS INACTIVE
  // ----------------------------------------------------------

  if (!monitoringIsActive) {
    unsigned long sleepDuration =
      calculateSleepDuration(
        millis()
      );

    bool wokeFromHandleTouch =
      enterHandleTouchLightSleep(
        sleepDuration
      );

    if (wokeFromHandleTouch) {
      beginMovementMonitoring(
        millis()
      );
    }
  }

  delay(20);
}