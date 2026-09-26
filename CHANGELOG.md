# Changelog

Release history for ESP Hardware Discovery. The component version and Git tag
use Semantic Versioning beginning with `v1.0.0`. The initial snapshot is
recorded below by its original commit.

## [Unreleased]

### Added

- Microchip 24CS256 and 24CS512 EEPROM profiles (`EEPROM_PROFILE_24CS256`,
  `EEPROM_PROFILE_24CS512`), memory catalog IDs `MEMORY_24CS256` (9) and
  `MEMORY_24CS512` (10), and the `IC_EEPROM_SELF_24CS256()` and
  `IC_EEPROM_SELF_24CS512()` self-reference macros. Both share the 24CS128
  serial and configuration registers; the driver applies their 32/64 KiB
  capacity, 64/128-byte pages and 4/8 KiB enhanced-protection zones.
- Host tests for both geometries: serials at every strap, manifests, page
  splitting above 16 KiB, bounds, zone protection and a mixed-part scan.
- Power catalog ID `POWER_TPS7A20` (9) for TI's TPS7A20 GPIO-gated
  low-noise LDOs, alongside `POWER_ADM7150` and `POWER_RT9193`.
- Communication catalog ID `COMM_W5500` (6) for WIZnet's W5500 SPI Ethernet
  controller, and sensor catalog ID `SENSOR_THERMOCOUPLE_MAX31856` (16) for
  the MAX31856 SPI thermocouple converter, which is not a MAX31855.
- Board category `CAT_INTSAT` (24) with `INTSAT_NEO` (1), `INTSAT_X20` (2),
  `INTSAT_MAX` (3) and `INTSAT_CARRIER_ARDUSIMPLE` (4), and the `IC_BOARD()`
  macro. A board descriptor's address byte is the board revision (1 = A), so a
  manifest can name the board it is on, and boards stacked with it, by ID and
  revision.
- `eeprom_identify()`: identifies a 24CS128, 24CS256 or 24CS512 by its
  Manufacturer ID (reserved address `0x7C`) and an M24128-U by its
  identification-page header, without writing and without inferring a part
  from an ACK. `eeprom_profile_memory_id()` gives a profile's self-reference ID.
- `eeprom_find_board()`: the one installed entry in a board category, with its
  ID and revision, or none, or a conflict.
- `eeprom_intsat_template()` and `eeprom_intsat_options_t`: the manufacturing
  manifests of the Intsat NEO, X20 and MAX revision A boards, moved from
  navlistener-software's factory-initialization code so its firmware and the
  factory CA fixture write identical bytes. Host tests fix each released list
  byte for byte.
- Host mock of the 24CS Manufacturer ID read, including a stuck bus.

## [1.0.0] - 2026-09-21

First production source release, incorporating the reviewed implementation
and all EEPROM and catalog additions since `20251231`.

### Added

- Explicit EEPROM profiles for Microchip 24AA02E64, 24AA025E64 and 24CS128,
  and ST M24128-U, with part-specific self-reference macros.
- `eeprom_read_factory_id()` returns a typed 64-bit EUI on 24AA parts, the
  full 128-bit serial on 24CS128, or the full 128-bit UID on M24128-U.
- Two-byte addressing, 64-byte page splitting, and manifest/status readback
  for 24CS128 and M24128-U. Provisioning leaves their extra array storage
  and identity memory untouched. No permanent lock commands are issued.
- `eeprom_get_program_state()` distinguishes a uniformly blank manifest,
  an existing record, a partial/dirty image, and a bus error.
- `eeprom_update_ic_status_at()` selects a particular descriptor by address
  when a board carries two instances of the same part.
- GNSS and mobile-board catalog entries: NEO-M10/F10N/F10T, ZED-F9T,
  MAX-M10S/M10N/F10S, ZED-X20P, MAX31328, ICM-45686, MMC34160PJ,
  BMP390, MS5607, HDC2080, ADM7150, RT9193, CR123A, CR2032 and CR1220.
- 18 additional sensor and monitoring entries: LSM6DSRX, MCP9804, STS35,
  STS31A, NXP LM75A, SHT21, LM35, LM34, BMP390L, HDC2022, HIH8121,
  VCNL4200, AT42QT1070, NJL7502L, SFH 3310, ICS-43434, INA260 and LTC2990.
  The [sensor catalog](SENSORS.md) records interfaces and manufacturer sources.
- Host tests for page wrapping, address aliases, factory identities,
  protection, bus faults, interrupted transactions, and concurrent calls.
- GitHub CI for host tests, reference-example syntax, and ESP-IDF builds;
  contribution guidance and issue/pull-request templates.

### Fixed

- EEPROM page writes split at the silicon's page boundaries and use bounded
  ACK polling. The two 24AA parts share an 8-byte write chunk size.
- Descriptors now occupy byte 16 onward, matching the documented layout;
  the little-endian timestamp is persisted at bytes 8–15.
- Provisioning commits descriptors and timestamp before the header and
  verifies the result by readback. A guard read failure or partial/dirty
  image refuses a non-forced write. Forced overwrites are not atomic.
- Capability reads set `is_valid` only after required reads succeed and
  expose reserved footer bytes. Failed EUI reads are logged.
- Bus operations are serialized by an internal mutex after initialization.
- Address/capacity bounds and the 24AA factory EUI region are protected.
- Scanning reports the non-addressable 24AA02E64 once across `0x50–0x57`
  and checks EEPROM self-references.
- The `IC()` helper macro no longer substitutes its argument into the `.id`
  field designator.
- CMake selects `driver` on ESP-IDF 5.2 and `esp_driver_i2c` on 5.3 or later,
  matching the documented minimum framework version.
- Installation instructions use the GitHub release instead of an unavailable
  registry package; component metadata points to the repository documentation.

## [20251231] - 2026-01-02

Initial public source snapshot, published January 2, 2026, originally tagged
`20251231` with internal component metadata version `1.0.0`. It predates the
review fixes and is superseded by the production `v1.0.0` release above.

### Added

- Four-byte IC descriptors, 23 categories, and up to 56 entries per board.
- EEPROM self-reference, project/PCB/revision fields, status tracking,
  queries, bus scanning, and IC name mappings.
- GNSS, Shepherd, and MIDI project identifiers and reference examples.
- MIT license and initial documentation.

[Unreleased]: https://github.com/ptudor/esp32-hardware-discovery/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/ptudor/esp32-hardware-discovery/releases/tag/v1.0.0
[20251231]: https://github.com/ptudor/esp32-hardware-discovery/commit/916fbdf4d6cd71059121d39e4c1b0baff18d1c62
