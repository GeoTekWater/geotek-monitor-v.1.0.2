# Geotek Monitor

## IoT Water Infrastructure Monitoring and AI-Ready Data Platform

Geotek Monitor  v.1.0.2 is a water infrastructure monitoring system developed by Geotek Water Solutions Ltd in furtherance of the AI for Climate Resilience project funded by Klarna through Milkywire.

The system combines ESP32-based field devices, SIM800L mobile connectivity, environmental sensors, persistent flash storage, Supabase, and the Geotek Monitor Dashboard.

Geotek Monitor supports two types of water infrastructure:

1. Motorized water infrastructure.
2. Handpump water infrastructure.

The field devices collect operational information, store it locally, consolidate the readings into eight-hour reporting periods, and send the reports to Supabase through HTTPS.

---

# 1. System Architecture

```text
Water Infrastructure
        |
        v
Sensors and ESP32
        |
        v
Local Flash Storage
        |
        v
SIM800L Mobile Data
        |
        v
HTTPS Request
        |
        v
Supabase Edge Function
        |
        v
Supabase PostgreSQL Database
        |
        v
Geotek Monitor Dashboard
        |
        v
AI Analysis and Recommendations
```

---

# 2. Main Components

## 2.1 Motorized Water Infrastructure Monitor

The motorized monitor uses:

* ESP32 microcontroller.
* SIM800L GSM and GPRS module.
* Pulse-output water flow meter.
* Analogue TDS sensor.
* LittleFS flash storage.

The device performs the following functions:

* Sleeps when no pumping activity is detected.
* Wakes when the flow meter detects water movement.
* Records flow rate in litres per minute every five minutes.
* Records the volume of water delivered during each interval.
* Records TDS measurements during pumping.
* Stores each five-minute record in ESP32 flash memory.
* Consolidates all readings after eight hours.
* Sends the complete report to Supabase.
* Deletes the delivered report only after Supabase confirms successful receipt.
* Begins a new eight-hour reporting period.

### Expected Pump Flow Ranges

| Pump Size |       Expected Healthy Flow |
| --------- | --------------------------: |
| 0.5 HP    |  20 to 60 litres per minute |
| 1.0 HP    |  30 to 80 litres per minute |
| 1.5 HP    | 40 to 100 litres per minute |

These ranges are operational references and should be adjusted based on the installed pump, total dynamic head, pipe diameter, water level, and manufacturer specifications.

### TDS Screening Range

The current project screening range is:

* Below 50 ppm: Below the project screening range.
* 50 to 500 ppm: Within the project screening range.
* Above 500 ppm: Requires further review.

TDS alone does not confirm that water is safe for drinking. Microbiological testing and other chemical tests may still be required.

---

## 2.2 Handpump Movement Monitor

The handpump monitor uses:

* ESP32 microcontroller.
* SIM800L GSM and GPRS module.
* MPU6050 accelerometer and gyroscope.
* Protected handle-touch or contact sensor.
* LittleFS flash storage.

The device performs the following functions:

* Sleeps while the handpump is inactive.
* Wakes when the handle-touch input is activated.
* Reads upward movement along the MPU6050 Y-axis.
* Counts each valid upward movement once.
* Groups upward movements into sets of 500.
* Calculates the average Y-axis value for each completed set of 500 movements.
* Rounds each average to two decimal places.
* Stores every completed average in ESP32 flash memory.
* Sends all stored averages to Supabase after eight hours.
* Deletes delivered reports only after successful database receipt.
* Clears active counters and begins a new eight-hour reporting period.

### Example Handpump Average Record

```json
{
  "batch_index": 1,
  "movement_count": 500,
  "average_y": 2.41,
  "unit": "metres_per_second_squared",
  "window_offset_seconds": 3120
}
```

A partial set of fewer than 500 upward movements is recorded in the report diagnostics but is not treated as a completed average.

---

# 3. Reporting Schedule

Both monitoring systems operate on eight-hour reporting periods.

This produces three reporting periods within every twenty-four hours.

The period begins from the time the device starts unless a real-time clock or network-synchronized schedule is added.

