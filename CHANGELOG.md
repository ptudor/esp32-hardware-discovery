# Changelog

Release history for ESP Hardware Discovery. The component version and Git tag
use Semantic Versioning beginning with `v1.0.0`. The earlier date-based tag
is retained as a historical snapshot.

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

### Migration from 20251231

- **ESP-IDF 5.2 or later is required.** Create an `i2c_master` bus and call
  `eeprom_discovery_init(bus_handle)` once before bus operations. The legacy
  `driver/i2c.h` setup is no longer used.
- Select the 24CS128 or M24128-U profile from known assembly information
  before scanning, reading, or provisioning. The default profile handles
  both 24AA variants.
- `eeprom_find_category()` returns a `const eeprom_ic_descriptor_t *`.
- `CAP_COMPONENT_SIZE` was removed; use `CAP_BYTES_PER_IC`.
  `CAP_OFFSET_IC_LIST` remains a deprecated alias of `CAP_OFFSET_COMPONENTS`.
- The historical implementation wrote descriptors at byte 8 instead of the
  documented byte 16. Reprovision EEPROMs written by that snapshot from the
  authoritative board manifest; there is no automatic data migration.
- Use `eeprom_read_factory_id()` for board identity across all supported
  parts. The legacy EUI API and `unique_id[8]` field remain 24AA-only.
- Existing category and part IDs and the four-byte descriptor format remain
  unchanged. Catalog additions append IDs within existing categories.

## [20251231] - 2026-01-02

Initial public source snapshot, published January 2, 2026, with the tag
`20251231` and internal component metadata version `1.0.0`. It predates the
review fixes and is superseded by the production `v1.0.0` release above.

### Added

- Four-byte IC descriptors, 23 categories, and up to 56 entries per board.
- EEPROM self-reference, project/PCB/revision fields, status tracking,
  queries, bus scanning, and IC name mappings.
- GNSS, Shepherd, and MIDI project identifiers and reference examples.
- MIT license and initial documentation.

[1.0.0]: https://github.com/ptudor/esp32-hardware-discovery/releases/tag/v1.0.0
[20251231]: https://github.com/ptudor/esp32-hardware-discovery/releases/tag/20251231
