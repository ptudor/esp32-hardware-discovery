# Sensor catalog additions

These entries identify parts in a programmed board manifest. The discovery
component reads that manifest; application firmware supplies measurement drivers,
calibration and board wiring. All IDs below append to existing categories.
Existing IDs and the four-byte descriptor format remain unchanged.

## Digital sensors and monitors

Addresses are **7-bit I2C addresses**, before adding the read/write bit. Store
the address actually configured on the board. For SPI or I2S, use
`IC_INSTALLED(category, id)` and keep bus/pin configuration in board firmware.

| Part / manufacturer reference | Category | ID symbol = value | Interface / address |
| --- | --- | --- | --- |
| [LSM6DSRX][lsm6dsrx] | `CAT_IMU` | `IMU_LSM6DSRX = 8` | I2C `0x6A`/`0x6B`, or SPI |
| [MCP9804][mcp9804] | `CAT_TEMP` | `TEMP_MCP9804 = 6` | I2C `0x18`–`0x1F` |
| [STS35-DIS][sts35] | `CAT_TEMP` | `TEMP_STS35 = 7` | I2C `0x4A`/`0x4B` |
| [STS31A-DIS][sts31a] | `CAT_TEMP` | `TEMP_STS31A = 8` | I2C `0x4A`/`0x4B` |
| [NXP LM75AD][lm75a] | `CAT_TEMP` | `TEMP_LM75A_NXP = 9` | I2C `0x48`–`0x4F`; 11-bit temperature |
| [SHT21][sht21] | `CAT_TEMP` | `TEMP_SHT21 = 10` | I2C `0x40`; humidity and temperature |
| [BMP390L][bmp390l] | `CAT_PRESSURE` | `PRESSURE_BMP390L = 6` | I2C `0x76`/`0x77`, or SPI |
| [HDC2022][hdc2022] | `CAT_SENSOR` | `SENSOR_HDC2022 = 10` | I2C `0x40`/`0x41`; humidity and temperature |
| [HIH8121-021][hih8121] | `CAT_SENSOR` | `SENSOR_HIH8121 = 11` | I2C; [default `0x27`][humidicon-i2c] |
| [VCNL4200][vcnl4200] | `CAT_SENSOR` | `SENSOR_PROX_VCNL4200 = 12` | I2C `0x51`; proximity and ambient light |
| [AT42QT1070][at42qt1070] | `CAT_SENSOR` | `SENSOR_TOUCH_AT42QT1070 = 13` | I2C `0x1B` in comms mode; up to seven touch keys |
| [INA260][ina260] | `CAT_POWER` | `POWER_INA260 = 7` | I2C `0x40`–`0x4F`; current, voltage and power |
| [LTC2990][ltc2990] | `CAT_POWER` | `POWER_LTC2990 = 8` | I2C `0x4C`–`0x4F`; voltage, current and temperature |
| [ICS-43434][ics43434] | `CAT_AUDIO` | `AUDIO_ICS43434 = 6` | I2S microphone; descriptor address `0` |
| [MAX31856][max31856] | `CAT_SENSOR` | `SENSOR_THERMOCOUPLE_MAX31856 = 16` | SPI thermocouple converter; descriptor address `0` |

SHT21 stays beside SHT35 in `CAT_TEMP`; HDC2022 stays beside HDC2080 in
`CAT_SENSOR`. These categories identify component families, not every quantity
a part measures. Query the specific ID when choosing a driver.

The NXP LM75A entry is manufacturer-specific: the NXP part has an 11-bit
temperature result, while the [TI LM75A][lm75a-ti] has a 9-bit result. SHT21
uses I2C; the similarly named SHT21P uses PWM. BMP390L has its own ID to
preserve its exact identity instead of relabeling existing `PRESSURE_BMP390`
descriptors. A distinct ID does not imply driver incompatibility.
MAX31856 is not a MAX31855: it has a different register map, configurable
thermocouple types and fault detection, so it has its own ID.

### Address planning