For fixed reporting times, such as:

* 12:00 a.m.
* 8:00 a.m.
* 4:00 p.m.

Add a DS3231 real-time clock or validated network-time synchronization.

---

# 4. Data Storage Strategy

LittleFS is used to store readings on the ESP32.

## Motorized Monitor Storage

The device stores five-minute readings in:

```text
/active_motorized_samples.ndjson
```

When the eight-hour period ends, the readings are transferred into a finalized report such as:

```text
/report_0000000001.json
```

## Handpump Monitor Storage

The device stores completed 500-movement averages in:

```text
/active_handpump_averages.ndjson
```

When the eight-hour period ends, the averages are transferred into a finalized report such as:

```text
/handpump_report_0000000001.json
```

## Memory Clearing Policy

The ESP32 filesystem should not be formatted every eight hours.

Instead:

1. Active records are moved into a finalized report.
2. The active file is cleared for the next reporting period.
3. The finalized report remains in memory until Supabase confirms receipt.
4. The delivered report is then deleted.

This prevents data loss when the mobile network is unavailable.

---

# 5. Supabase Data Flow

The ESP32 does not write directly to Supabase database tables.

The recommended flow is:

```text
ESP32
  |
  v
Supabase Edge Function
  |
  v
Device authentication
  |
  v
Payload validation
  |
  v
Raw telemetry storage
  |
  v
Normalized sensor readings
  |
  v
Dashboard and AI services
```

The Edge Function endpoint is:

```text
https://YOUR_PROJECT_REFERENCE.supabase.co/functions/v1/ingest-sensor-data
```

The firmware sends the following headers:

```text
Content-Type: application/json
apikey: SUPABASE_PUBLISHABLE_KEY
x-device-id: REGISTERED_DEVICE_ID
x-device-key: UNIQUE_DEVICE_SECRET
```

Never place the Supabase service-role key inside ESP32 firmware.

---

# 6. Recommended Project Structure

```text
geotek-monitor/
|
|-- firmware/
|   |
|   |-- motorized-monitor/
|   |   |-- motorized-monitor.ino
|   |
|   |-- handpump-monitor/
|       |-- handpump-monitor.ino
|
|-- supabase/
|   |
|   |-- functions/
|   |   |-- ingest-sensor-data/
|   |       |-- index.ts
|   |
|   |-- migrations/
|   |   |-- geotek-monitor-schema.sql
|   |
|   |-- config.toml
|
|-- documentation/
|   |
|   |-- architecture.md
|   |-- calibration-guide.md
|   |-- installation-guide.md
|
|-- README.md
```

---

# 7. Required Arduino Libraries

Install the following libraries through the Arduino Library Manager:

```text
TinyGSM
ArduinoHttpClient
ArduinoJson
Adafruit MPU6050
Adafruit Unified Sensor
Adafruit BusIO
```

The ESP32 Arduino core also provides:

```text
LittleFS
Preferences
Wire
ESP32 Sleep APIs
```

Use ArduinoJson version 7 for the supplied firmware.

---

# 8. Recommended ESP32 Pin Assignment

## Motorized Monitor

| Component               | ESP32 Pin |
| ----------------------- | --------: |
| SIM800L TX to ESP32 RX2 |   GPIO 16 |
| ESP32 TX2 to SIM800L RX |   GPIO 17 |
| Flow meter signal       |   GPIO 27 |
| TDS analogue output     |   GPIO 34 |

## Handpump Monitor

| Component                     | ESP32 Pin |
| ----------------------------- | --------: |
| SIM800L TX to ESP32 RX2       |   GPIO 16 |
| ESP32 TX2 to SIM800L RX       |   GPIO 17 |
| MPU6050 SDA                   |   GPIO 21 |
| MPU6050 SCL                   |   GPIO 22 |
| Protected handle-touch output |   GPIO 33 |

Pin assignments may be changed in the firmware if required.

---

# 9. Firmware Configuration

Update the following values before deployment:

