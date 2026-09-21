# OVERVIEW — ESP Hardware Discovery

Architecture summary for the ESP-IDF component. User-facing documentation
lives in README.md; contributor rules live in CLAUDE.md.

## What It Does

Every PCB carries a Microchip 24AA02E64, 24AA025E64 or 24CS128 EEPROM
programmed once at manufacturing with a description of what is installed on
the board. Firmware reads it at boot and adapts to reality — PCB revisions,
substitute parts, unpopulated footprints, and field failures — without
compile-time configuration.

## Memory Layout (256 bytes, canonical)

```
Header (16 bytes)
  0        Magic (1–254; production uses 79. 0x00 = unprogrammed, 0xFF = erased)
  1        Project ID
  2        PCB ID
  3        Revision
  4–6      Reserved (0; future: CRC8, feature flags, extended count)
  7        Component count N (max 56)
  8–15     64-bit Unix timestamp, little-endian (when programmed)

Components (224 bytes)
  16–239   N × 4-byte IC descriptors: [category][id][address][status]

Footer (16 bytes)
  240–247  Reserved (surfaced to callers as reserved_footer)
  248–255  Factory-programmed 8-byte unique ID (read-only silicon)
```

On 24CS128 the same manifest occupies bytes 0–247. Its factory serial is
16 bytes at security word address `0x0800`, read through the additional
interface at main I2C address + 8. Main-array bytes 248–16383 are untouched.
Use `eeprom_read_factory_id()` for board identity: it returns a typed
64-bit EUI or full 128-bit serial. The RTC EUI is a separate component ID.

The descriptor's address byte is polymorphic: I2C address for I2C parts,
GPIO pin for buttons/LEDs, PWM channel for actuators, 0 for SPI/UART/other.

**Layout invariants** (boards in the field depend on these): descriptor byte
order, header bytes 0–7, the unique-ID region, and all existing enum values
are frozen. New ICs/categories/projects append with the next available ID.

## Module Structure

```
include/esp_hardware_discovery.h   Public API: layout macros, enums, structs, prototypes
src/esp_hardware_discovery.c       Implementation (single translation unit)
test/host/                         Host tests against all EEPROM profiles
esp_hardware_discovery_examples.c  Reference examples (not compiled into the component)
```

The implementation is layered:

1. **Module state** — `eeprom_discovery_init()` stores the `i2c_master` bus
   handle, creates the mutex, and lazily adds one device handle per EEPROM
   address (0x50–0x57, plus CS128 security interfaces at 0x58–0x5F).
   `eeprom_set_profile()` selects CS128 explicitly before bus operations;
   the default is the shared 24AA protocol. Everything below refuses to run
   before init.
2. **Low-level I/O** (static, callers hold the lock) — `eeprom_write_bytes()`
   chunks writes at the 8-byte common denominator (24AA02E64 pages are 8 bytes;
   24AA025E64 pages are 16), or 64-byte pages with two-byte addressing on
   CS128. It ACK-polls each chunk until the write cycle (Twc ≤ 5 ms)
   completes. Reads are sequential and unchunked. Bounds use each part's
   capacity; 24AA writes reject the EUI region. CS128 writes first inspect
   software protection. Its security/configuration interfaces are read-only
   in this driver; no lock commands are issued.
3. **Unlocked internals** — read/write/update logic shared by the public API.
   `eeprom_write_capabilities()` commits descriptors and timestamp first, the
   header page carrying the magic byte last, then verifies by read-back: an
   interrupted programming run never yields a valid-looking board.
4. **Public API** — thin wrappers that take the mutex, making every
   bus-touching call thread-safe. Pure query helpers (`eeprom_has_ic`,
   `eeprom_count_category`, `eeprom_find_category`, printing/naming) operate
   on the caller's struct and need neither init nor lock.

## Key Conventions

- `components[0]` is **always** the EEPROM itself, using the part-specific
  `IC_EEPROM_SELF_24AA02E64(addr)`, `IC_EEPROM_SELF_24AA025E64(addr)` or
  `IC_EEPROM_SELF_24CS128(addr)` macro.
  `eeprom_validate_self_reference()` checks category, supported part ID,
  address, and status; `eeprom_scan_bus()` warns when a device in the EEPROM
  address range lacks it (likely a foreign part).
- The 24AA02E64 aliases all of `0x50`-`0x57`; scans record it once and stop.
  The 24AA025E64 and CS128 main arrays respond at their strapped addresses.
  Scans do not enumerate CS128 security interfaces as additional boards.
- Iterate components from index 1; index 0 is the EEPROM.
- A failed guard read is never "blank": `eeprom_write_capabilities(force=false)`
  aborts on bus errors rather than risk overwriting a programmed board. Blank
  also requires a uniform 0xFF or 0x00 manifest region (bytes 0–247); a partial/dirty image
  is refused.
- `eeprom_update_ic_status()` updates the first (category, id) match;
  `eeprom_update_ic_status_at()` also matches the address byte for boards
  carrying duplicate parts.

## Testing

`cd test/host && make` builds the implementation against mock ESP-IDF headers
and simulated addressable/non-addressable EEPROM profiles that faithfully
model page-buffer wraparound, address aliasing, two-byte CS128 addressing,
security serials, protection, write-cycle busy NACKs, fault injection, and
bus-concurrency violations — no ESP-IDF or hardware required.
`make syntax-examples` checks the examples file. On-target smoke test:
`idf.py build` against ESP-IDF ≥ 5.2.
