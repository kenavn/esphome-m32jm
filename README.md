# ESPHome M32JM pressure sensor

[![CI](https://github.com/kenavn/esphome-m32jm/actions/workflows/ci.yaml/badge.svg)](https://github.com/kenavn/esphome-m32jm/actions/workflows/ci.yaml)

ESPHome external component for the **TE Connectivity / MEAS M3200-family**
amplified piezoresistive pressure sensors with the **I²C digital output**
option (`M32JMx-xxxxxx-xxxPG/A/D` and similar variants).

Originally developed for and tested against the **`M32JM-000105-100PG`**
(0–100 PSI gauge, 3.3 V supply, I²C). Other ranges in the same family are
supported by setting the `full_scale_psi` config option.

## Why a custom component?

The M3200 family uses the MEAS / Honeywell SSC "Measurement Request +
Data Fetch" I²C protocol — **not** standard register-based I²C. A
"measurement request" is a *read header followed by an immediate STOP with
zero data bytes*; the next read returns the result. Generic libraries that
expect `read_register(reg)` semantics fail silently or return stale data.

The component encapsulates the three quirks documented in M3200 datasheet
§§1.6–1.8:

1. **Measurement Request** is a zero-byte read transaction (no register byte).
2. A **≥ 10 ms wait** is required before reading the result (datasheet
   response time: 3 ms non-sleep, 8.4 ms sleep).
3. Repeated-start during data is **not allowed** — Data Fetch must be a
   fresh START transaction, not a restart following the MR.

## Hardware compatibility

| Family    | Status      | Notes |
| --------- | ----------- | ----- |
| M3200 / M32JM I²C | **Tested** | `M32JM-000105-100PG` (cable variant) |
| M3200 SPI | Not supported | this component is I²C-only |
| M3200 analog | Not supported | use the analog `output` directly into an ADC |

The same protocol is used by other MEAS digital pressure modules
(85BSD, 86BSD, MSP300 with the digital option) and by the wider Honeywell
SSC/HSC/ABP I²C family. Should work with minor address/scaling tweaks but is
not tested here.

## Wiring

| Sensor wire | Function     | ESP32 pin       |
| ----------- | ------------ | --------------- |
| Red         | +Supply (3.3 V)\* | 3V3        |
| Black       | GND          | GND             |
| Green       | SDA          | GPIO21 (default) |
| White       | SCL          | GPIO22 (default) |

\* The M32JM accepts 2.7 – 5.0 V, but powering at 3.3 V keeps the I²C
levels native to the ESP32 (no level shifter needed). Powering at 5 V
requires a bidirectional level shifter on SDA and SCL — the ESP32 GPIOs
are not 5 V-tolerant.

**External 4.7 kΩ pull-ups on SDA and SCL to 3V3 are strongly recommended.**
The M3200 cable variant has none built in. The bus *will* run on the
ESP32's internal pull-ups (~45 kΩ, enabled automatically by ESPHome's I²C
driver) — short cables and a single device at 100 kHz can get away with
this — but the rising-edge time constant (RC ≈ 45 kΩ × bus capacitance)
exceeds the I²C standard-mode spec of 1 µs as soon as the bus has any
real length or capacitance. Symptoms when you push the limit: occasional
NACKs, garbled bytes, or `Data fetch failed` warnings, especially at
400 kHz, on cables over ~30 cm, or with multiple I²C devices on the same
bus. With proper 4.7 kΩ external pull-ups, RC drops to ~0.2 µs and the
bus is reliable up to fast-mode (400 kHz) on long cables.

## Installation

Add to your ESPHome YAML:

```yaml
external_components:
  - source: github://kenavn/esphome-m32jm@main

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 100kHz

sensor:
  - platform: m32jm
    address: 0x28
    full_scale_psi: 100.0
    update_interval: 1s
    pressure:
      name: "M32JM Pressure"
    temperature:
      name: "M32JM Temperature"
```

> **Use the `esp-idf` framework**, not Arduino. The MR transaction relies
> on the ESP-IDF I²C bus emitting a zero-byte read header. The Arduino
> TwoWire backend handles `len=0` inconsistently across platform versions.

A complete example is in [`example.yaml`](example.yaml).

## Configuration reference

### `sensor` block

| Option            | Type    | Default | Description |
| ----------------- | ------- | ------- | ----------- |
| `address`         | hex     | `0x28`  | I²C address. M3200 ordering options: `0x28`, `0x36`, `0x46`, `0x48`, `0x51`. |
| `full_scale_psi`  | float   | `100.0` | Pressure full-scale in PSI — must match your part suffix (`-100PG` → 100). |
| `update_interval` | time    | `60s`   | How often to read. The driver does an MR + 11 ms wait + DF per update. |
| `pressure`        | sensor  | —       | A standard ESPHome [sensor schema](https://esphome.io/components/sensor/index.html#config-sensor). Reports in PSI. |
| `temperature`     | sensor  | —       | A standard ESPHome sensor schema. Reports in °C. |
| `i2c_id`          | id      | —       | If you have multiple I²C buses, point this at the right one. |

`pressure` and `temperature` are both optional, but you'll usually want
at least one of them.

## Decoding (for the curious)

Each Data Fetch returns 4 bytes: `[byte0, byte1, byte2, byte3]`.

```
status     = (byte0 >> 6) & 0x03
                   00 = OK    01 = reserved
                   10 = stale  11 = fault
pressure_counts    = ((byte0 & 0x3F) << 8) | byte1            # 14-bit
temperature_counts = (byte2 << 3) | (byte3 >> 5)              # 11-bit

PSI  = (pressure_counts - 1000) / 14000 * full_scale_psi
T_C  = temperature_counts * 200 / 2048 - 50
```

The constants `1000` and `15000` are M3200 family-specific span counts (10 %
and 90 % of the 14-bit range, see datasheet §2.1). They're *not* `0` and
`16383` — that's a common mistake.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `Measurement request failed` on every cycle | Wiring (SDA/SCL swapped, wrong address, no pull-up at all). Enable `scan: true` on the i2c block to see what addresses respond. |
| Intermittent `Data fetch failed` or garbled readings | Pull-up too weak for the bus. ESP32 internal pull-ups (~45 kΩ) are out of spec on long cables or at 400 kHz — add external 4.7 kΩ pull-ups, or drop the I²C frequency to `50kHz`. |
| `Sensor fault status` (status `11`) | Sensor reports an internal fault — check supply voltage (must be ≥ 2.7 V) and that the sensor isn't physically over-pressured. |
| Stale data warnings (status `10`) | MR-to-DF wait was too short. The driver uses 11 ms by default; if you've forked it and reduced this, raise it back. |
| Pressure offset of a few % at zero | Normal — factory accuracy is ±1.5 % FS for gauge variants. |
| Wildly wrong values | Check your `full_scale_psi` matches the part-number suffix. A `-100PG` configured as `30.0` will scale 3.3× too small. |

## Development

A standalone PlatformIO + ESP-IDF prototype lives in
[`prototype/`](prototype/). It implements the same protocol in plain C and
was used to validate the wire-level behavior before the ESPHome port. Useful
as a reference if you ever need to debug the protocol directly with logic
analyzer.

```sh
cd prototype
pio run --target upload --target monitor
```

## References

- **M3200 datasheet, rev A20** (Nov 2023) — TE Connectivity. I²C protocol in
  §§1.1–1.8; status bits Table 1; "I²C Protocol Differences" §1.8; packet
  layout §1.6.1 / §2.1; response time Note 3. Includes an STM32 reference C
  driver in the appendix.
- **TE app note**: *Interfacing to MEAS Digital Pressure Modules* (AN802) —
  separate protocol document, also embedded in the current datasheet
  appendix.
- **Honeywell TN-008201**: *I²C Communications with Honeywell Digital Output
  Pressure Sensors* — defines the MR/DF pattern that MEAS adopted; useful
  for understanding the design intent.

## License

MIT — see [LICENSE](LICENSE).