```cpp
const char APN[] =
  "YOUR_NETWORK_APN";

const char SUPABASE_HOST[] =
  "YOUR_PROJECT_REFERENCE.supabase.co";

const char SUPABASE_PUBLISHABLE_KEY[] =
  "YOUR_SUPABASE_PUBLISHABLE_KEY";

const char DEVICE_ID[] =
  "YOUR_REGISTERED_DEVICE_ID";

const char DEVICE_KEY[] =
  "YOUR_UNIQUE_DEVICE_SECRET";

const char WATER_POINT_ID[] =
  "YOUR_WATER_POINT_UUID";
```

For motorized systems, set the installed pump horsepower:

```cpp
const float PUMP_HORSEPOWER = 1.0F;
```

Supported values are:

```text
0.5
1.0
1.5
```

For handpump systems, confirm the MPU6050 mounting orientation:

```cpp
const int UPWARD_DIRECTION = 1;
```

Use:

```text
1
```

when upward movement produces a positive Y-axis deviation.

Use:

```text
-1
```

when upward movement produces a negative Y-axis deviation.

---

# 10. SIM800L Requirements

The SIM800L requires:

* A dedicated power supply.
* Approximately 4.0 V operating voltage.
* Sufficient current for transmission bursts.
* A common ground with the ESP32.
* Appropriate voltage-level protection on the serial connection.
* An active 2G mobile network.

Do not power the SIM800L from the ESP32 3.3 V pin.

The SIM800L may require up to approximately 2 A during transmission bursts. Poor power supply design is a common cause of failed network registration, resets, and incomplete uploads.

HTTPS capability depends on the SIM800L firmware version. Test the exact hardware and firmware against the Supabase endpoint before deployment.

---

# 11. Electrical Safety

All monitoring electronics must operate on an isolated, low-voltage circuit.

The ESP32, SIM800L, flow meter, TDS sensor, and MPU6050 must not be connected directly to pump mains electricity.

A qualified electronics or electrical technician should handle installations around motorized water infrastructure.

The handpump metal handle should not be connected directly to an ESP32 GPIO pin. Use a protected interface such as:

* Optoisolator.
* Transistor circuit.
* Capacitive-touch module.
* Protected contact sensor.
* Surge and static protection circuit.

Outdoor installations should also include protection against:

* Moisture.
* Dust.
* Corrosion.
* Lightning-induced voltage.
* Static discharge.
* Vandalism.
* Reverse polarity.

---

# 12. Sensor Calibration

## Flow Meter Calibration

The flow meter calibration value must be verified for the exact meter being used.

Use the following process:

1. Pass a known volume of water through the meter.
2. Record the total number of pulses.
3. Divide the number of pulses by the known volume.

```text
Pulses per litre =
Total recorded pulses / Known collected litres
```

Update:

```cpp
const float FLOW_PULSES_PER_LITRE = 450.0F;
```

with the measured result.

## TDS Sensor Calibration

Calibrate the TDS sensor using a suitable reference solution.

Update:

```cpp
const float TDS_CALIBRATION_MULTIPLIER = 1.0F;
```

after comparing the sensor output with the reference measurement.

A temperature sensor should be added in a future version because conductivity and TDS readings are affected by water temperature.

## MPU6050 Calibration

The MPU6050 must be calibrated after installation on the actual handpump.

Calibration should test:

* Normal upward movement.
* Normal downward movement.
* Small vibrations.
* Wind.
* Accidental contact.
* Sensor mounting changes.
* Handle impact.
* Deliberate tampering.

The following thresholds may require adjustment:

```cpp
const float UP_MOVEMENT_START_THRESHOLD = 1.20F;
const float UP_MOVEMENT_RELEASE_THRESHOLD = 0.25F;
const unsigned long MINIMUM_STROKE_GAP_MS = 300UL;
```

---

# 13. Supabase Database Tables

The recommended Supabase structure includes:

```text
water_points
devices
telemetry_batches
motorized_reports
handpump_reports
sensor_readings
motorized_interval_samples
handpump_average_batches
```

## water_points

Stores the identity and location of each monitored water infrastructure.