VCNL4200 has a fixed `0x51` address. The 24AA02E64 responds throughout
`0x50`–`0x57`, so it cannot share a direct bus segment with VCNL4200. Use
an addressable 24AA025E64 or 24CS128 at another address, such as `0x50`,
or separate the devices onto different bus segments. See the
[EEPROM hardware notes](README.md#eeprom).

When the board's EEPROM address is known, read it directly with
`eeprom_read_capabilities()` rather than scanning the EEPROM block that also
contains VCNL4200. An address ACK alone does not establish device identity.

Other shared ranges also require planning: SHT21 and HDC2022 can use `0x40`,
while INA260 spans `0x40`–`0x4F`; LM75A, STS3x and LTC2990 overlap portions
of that range. Choose compatible straps before programming the manifest.

## Analog sensors

| Part / manufacturer reference | Category | ID symbol = value | Output |
| --- | --- | --- | --- |
| [LM35][lm35] | `CAT_TEMP` | `TEMP_LM35 = 11` | Analog temperature, 10 mV/degree C |
| [LM34][lm34] | `CAT_TEMP` | `TEMP_LM34 = 12` | Analog temperature, 10 mV/degree F |
| [NJL7502L][njl7502l] | `CAT_SENSOR` | `SENSOR_LIGHT_NJL7502L = 14` | Phototransistor current |
| [SFH 3310][sfh3310] | `CAT_SENSOR` | `SENSOR_LIGHT_SFH3310 = 15` | Phototransistor current |

Use `IC_INSTALLED(category, id)` for these identities. The ADC input, reference,
resistor network and conversion/calibration belong in the board configuration;
the descriptor does not encode an external ADC channel or its relationship to a
sensor. This catalog entry alone does not make an analog part readable by I2C.

## Example manifest entries

```c
// Within a capabilities record for an addressable EEPROM at 0x50:
caps.component_count = 6;
caps.components[0] = IC_EEPROM_SELF_24AA025E64(0x50);
caps.components[1] = IC_I2C(CAT_SENSOR, SENSOR_HDC2022, 0x41);
caps.components[2] = IC_I2C(CAT_SENSOR, SENSOR_PROX_VCNL4200, 0x51);
caps.components[3] = IC_I2C(CAT_POWER, POWER_INA260, 0x40);
caps.components[4] = IC_INSTALLED(CAT_AUDIO, AUDIO_ICS43434);
caps.components[5] = IC_INSTALLED(CAT_TEMP, TEMP_LM35);
```

Set the rest of the capabilities header as in the README's manufacturing
example. The addresses above assume matching hardware straps. Sensor drivers
must still validate responses and initialize each installed device.

[lsm6dsrx]: https://www.st.com/resource/en/datasheet/lsm6dsrx.pdf
[mcp9804]: https://ww1.microchip.com/downloads/en/DeviceDoc/MCP9804-Typical-Accuracy-Digital-Temperature-Sensor-Data-Sheet-DS20002203D.pdf
[sts35]: https://sensirion.com/media/documents/1DA31AFD/65D613A8/Datasheet_STS3x_DIS.pdf
[sts31a]: https://sensirion.com/media/documents/1C706A20/65D61469/Datasheet_STS3xA_DIS.pdf
[lm75a]: https://www.nxp.com/docs/en/data-sheet/LM75A.pdf
[lm75a-ti]: https://www.ti.com/lit/ds/symlink/lm75a.pdf
[sht21]: https://sensirion.com/media/documents/120BBE4C/63500094/Sensirion_Datasheet_Humidity_Sensor_SHT21.pdf
[bmp390l]: https://www.mouser.com/pdfDocs/bst-bmp390l-ds001.pdf
[hdc2022]: https://www.ti.com/lit/ds/symlink/hdc2022.pdf
[hih8121]: https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/humidity-with-temperature-sensors/honeywell-humidicon-hih8000-series/documents/sps-siot-hih8000-datasheet-009075-7-en-ciid-147072.pdf
[humidicon-i2c]: https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/humidity-with-temperature-sensors/common/documents/sps-siot-i2c-comms-humidicon-tn-009061-2-en-ciid-142171.pdf
[vcnl4200]: https://www.vishay.com/docs/84430/vcnl4200.pdf
[at42qt1070]: https://ww1.microchip.com/downloads/en/DeviceDoc/Atmel-9596-AT42-QTouch-BSW-AT42QT1070_Datasheet.pdf
[ina260]: https://www.ti.com/lit/ds/symlink/ina260.pdf
[ltc2990]: https://www.analog.com/media/en/technical-documentation/data-sheets/LTC2990.pdf
[ics43434]: https://invensense.tdk.com/wp-content/uploads/2016/02/DS-000069-ICS-43434-v1.2.pdf
[max31856]: https://www.analog.com/media/en/technical-documentation/data-sheets/max31856.pdf
[lm35]: https://www.ti.com/lit/ds/symlink/lm35.pdf
[lm34]: https://www.ti.com/lit/ds/symlink/lm34.pdf
[njl7502l]: https://www.nisshinbo-microdevices.co.jp/en/products/ambient-light-sensor/spec/?product=njl7502l
[sfh3310]: https://look.ams-osram.com/m/7d554e868615e06b/original/SFH-3310.pdf
