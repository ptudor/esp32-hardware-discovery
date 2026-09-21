# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2025-11-04

### Added
- Initial release
- 4-byte IC descriptor format: [category][id][i2c_address][status]
- 23 component categories (RTC, GPS, IMU, CRYPTO, DISPLAY, COMM, USB_SERIAL, MOTOR, TEMP, PRESSURE, SENSOR, AUDIO, POWER, LED, IO_EXPANDER, MEMORY, MCU, CONNECTOR, BUTTON, BATTERY, ACTUATOR, ANTENNA, MISC)
- EEPROM self-reference validation (component[0])
- 56 component capacity per board
- Field-updateable status (7 status codes)
- Reserved bytes (4-6) for future features
- Creative address field usage (I2C addr / GPIO pin / PWM channel)
- Complete API: read, write, query, validate, field update
- Bus scanning for multiple EEPROMs
- Comprehensive IC name mappings
- Examples and full documentation

### Design Decisions
- Component count at byte 7 (optimal TLV-like design)
- Three reserved bytes for forward compatibility
- EEPROM self-reference as component[0] for validation
- Fixed 4-byte descriptor format (cache-friendly, easy parsing)
- Status byte at end for efficient field updates

### Categories
- **Core ICs**: RTC, GPS, IMU, Crypto, Display, Communication, USB/Serial, Motor, Temperature, Pressure, Sensor, Audio, Power, LED, I/O Expander, Memory, MCU
- **Physical Components**: Connector, Button, Battery, Actuator, Antenna

### Projects Supported
- GNSS (GPS monitoring and logging)
- Shepherd (Swarm robotics)
- MIDI (Audio hardware)

## [Unreleased]

### Added
- `24CS128` support (`MEMORY_24CS128 = 7`) with explicit per-address
  `eeprom_set_profile()`, two-byte addressing, 64-byte page splitting,
  protection checks and a part-specific self-reference macro.
- `eeprom_read_factory_id()` returns a typed 64-bit EUI for 24AA parts or
  the full 128-bit CS128 serial for use as board identity. The existing
  capabilities struct and 8-byte EUI API retain their sizes; CS128 callers
  use the new API. The manifest layout and extra storage remain unchanged.
- CS128 host simulation and tests for identity reads, mixed-bus scans,
  capacity/page boundaries, protection and failed/interrupted transactions.
- Addressable `24AA025E64` support with stable `MEMORY_24AA025E64 = 6`,
  part-specific self-reference helpers, validation, name mapping, and host tests
- Non-addressable `24AA02E64` alias simulation in the host I2C mock
- Public `eeprom_get_program_state()` API so first-boot provisioning can
  distinguish a uniformly blank EEPROM from a partial/dirty image and an I2C
  failure
- GNSS observer catalog entries for NEO-M10/F10N/F10T, ZED-F9T, BMP390,
  HDC2080, ADM7150, RT9193, and CR123A
- GNSS observer catalog entries for the MAX form factor and the mobile sensor
  group: MAX-M10S/M10N/F10S and ZED-X20P receivers, the MAX31328 TCXO clock,
  the ICM-45686 IMU, the MMC34160PJ magnetometer, the MS5607 pressure sensor,
  and CR2032/CR1220 cells

### Changed (pre-release fixes from code review; 1.0.0 was never deployed)
- EEPROM writes now deliberately use the 24AA02E64's 8-byte page size as a
  safe common denominator for both supported parts
- Bus scans return a self-described 24AA02E64 once instead of reporting the
  same physical chip at all eight aliases in `0x50`-`0x57`
- Hardware documentation now distinguishes the non-addressable 24AA02E64 from
  the A0/A1/A2-addressable 24AA025E64 and correctly describes 248 writable
  bytes plus the eight-byte factory EUI-64
- BREAKING: migrated from the deprecated legacy `driver/i2c.h` API to the
  `i2c_master` driver; `eeprom_discovery_init(bus_handle)` must now be called
  once before any bus operation (requires ESP-IDF >= 5.2)
- BREAKING: `eeprom_find_category()` now returns `const eeprom_ic_descriptor_t*`
- Component descriptors are stored at byte 16 per the documented memory map,
  and the 64-bit little-endian timestamp at bytes 8-15 is now actually
  persisted and read back (set `caps->timestamp` before writing)
- EEPROM writes are chunked to the 24AA02E64's 8-byte page buffer with ACK
  polling after each page (previously, writes of 3+ components were corrupted
  by intra-page wraparound)
- `eeprom_write_capabilities()` commits the header page (magic byte) last and
  verifies the whole image by read-back, so an interrupted write never leaves
  a valid-looking record; with `force=false`, a bus error during the guard
  check now refuses to write instead of being treated as "blank"
- `eeprom_read_capabilities()` sets `is_valid` only after every read succeeds,
  populates `reserved_footer` from bytes 240-247, and warns when the unique-ID
  read fails
- All bus-touching functions are serialized by an internal mutex (thread-safe
  after init)
- Low-level accesses are bounds-checked against the 256-byte array, and writes
  overlapping the factory unique-ID region (0xF8-0xFF) are rejected
- Fixed the `IC()` helper macro, whose `id` parameter captured the `.id`
  designator and made every `IC_*` macro fail to compile

### Added
- `eeprom_update_ic_status_at()` - address-qualified status update for boards
  carrying two identical parts (the legacy function documents first-match)
- Scan now validates each device's self-reference and warns about foreign
  EEPROMs in the 0x50-0x57 range
- Host-side unit test suite under `test/host` (mock I2C with faithful
  page-buffer semantics, fault injection, and concurrency checks)
- OVERVIEW.md architecture summary

### Removed
- `CAP_COMPONENT_SIZE` macro (unused duplicate of `CAP_BYTES_PER_IC`);
  `CAP_OFFSET_IC_LIST` is retained as a deprecated alias of
  `CAP_OFFSET_COMPONENTS`

### Planned Features
- CRC8 checksum in reserved byte 4
- Feature flags in reserved byte 5
- Extended component count in reserved byte 6
- Additional component categories as needed
- More IC IDs within existing categories