## devices

Stores registered monitoring devices and their hashed authentication credentials.

## telemetry_batches

Stores the complete raw eight-hour payload for auditing and recovery.

## motorized_reports

Stores summary values for motorized water infrastructure.

## handpump_reports

Stores summary values for handpump infrastructure.

## sensor_readings

Stores normalized values used for dashboard charts and AI analysis.

## motorized_interval_samples

Stores individual five-minute motorized flow and TDS readings.

## handpump_average_batches

Stores each average calculated from 500 upward movements.

---

# 14. Motorized Data Payload

A motorized report may contain:

```json
{
  "schema_version": 2,
  "report_id": "GM-MOTOR-001-R-1",
  "device_id": "GM-MOTOR-001",
  "water_point_id": "WATER_POINT_UUID",
  "infrastructure_type": "motorized",
  "reporting_period_hours": 8,
  "overall_status": "normal",
  "equipment": {
    "pump_horsepower": 1,
    "expected_minimum_flow_lpm": 30,
    "expected_maximum_flow_lpm": 80
  },
  "summary": {
    "total_volume_litres": 18240.5,
    "average_active_flow_lpm": 48.6,
    "minimum_active_flow_lpm": 34.2,
    "maximum_active_flow_lpm": 71.8,
    "latest_flow_lpm": 47.9,
    "active_flow_samples": 10,
    "total_flow_samples": 10,
    "flow_status": "within_expected_range"
  },
  "water_quality": {
    "parameter": "tds",
    "unit": "ppm",
    "sample_count": 10,
    "status": "within_project_screening_range",
    "average": 213.4,
    "minimum": 188.2,
    "maximum": 241.7,
    "latest": 219.6
  },
  "samples": []
}
```

---

# 15. Handpump Data Payload

A handpump report may contain:

```json
{
  "schema_version": 2,
  "report_id": "GM-HANDPUMP-001-HP-R-1",
  "device_id": "GM-HANDPUMP-001",
  "water_point_id": "WATER_POINT_UUID",
  "infrastructure_type": "handpump",
  "reporting_period_hours": 8,
  "overall_status": "movement_detected",
  "movement": {
    "handle_movement_count": 1240,
    "movements_per_hour": 155,
    "active_duration_seconds": 1820,
    "peak_acceleration_y_deviation": 4.22,
    "average_acceleration_y_deviation": 2.48,
    "peak_gyroscope_y_magnitude": 0,
    "average_gyroscope_y_magnitude": 0,
    "latest_acceleration_y": 2.37,
    "latest_gyroscope_y": 0
  },
  "averages_of_500": [
    {
      "batch_index": 1,
      "movement_count": 500,
      "average_y": 2.41,
      "unit": "metres_per_second_squared",
      "window_offset_seconds": 3120
    },
    {
      "batch_index": 2,
      "movement_count": 500,
      "average_y": 2.56,
      "unit": "metres_per_second_squared",
      "window_offset_seconds": 7280
    }
  ]
}
```

---

# 16. Device Registration

Every physical device should have:

* A unique device ID.
* A unique device secret.
* A registered water point ID.
* A defined infrastructure type.
* A firmware version.
* An active or inactive status.

Device secrets should be stored as secure hashes in Supabase.

Never store plain device secrets in the database.

The device secret should be known only to:

* The registered ESP32 device.
* Authorized Geotek administrators.
* The Supabase verification function.

---

# 17. Supabase Edge Function Responsibilities

The ingestion Edge Function should:

1. Accept POST requests only.
2. Require JSON content.
3. Validate the request size.
4. Authenticate the device.
5. Confirm that the device belongs to the submitted water point.
6. Confirm the infrastructure type.
7. Validate numerical values.
8. reject malformed or unrealistic readings.
9. prevent duplicate reports.
10. store the raw payload.
11. insert normalized summary readings.
12. insert individual five-minute motorized samples.
13. insert individual handpump 500-movement averages.
14. update the device last-seen timestamp.
15. return HTTP 201 for a new report.
16. return HTTP 200 for an already stored duplicate.

