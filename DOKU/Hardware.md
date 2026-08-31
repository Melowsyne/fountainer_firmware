# Hardware — Wiring, Sensors, Calibration

Target: **ESP32-S3 DevKitC-1**, 16 MB flash, USB-Serial-JTAG console.
All direct hardware access is encapsulated in `src/device/hal.{c,h}`
(pattern: every function returns `bool`, `true` = ok); the AM2302 climate
sensor has its own driver module `src/device/onewire_am2302.{c,h}`.

## 1. Pin assignment

Defined in `src/device/hal.h` (taken from the verified predecessor project):

| GPIO | Define | Connected to |
|---|---|---|
| 4 | `HAL_DHT_GPIO` | AM2302/DHT22 data line (single-wire, bit-banged) |
| 5 | `HAL_SSR_GPIO` | Solid-state relay (pump), driven via LEDC PWM |
| 7 | `HAL_I2C_SCL_GPIO` | I2C SCL → ADS1115 (left header) |
| 8 | `HAL_I2C_SDA_GPIO` | I2C SDA → ADS1115 (left header) |

## 2. Pressure sensor path

The line-pressure sensor is a **0–5 V** analog transducer (original full
scale 500 PSI = 34.47 bar) read through an external **ADS1115** 16-bit ADC on
the I2C bus — not the ESP32's internal ADC. A voltage divider brings the
0–5 V output down to ≤ 3.3 V at the ADC input.

Conversion (`hal_pressure_bar_read` / combined `hal_pressure_read`, which
returns mV and bar from a single conversion):

```
bar = clamp((V − 0.5 V) / 4.0 V × range) × scale + offset
```

The curve is **runtime-calibratable** via datapoints — no reflash:

| Datapoint | Meaning | Default |
|---|---|---|
| `Fon_Sensor_Range_Bar` | full-scale range | 34.47 bar (500 PSI) |
| `Fon_Sensor_Scale` | multiplicative correction | 1.0 |
| `Fon_Sensor_Offset` | additive correction (mbar) | 0 |

`pump_task` re-applies the calibration from the datapoints once per second
(`hal_pressure_calibration_set`), so a `dp_write` takes effect immediately.

**Wiring diagnostics:** each reading is a 4-sample burst; the spread
(max − min, in sensor-mV) is exposed as `hal_pressure_noise_mv_get()` and
reported via the `Fon_Sensor_Noise_mV` datapoint — an early indicator of
grounding or cabling problems. Read errors count into
`Fon_Sensor_Err_Count`; the raw pin voltage is visible as
`Fon_Sensor_Voltage_mV`.

## 3. Pump relay

The pump (1 kW) is switched by a solid-state relay on GPIO 5, driven through
the LEDC peripheral. Only `pump_task` commands the relay in normal operation
(`hal_relay_set/get`); the watchdog's pre-reboot hook forces it **OFF**
before any escalated reboot, and `main_init` failure paths do the same —
the pump never stays energized through a firmware restart.

## 4. Climate sensor (AM2302 / DHT22)

Bit-banged single-wire protocol on GPIO 4 inside a critical section
(`am2302_init` / `am2302_climate_read`), sampled by `task_measure` on a 60 s
sub-period (pressure runs at 5 s). Values feed the `Ambient_Temperature` and
`Ambient_Humidity` datapoints. The driver exposes wiring-diagnosis getters
(last failing protocol phase, idle line level, low-pulse stretch) for bench
debugging.

## 5. On-chip peripherals

- **Internal temperature sensor** (`hal_internal_temp_read`) →
  `System_Temperature`.
- **USB/VBUS detection** (`hal_usb_connected_get`) — logging produces local
  console output only when a cable is actually attached; also useful to
  distinguish bench and field behavior.
- **Naming note:** the HAL entry point is `hal_setup()`, not `hal_init()` —
  the latter collides with a global symbol in the ESP-IDF Wi-Fi blob
  (`libpp.a`).

## 6. Flash layout

`partitions.csv` (16 MB):

| Partition | Offset | Size | Purpose |
|---|---|---|---|
| `nvs` | 0x9000 | 24 K | config datapoints, network config, trial flag |
| `otadata` | 0xf000 | 8 K | active-slot record (A/B OTA) |
| `phy_init` | 0x11000 | 4 K | RF calibration |
| `ota_0` | 0x20000 | 7.5 M | firmware slot A |
| `ota_1` | 0x7a0000 | 7.5 M | firmware slot B |
| `logstore` | 0xf20000 | 128 K | persistent WARN/ERROR log tier (2×64 K slots, runtime-detected) |

The bootloader and partition table are **not** OTA-updatable by design —
changing them requires USB.