---

# 18. Dashboard Integration

The Geotek Monitor Dashboard can use the Supabase data to display:

## Motorized Water Infrastructure

* Five-minute flow-rate graph.
* Total water delivered.
* Average active flow rate.
* Minimum and maximum flow rate.
* Pump health status.
* TDS average, minimum, maximum, and latest value.
* Eight-hour operating history.
* AI-generated preventive maintenance recommendations.

## Handpump Water Infrastructure

* Average Y-axis values for each 500 movements.
* Total upward movement count.
* Movement frequency.
* Eight-hour activity pattern.
* Last active period.
* Inactivity alerts.
* AI-generated maintenance recommendations.

---

# 19. AI Integration

The stored data can be passed to a hosted Google Gemini agent.

The AI agent may analyze:

* Declining motorized flow rates.
* Unusual changes in pump output.
* TDS movement over time.
* Repeated periods of inactivity.
* Changes in handpump movement patterns.
* Reduced movement intensity.
* Maintenance history.
* Seasonal and hydroclimatic context.

Example AI output:

```text
The average motorized flow rate has declined from 48.6 litres per minute to 34.1 litres per minute across three reporting periods. The pump remains operational but is approaching the lower expected range for a 1 HP system. A field inspection of the pump, pipework, and water level is recommended.
```

AI outputs should be treated as decision support. Final maintenance, water safety, and drilling decisions should remain subject to qualified human review.

---

# 20. Testing Checklist

Before field deployment, confirm:

* ESP32 powers on reliably.
* SIM800L registers on the mobile network.
* GPRS connection succeeds.
* HTTPS requests reach Supabase.
* Device credentials are accepted.
* Flow pulses are counted accurately.
* Flow rate matches manual measurements.
* TDS readings match reference testing.
* MPU6050 detects upward movement correctly.
* Touch input wakes the handpump device.
* Light sleep works as expected.
* Five-minute samples are written to flash.
* 500-movement averages are written to flash.
* Eight-hour reports are generated.
* Failed uploads remain stored.
* Successful uploads are removed from flash.
* Duplicate reports are not inserted twice.
* Dashboard charts display the stored values.

---

# 21. Known Limitations

* The SIM800L depends on available 2G coverage.
* HTTPS support varies between SIM800L firmware versions.
* TDS alone does not establish drinking water safety.
* Flow rate alone cannot always distinguish a switched-off pump from a failed pump.
* The reporting schedule is based on device uptime unless an RTC is added.
* Light sleep reduces ESP32 power consumption but does not automatically reduce SIM800L consumption.
* MPU6050 movement thresholds require field calibration.
* A touch wire connected to outdoor metal infrastructure requires strong electrical protection.
* Power loss during an active reporting period may affect timing unless an RTC and stronger recovery mechanism are added.

---

# 22. Recommended Future Improvements

* Add a pump-current sensor to confirm when the motorized pump is powered.
* Add a water temperature sensor for improved TDS compensation.
* Add a DS3231 real-time clock.
* Add battery-voltage monitoring.
* Add SIM800L power control.
* Add device health heartbeat reports.
* Add firmware over-the-air update capability.
* Add additional water-quality sensors.
* Add tamper detection.
* Add remote configuration of thresholds.
* Add geospatial and hydroclimatic data integration.
* Add automated AI alerts and maintenance workflows.

---

# 23. Project Ownership

Geotek Monitor is developed and maintained by:

**Geotek Water Solutions Ltd**

Dashboard:

```text
https://dashboard.geotekwater.com
```

Website:

```text
https://www.geotekwater.com
```

The system is being developed in furtherance of a climate-resilience implementation supported by Klarna through Milkywire.

---

# 24. Disclaimer

This firmware and technical documentation require field calibration, hardware validation, security review, and supervised testing before production deployment.

Water quality classifications generated by the system are screening indicators and should not replace certified laboratory analysis or applicable public-health requirements.

AI-generated recommendations are advisory and should be reviewed by qualified engineers, hydrogeologists, water-quality professionals, or authorized field personnel.
