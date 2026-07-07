# Code Review — esp32-hardware-discovery

**Date:** 2026-07-06
**Scope:** Every file in the repository at commit `916fbdf` (Initial commit).
**Files reviewed:** `include/esp_hardware_discovery.h`, `src/esp_hardware_discovery.c`, `esp_hardware_discovery_examples.c`, `CMakeLists.txt`, `idf_component.yml`, `README.md`, `CHANGELOG.md`, `CLAUDE.md`, `LICENSE.txt`.

**Hard constraints on all fixes** (from CLAUDE.md): the on-chip EEPROM layout of already-programmed boards must not change without a migration path; the 4-byte descriptor format is fixed; existing enum values must not change; no public function may be removed; existing logging must be preserved.

---

## Critical

### R-001 — Multi-byte EEPROM writes violate the 24AA02E64's 8-byte page buffer, corrupting all boards written with ≥3 components

- **Severity:** Critical (data corruption on the primary manufacturing path)
- **Location:** `src/esp_hardware_discovery.c:30-52` (`eeprom_write_bytes`), called from `eeprom_write_capabilities` at `src/esp_hardware_discovery.c:282-283`
- **Problem:** The 24AA02E64 has an **8-byte page write buffer**. A single I2C write transaction may carry at most 8 data bytes, and all of them must fall inside one 8-byte page (addresses `N*8 … N*8+7`). If more bytes are sent, the device's internal address pointer **wraps around within the current page** and later bytes overwrite earlier ones — silently. `eeprom_write_bytes` issues one I2C transaction for the entire buffer regardless of length. `eeprom_write_capabilities` calls it with `component_count * 4` bytes (up to 224) starting at offset 8.
  - The header write (8 bytes at offset 0) is safe only by accident: it is exactly one page, page-aligned.
  - The IC-descriptor write is safe only for `component_count ≤ 2` (≤ 8 bytes filling page 8–15 exactly). For 3+ components the transaction is >8 bytes: the EEPROM keeps only the last 8 bytes (modulo wraparound) of the stream in page 8–15 and never touches pages 16+. Every descriptor beyond the wrapped page is lost, and the first page holds wrapped garbage.
  - The header's own count byte says N components, so reads will happily parse whatever bytes happen to be at offsets 8–231 (mostly `0xFF` erased state or wrapped garbage) as descriptors.
- **Evidence:**
  ```c
  // src/esp_hardware_discovery.c:282 — one transaction, up to 224 bytes:
  ret = eeprom_write_bytes(i2c_addr, CAP_OFFSET_IC_LIST, ic_data, ic_data_len);
  ```
  `eeprom_write_bytes` (lines 36-45) builds a single start/addr/data/stop sequence with no chunking. The Microchip 24AA02E64 datasheet (DS20002124, "Page Write") specifies the 8-byte page buffer and the intra-page wraparound behavior. README.md:275 even states "Write Time: 5ms **per page**", acknowledging the page model the code ignores. The example board (`esp_hardware_discovery_examples.c:25`, 6 components = 24 bytes) exceeds the limit 3×.
- **Fix specification:**
  - Inside `eeprom_write_bytes` (or a new static helper it delegates to), split any write into chunks such that each I2C transaction (a) carries ≤ 8 data bytes and (b) does not cross an 8-byte page boundary — i.e., the first chunk length is `min(len, 8 - (mem_addr % 8))`, subsequent chunks up to 8 bytes each.
  - After **each** chunk, wait for the write cycle: either keep the existing `vTaskDelay(pdMS_TO_TICKS(5))` per chunk, or (better) implement ACK polling (retry the address byte until the device ACKs, with a ~6 ms timeout). ACK polling is preferred — it is faster and robust across temperature/voltage.
  - Must NOT change: the function's signature, the byte values/offsets written to the chip (only the transaction chunking changes), the public API, or the read path (sequential reads are unaffected by page size and are correct as-is).
  - Edge cases: `len == 0` already rejected; unaligned start addresses (e.g. `eeprom_update_ic_status` writes 1 byte at arbitrary offset — already ≤8 and within a page, but must still work through the new chunking); a chunk failure mid-sequence must abort and return the error (see R-004 for ordering implications).
- **Verification:** On hardware: program a board with ≥3 components (e.g. the 6-component example), read back all 232 bytes raw, and compare byte-for-byte against the intended image; then `eeprom_read_capabilities` + `eeprom_print_capabilities` must show all 6 descriptors with correct category/id/addr/status. Without hardware: unit-test the chunking helper (host build with a mock I2C layer) asserting every emitted transaction has ≤8 bytes and `start_addr/8 == end_addr/8`.

---

### R-002 — On-chip layout contradicts the documented memory map; the timestamp field is never stored or read (dead feature)

- **Severity:** Critical (data-format ambiguity across firmware/tools; silently dead documented feature)
- **Location:**
  - `include/esp_hardware_discovery.h:46-81` (memory-map comment and offset macros — `CAP_OFFSET_TIMESTAMP 8`, `CAP_OFFSET_COMPONENTS 16`, `CAP_OFFSET_IC_LIST 8`)
  - `src/esp_hardware_discovery.c:181, 282, 408` (all component I/O uses `CAP_OFFSET_IC_LIST` = 8)
  - `src/esp_hardware_discovery.c:129-210` (`eeprom_read_capabilities` — never reads bytes 8-15 as timestamp), `212-295` (`eeprom_write_capabilities` — never writes a timestamp)
  - `README.md:143-162` (documents the byte-16 layout), `src/esp_hardware_discovery.c:789-798` (prints `caps->timestamp`)
- **Problem:** The header comment, README, and macros document this layout: header 0–7, **64-bit timestamp at bytes 8–15**, components at **bytes 16–239**. The code implements a different one: components start at **byte 8** (`CAP_OFFSET_IC_LIST`), and no timestamp exists on-chip at all. Two macros share offset 8 with contradictory meanings (`CAP_OFFSET_TIMESTAMP` and `CAP_OFFSET_IC_LIST`), and `CAP_OFFSET_COMPONENTS` (16) is never used. Consequences:
  1. Any external tool, future firmware, or human following the documented layout will misread/miswrite every board programmed by this code (each descriptor shifted by 8 bytes = 2 descriptor slots; the first two "timestamp" reads are actually descriptors).
  2. `eeprom_capabilities_t.timestamp` is populated only by `memset(0)` on read, so `eeprom_print_capabilities` always prints "[timestamp not set]" — the documented "when programmed" feature does not exist. The `%llu` print branch at line 795 is dead code.
  3. `CAP_MAX_COMPONENTS 56` is derived from the documented layout (`(240-16)/4`); under the actual layout the descriptors end at byte 232, leaving bytes 232–239 silently unused.
- **Evidence:**
  ```c
  #define CAP_OFFSET_TIMESTAMP    8    // header, line 71: "64-bit Unix timestamp (8 bytes)"
  #define CAP_OFFSET_COMPONENTS   16   // header, line 72: "Start of component array" — never referenced
  #define CAP_OFFSET_IC_LIST      8    // header, line 81 — what the code actually uses
  ```
  `grep CAP_OFFSET_COMPONENTS src/` returns nothing; `grep CAP_OFFSET_IC_LIST` matches all three component I/O sites.
- **Fix specification:** This is an **owner decision**, not a mechanical fix, because CLAUDE.md forbids layout changes for boards already in the field. **Needs investigation first:** determine whether any physical board was programmed (a) by this code — layout = components at 8, no timestamp; (b) by other tooling following the documented layout; or (c) not at all (repo is a single initial commit — if no boards exist, either layout may be canonicalized). Then pick exactly one:
  - **Option A (no field boards, or willing to reprogram):** make the code match the documentation — component I/O moves to `CAP_OFFSET_COMPONENTS` (16); `eeprom_write_capabilities` writes `caps->timestamp` little-endian (pick and document endianness!) at bytes 8–15; `eeprom_read_capabilities` reads it back; delete `CAP_OFFSET_IC_LIST` from the .c usage (the macro itself is public — keep it defined but repoint/deprecate it, don't remove).
  - **Option B (field boards exist with the code's layout):** the code is canonical — rewrite the header comment, README.md:143-162, and macros so the documented map says components at byte 8, no on-chip timestamp; remove the timestamp print path or repurpose it explicitly; either repurpose bytes 232–239 as additional reserved space or raise `CAP_MAX_COMPONENTS` to 58 (a value change downstream code may depend on — safer to keep 56 and document 232–239 as reserved).
  - Must NOT change in either option: descriptor byte order `[cat][id][addr][status]`, header bytes 0–7, unique-ID region 248–255, enum values, public function signatures. `eeprom_update_ic_status`'s status-byte offset arithmetic (line 408) must be updated in lockstep with whichever component offset is chosen.
- **Verification:** After the fix, `grep -n 'CAP_OFFSET'` shows exactly one offset used for component I/O and it matches the memory-map comment and README byte-for-byte. If Option A: program a board, power-cycle, and confirm the printed timestamp matches the programming time; hexdump bytes 8–15 and confirm the encoded value. If Option B: confirm README/header show the byte-8 layout and that a board programmed before the fix still reads identically after it.

---

## High

### R-003 — Transient I2C failure defeats the overwrite guard: `eeprom_write_capabilities(force=false)` can clobber a programmed EEPROM

- **Severity:** High (data loss on production boards)
- **Location:** `src/esp_hardware_discovery.c:107-116` (`eeprom_is_programmed`), used as guard at `src/esp_hardware_discovery.c:221-225`
- **Problem:** `eeprom_is_programmed` returns `false` both when the magic byte is outside 1–254 **and when the I2C read itself fails** (line 111-113). The `force=false` guard in `eeprom_write_capabilities` exists precisely to prevent accidental overwrite of a programmed board — but a transient bus error (noise, contention, marginal pull-ups, a busy device mid-write-cycle) makes the guard evaluate "not programmed" and the function proceeds to overwrite. The whole purpose of the safety check is defeated by the failure mode it is most likely to encounter in the field.
- **Evidence:**
  ```c
  esp_err_t ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_MAGIC, &magic, 1);
  if (ret != ESP_OK) {
      return false;          // ← indistinguishable from "blank EEPROM"
  }
  ```
- **Fix specification:** Inside `eeprom_write_capabilities`, do not rely on the boolean. Read the magic byte directly (or via a new *static* tri-state helper: programmed / blank / bus-error). On bus error, log ESP_LOGE and return `false` **without writing** — never treat a failed read as permission to write. The public `eeprom_is_programmed` signature and semantics must not change (downstream projects call it); only the internal guard logic changes. Edge case: an erased EEPROM reads `0xFF` (`CAP_MAGIC_ERASED`) and a genuinely blank check must still allow writing.
- **Verification:** With a mock/fault-injected I2C layer, make the guard read return `ESP_ERR_TIMEOUT` and assert `eeprom_write_capabilities(addr, &caps, false)` returns false and issues no write transactions. On hardware: disconnect SDA, call write with `force=false`, confirm it errors out cleanly.

### R-004 — Non-atomic write ordering: header (magic + count) is committed before the descriptors, so a failed write leaves a valid-looking but garbage record

- **Severity:** High (data-integrity: partial writes are indistinguishable from good boards)
- **Location:** `src/esp_hardware_discovery.c:245-291` (`eeprom_write_capabilities`)
- **Problem:** The function writes the 8-byte header first (magic=79, component_count=N), then the descriptor array. If the descriptor write fails (bus glitch, power loss mid-programming, or the R-001 page bug), the EEPROM is left with a **valid magic and a nonzero count** pointing at unwritten/garbage descriptor bytes. Every subsequent boot passes `eeprom_is_programmed`, `eeprom_read_capabilities` returns `true`, and firmware "discovers" garbage hardware. There is also no read-back verification after programming, so manufacturing has no signal that the write took.
- **Evidence:** Lines 257-261 write the header and only then lines 282-291 write descriptors; the descriptor-failure path (line 287-290) returns false but leaves the committed header in place.
- **Fix specification:** Reverse the commit order: write the descriptor array first, then the header last — the magic byte is the commit record. Better still, within the header write make it the true last byte committed (with R-001's chunking, the header is one 8-byte page, so writing it last is sufficient; magic is byte 0 of that page). Optionally (recommended for a manufacturing path): after all writes, read back header + descriptors and memcmp against the intended image, returning false on mismatch — log with ESP_LOGE. Must NOT change: byte layout, public signature, the `force` semantics, existing log messages (add, don't remove). Edge case: a *reprogram* (`force=true`) of an already-programmed board briefly has old-header/new-descriptors if interrupted — acceptable and strictly better than valid-header/garbage-descriptors, but worth a note in the function's doc comment; a fully safe variant first zeroes the magic byte, writes descriptors, then writes the final header.
- **Verification:** Fault-inject a failure in the descriptor write (mock I2C, or pull the bus mid-write): `eeprom_is_programmed` must return false afterwards (magic not yet committed). Then complete a clean write and confirm read-back matches.

### R-005 — `esp_hardware_discovery_examples.c` does not compile: wrong header, undefined identifiers, missing includes

- **Severity:** High (the shipped reference code is broken; users copy examples verbatim)
- **Location:** `esp_hardware_discovery_examples.c:6, 224-225, 391-406`
- **Problem / Evidence:** The file is not in `CMakeLists.txt` `SRCS`, so the component builds — but anyone compiling the examples (as README.md:292 invites) hits:
  1. Line 6: `#include "eeprom_24aa02e64.h"` — no such header exists; must be `esp_hardware_discovery.h`. (Stale name from a pre-rename version of the library; the `@file eeprom_examples.c` doc comment at line 2 is also stale.)
  2. Lines 224-225: `PROJECT_GALMON` and `GALMON_PCB_MAIN` are not defined anywhere. The enums define `PROJECT_GNSS` / `GNSS_PCB_MAIN` — evidently the project was renamed and the example wasn't updated.
  3. Lines 397-405: `vTaskDelay` / `pdMS_TO_TICKS` used without including `freertos/FreeRTOS.h` and `freertos/task.h`.
  4. Line 391: the file defines `app_main`, so it can never be added to the component's `SRCS` without colliding with the consuming application. It behaves like an example *project* file but lives at the component root.
- **Fix specification:** Fix the include to `esp_hardware_discovery.h`; replace `PROJECT_GALMON`→`PROJECT_GNSS` and `GALMON_PCB_MAIN`→`GNSS_PCB_MAIN`; add the FreeRTOS includes; update the stale `@file` name. Structurally, the idiomatic ESP-IDF shape is `examples/<name>/main/main.c` with its own CMakeLists so `idf.py build` can compile it in CI — recommended but optional; if the file stays at the root, state in a comment that it is reference-only. Must NOT change: the example semantics (they document the intended API usage). Also fix the convention violation inside it — see R-010.
- **Verification:** Compile the file (either moved into an example project and `idf.py build`, or `gcc -fsyntax-only` with ESP-IDF include stubs). Zero errors, zero implicit-declaration warnings.

---

## Medium

### R-006 — Partial read failure leaves `caps->is_valid = true` with a nonzero count and zeroed components

- **Severity:** Medium (data-integrity for callers that inspect the struct instead of the return value)
- **Location:** `src/esp_hardware_discovery.c:162` (sets `is_valid = true`), `193-197` (descriptor read fails → `return false` with `is_valid` still true)
- **Problem:** `is_valid` is set before the component descriptors are read. If the descriptor read fails, the function returns `false` but the out-struct says `is_valid == true`, `component_count == N`, `components[] == all zeros`. Any caller that checks the struct (the query helpers `eeprom_has_ic`/`eeprom_count_category`/`eeprom_find_category` gate on `caps->is_valid`, not on the read's return value) will treat N zeroed descriptors as real data; `eeprom_print_capabilities` prints N rows of "Unknown". The struct's own validity flag lies about the struct's state.
- **Evidence:** Line 162 `caps->is_valid = true;` precedes the `eeprom_read_bytes(..., CAP_OFFSET_IC_LIST, ...)` call at 181; the failure branch at 193-197 does not reset it.
- **Fix specification:** Set `is_valid = true` only once *everything* the function reads has succeeded (i.e., immediately before the final `return true`), or explicitly set `caps->is_valid = false;` in the descriptor-failure branch. The magic-validation failure path (line 156-160) already sets it false — keep that. Must NOT change: the function's return-value semantics or the struct layout.
- **Verification:** Unit test with a mock I2C layer: header read succeeds, descriptor read fails → assert return is false AND `caps.is_valid == false`.

### R-007 — No concurrency protection: `eeprom_update_ic_status` is a read-modify-write with no mutual exclusion, and no thread-safety contract is documented

- **Severity:** Medium (race condition; low probability today, silent corruption when it hits)
- **Location:** `src/esp_hardware_discovery.c:379-421` (`eeprom_update_ic_status`); the module generally (no mutex anywhere)
- **Problem:** ESP-IDF's legacy I2C driver serializes individual *transactions* per port, but nothing serializes this module's *logical operations*. `eeprom_update_ic_status` reads all capabilities, searches, then writes one status byte — if two tasks call it concurrently (e.g. two component-init tasks each marking their device FAILED, as `initialize_hardware_from_eeprom` in the examples does from a loop), or one task calls it while another runs `eeprom_write_capabilities(force=true)`, the interleaving can compute the status-byte offset against a stale component table or read a half-written table. Nothing in the header documents whether the API is thread-safe, so callers can't know they must serialize.
- **Evidence:** No `SemaphoreHandle_t`/mutex in `src/esp_hardware_discovery.c`; `eeprom_update_ic_status` performs read (line 383) → search (391-398) → write (410) as three separate bus operations.
- **Fix specification:** Two acceptable resolutions; pick one and be explicit: (a) add a module-level FreeRTOS mutex (created lazily or via an init function — note an init function is a public-API addition, allowed; removal isn't) taken around every public function's bus sequence; or (b) document loudly in the header that the API is NOT thread-safe and callers must serialize access. Given the component's simplicity and CLAUDE.md's "fail gracefully" philosophy, (a) with a `static SemaphoreHandle_t` + `pthread_once`-style lazy init is the production-grade answer. Must NOT change: public signatures; behavior for the single-task case.
- **Verification:** For (a): two tasks hammering `eeprom_update_ic_status` on different ICs for 1000 iterations; read back and confirm both final statuses are correct. For (b): header text present; grep for the warning in generated docs.

### R-008 — I2C port hardcoded to `I2C_NUM_0`

- **Severity:** Medium (portability/correctness on multi-bus designs)
- **Location:** `src/esp_hardware_discovery.c:43, 80, 96` (every `i2c_master_cmd_begin(I2C_NUM_0, ...)`)
- **Problem:** Every transaction assumes the EEPROM hangs off port 0. Boards that put the ID EEPROM on `I2C_NUM_1` (common when port 0 is dedicated to a high-rate sensor) cannot use this component at all, and nothing in the header or README states the assumption. For a component whose entire purpose is adapting to hardware diversity, the bus itself is the one thing it refuses to adapt to.
- **Fix specification:** Add a module-level configuration — either a Kconfig option (`CONFIG_ESP_HW_DISCOVERY_I2C_PORT`, default 0) or a public setter `void eeprom_discovery_set_i2c_port(i2c_port_t port)` defaulting to `I2C_NUM_0`. Do NOT change existing function signatures (adding an `i2c_port` parameter to every call would break all downstream users). Store in a `static i2c_port_t` read by the three low-level functions. Document the default in the header and README's "Initialize I2C" section.
- **Verification:** Build for a target with two I2C ports, set port 1, confirm transactions go out on port 1 (logic analyzer or mock); default path unchanged.

### R-009 — Built on the legacy `driver/i2c.h` API, deprecated since ESP-IDF 5.2

- **Severity:** Medium (deprecation warnings now; removal in IDF 6 breaks the component)
- **Location:** `src/esp_hardware_discovery.c:11` and all of lines 30-101; README.md:38-49 teaches the legacy init
- **Problem:** `idf_component.yml` claims `idf: ">=5.0.0"` with no upper bound. From IDF 5.2 the legacy driver emits deprecation warnings (projects building with warnings-as-errors, common in production, will fail unless they suppress), and the legacy driver is slated for removal in IDF 6 — at which point every downstream project breaks on upgrade. The new `i2c_master.h` API also changes the ownership model (bus/device handles), which affects R-008's design.
- **Fix specification:** Migrate the three low-level functions to the `i2c_master.h` API (`i2c_master_bus_handle_t` + `i2c_master_dev_handle_t`, `i2c_master_transmit`, `i2c_master_transmit_receive`, `i2c_master_probe`). This forces a decision about who owns the bus handle: recommended shape is a public one-time init `esp_err_t eeprom_discovery_init(i2c_master_bus_handle_t bus)` — an API *addition* (allowed), with existing functions returning an error and ESP_LOGE if called before init. Bump `idf` dependency to `>=5.2.0` (or keep 5.0 support behind an `#if ESP_IDF_VERSION` fork — more code, only worth it if a downstream project is pinned to 5.0/5.1 — **needs investigation**: check what IDF versions the GNSS/Shepherd/MIDI projects actually pin). Coordinate with R-007 (the new driver is also not thread-safe per device) and R-008 (bus handle subsumes the port question). Must NOT change: public read/write/query function signatures or on-chip layout.
- **Verification:** `idf.py build` on IDF 5.2+ with default warning flags → zero deprecation warnings; hardware round-trip test (program + read back) still passes.

### R-010 — Flagship example violates the component's own core convention: no EEPROM self-reference at `components[0]`

- **Severity:** Medium (teaches users a pattern that fails validation; contradicts every doc in the repo)
- **Location:** `esp_hardware_discovery_examples.c:30-35` (Example 1), `219-233` (Example 6), `71` (Example 2's loop)
- **Problem:** The header (line 12: "CRITICAL CONVENTION"), README ("**Always** make component[0] the EEPROM itself"), and CLAUDE.md all mandate `components[0] = IC_EEPROM_SELF(...)`. Example 1 — the manufacturing example users will copy — puts the RTC at `components[0]` and omits the self-reference entirely. Any board programmed from that example permanently fails `eeprom_validate_self_reference()` (component[0] is CAT_RTC, not CAT_MEMORY), and since the EEPROM is field hardware, that mistake is baked into silicon. Example 6 repeats it. Example 2 then iterates `for (i = 0; ...)` instead of the documented "start at 1", initializing the EEPROM itself as if it were a peripheral.
- **Fix specification:** In Example 1: insert `caps.components[0] = IC_EEPROM_SELF(EEPROM_I2C_ADDR_0);`, shift the six devices to indices 1–6, set `component_count = 7`, and add an `eeprom_validate_self_reference(&verify)` call to the verify block. Same treatment for Example 6 (count 3→4). In Example 2, start the init loop at `i = 1` with the standard comment. Must NOT change: the validation function's logic (it is correct; the examples are wrong).
- **Verification:** After R-005 makes the file compile, run Example 1 against hardware (or mock): `eeprom_validate_self_reference` returns true on the read-back.

---

## Low

### R-011 — `eeprom_scan_bus` treats any ACKing device at 0x50–0x57 with a plausible byte 0 as a programmed 24AA02E64 — *Needs investigation*

- **Severity:** Low
- **Location:** `src/esp_hardware_discovery.c:297-327`
- **Problem:** Addresses 0x50–0x57 are the standard address block for *all* 24-series EEPROMs, FRAMs, and (notably) DDR SPD EEPROMs. The scan's only authenticity check is `eeprom_is_programmed` — byte 0 ∈ 1–254 — which nearly any programmed foreign EEPROM satisfies. The resulting `eeprom_capabilities_t` is garbage parsed as descriptors, with `is_valid = true`. `eeprom_validate_self_reference` exists precisely to catch this, but `eeprom_scan_bus` doesn't call it.
- **Evidence:** Lines 310-321: `probe → is_programmed → read_capabilities → found++`, no self-reference check.
- **Fix specification:** Decide the intended strictness (owner call). Recommended: inside the scan, after a successful read, call `eeprom_validate_self_reference`; on failure either skip the device or keep it but log ESP_LOGW ("device at 0x%02X has no valid self-reference — foreign EEPROM?"). Skipping is a behavior change to a public function — if downstream projects scan boards that predate the self-reference convention, skipping would hide their boards; hence "needs investigation": check whether all field boards carry the self-reference. Must NOT change: the function signature or return-value meaning (count of filled entries).
- **Verification:** Bench test with a foreign EEPROM (any 24LC02 with non-zero byte 0) on the bus: scan output matches the chosen policy.

### R-012 — Unique-ID read failure silently ignored in `eeprom_read_capabilities`

- **Severity:** Low
- **Location:** `src/esp_hardware_discovery.c:203`
- **Problem:** `eeprom_read_unique_id(i2c_addr, caps->unique_id);` — return value discarded. On failure the UID stays all-zeros from the `memset`, indistinguishable from a real (impossible, but unverifiable) zero UID. Anything keying off the UID (asset tracking, crypto seeding per the footer comment) silently gets `00:00:...:00`.
- **Fix specification:** Check the return; on failure log `ESP_LOGW(TAG, "Failed to read unique ID from 0x%02X", i2c_addr)` and continue (the read as a whole should still succeed — UID is supplementary). Must NOT change: return-value semantics of `eeprom_read_capabilities`.
- **Verification:** Mock the UID read to fail: capabilities read returns true, warning logged, UID zeroed.

### R-013 — `eeprom_find_category` silently casts away `const`

- **Severity:** Low (API contract violation; enables accidental UB)
- **Location:** `src/esp_hardware_discovery.c:372`; prototype at `include/esp_hardware_discovery.h:512`
- **Problem:** The function takes `const eeprom_capabilities_t *` but returns a **mutable** `eeprom_ic_descriptor_t*` into that const object via an explicit cast. A caller holding a genuinely const/rodata capabilities struct can now write through the pointer — undefined behavior — and the compiler can't warn.
- **Fix specification:** The clean fix is returning `const eeprom_ic_descriptor_t*`, but that changes the public API and would break downstream callers that write through it (the "old stock" example reads only). Since removing/changing public API needs owner permission per CLAUDE.md: preferred is changing the return type to const in the next minor version with a changelog note; the no-break alternative is taking a non-const `eeprom_capabilities_t*` parameter. Flag for owner decision; do not do silently.
- **Verification:** Downstream projects compile without new warnings after the chosen change.

### R-014 — No bounds check on `mem_addr + len` in low-level read/write (256-byte device, address counter wraps)

- **Severity:** Low (all current callers stay in bounds; a future caller silently corrupts byte 0+)
- **Location:** `src/esp_hardware_discovery.c:30-52, 57-85`
- **Problem:** `mem_addr` is `uint8_t` and the device is 256 bytes, so `mem_addr + len > 256` doesn't fault — the device address counter rolls over and a long write would wrap onto the header/magic region. Nothing guards against a bad `component_count`-derived length or future misuse.
- **Fix specification:** At entry of both functions: `if ((size_t)mem_addr + len > EEPROM_24AA02E64_SIZE) return ESP_ERR_INVALID_SIZE;`. Note the write path can also never legally touch 248–255 (factory UID, write-protected) — optionally reject writes overlapping `EEPROM_UNIQUE_ID_START`. Static functions, so no API impact.
- **Verification:** Unit test: `eeprom_write_bytes(addr, 250, buf, 10)` returns `ESP_ERR_INVALID_SIZE` without issuing a transaction.

### R-015 — Return values of `i2c_master_*` command-link builders unchecked

- **Severity:** Low
- **Location:** `src/esp_hardware_discovery.c:36-41, 63-78, 91-94`
- **Problem:** `i2c_cmd_link_create` can return NULL and every `i2c_master_write_byte`/`i2c_master_read`/`i2c_master_start` can return `ESP_ERR_NO_MEM` (static command buffer exhausted — realistic for the 224-byte read, which needs a large command link). Passing a NULL cmd handle into subsequent calls / `i2c_master_cmd_begin` is undefined; a failed `write` link silently truncates the transaction.
- **Fix specification:** Check `i2c_cmd_link_create()` for NULL (return `ESP_ERR_NO_MEM`); either check each builder call or use `i2c_cmd_link_create_static` with a sized buffer. Note this whole block dissolves if R-009 (driver migration) is done — do R-009 instead if it's accepted, and skip this.
- **Verification:** Code inspection + build; optionally force allocation failure via heap tracing.

### R-016 — Format-specifier mismatches (`%d` for `size_t`)

- **Severity:** Low (compiles today on xtensa/riscv32 where `size_t`==`unsigned int`, but trips `-Werror=format` in strict downstream builds)
- **Location:** `src/esp_hardware_discovery.c:177, 269`
- **Problem / Fix:** `ESP_LOGE(TAG, "Failed to allocate %d bytes...", ic_data_len)` passes `size_t` to `%d`. Use `%zu` (or `%u` with a cast). Also line 795's `%llu` for `uint64_t` is correct on ESP-IDF but `PRIu64` from `<inttypes.h>` is the portable idiom — change opportunistically only.
- **Verification:** Build with `-Wformat -Werror=format` clean.

### R-017 — `eeprom_update_ic_status` updates only the first matching (category, id); duplicates are silent

- **Severity:** Low (behavioral gap; boards with two identical parts exist by design)
- **Location:** `src/esp_hardware_discovery.c:391-398`
- **Problem:** The dual-RTC example proves boards can carry two ICs of the same category; two of the *same* chip (same category+id, different addresses — e.g. two MCP9808 at 0x18/0x19) is equally legal. `eeprom_update_ic_status` stops at the first match, so marking the second one FAILED silently updates the wrong descriptor. No overload exists that disambiguates by address, and the limitation is undocumented.
- **Fix specification:** Minimum: document the first-match behavior in the header prototype comment. Better: add (don't replace) `eeprom_update_ic_status_at(uint8_t i2c_addr, uint8_t category, uint8_t id, uint8_t ic_address, uint8_t new_status)` matching category+id+address. Existing function's behavior must not change (public API).
- **Verification:** Unit test with two identical descriptors at different addresses: the address-qualified call updates the intended one; the legacy call still updates the first.

### R-018 — Dead struct fields and orphaned macros mislead readers about what is persisted

- **Severity:** Low (maintainability; masks R-002)
- **Location:** `include/esp_hardware_discovery.h:440` (`timestamp` — see R-002), `447` (`reserved_footer[8]` — never read or written anywhere), `72` (`CAP_OFFSET_COMPONENTS` — unused), `76` (`CAP_COMPONENT_SIZE` — duplicate of `CAP_BYTES_PER_IC`, unused), `78` (`CAP_OFFSET_FOOTER` — unused; its comment says "8 bytes" while the layout comment says the footer region is 16)
- **Problem:** `reserved_footer` implies bytes 240–247 are surfaced to callers; no code touches them — the field is always zero. The unused/duplicate macros are exactly how R-002's two-layouts situation stayed invisible.
- **Fix specification:** Resolve after R-002 picks the canonical layout. Then either implement `reserved_footer` population in `eeprom_read_capabilities` (one extra 8-byte read at `CAP_OFFSET_FOOTER`) or comment the field explicitly as "not populated by reads; reserved". Macros are public API — do not delete without owner sign-off; at minimum fix the `CAP_OFFSET_FOOTER` comment and mark orphans as deprecated/informational. Struct layout must not change (downstream ABI).
- **Verification:** `grep -n` each macro/field name: every remaining one is either used by code or carries a comment stating it is informational.

### R-019 — `CMakeLists.txt`: `driver` should be a private dependency

- **Severity:** Low
- **Location:** `CMakeLists.txt:5`
- **Problem:** `REQUIRES driver` makes `driver` a *public* dependency, propagating its include dirs to every consumer — but the public header includes only `<stdint.h>/<stdbool.h>`; only the .c file touches `driver/i2c.h`. Harmless today (consumers need `driver` anyway to init the bus), but it's the wrong declaration and matters if R-009's migration changes which driver component is needed.
- **Fix specification:** Change to `PRIV_REQUIRES driver`. No other change. (If R-009 introduces `i2c_master_bus_handle_t` into the public header, it flips back to `REQUIRES esp_driver_i2c` — decide together with R-009.)
- **Verification:** `idf.py build` of a consuming project succeeds.

### R-020 — Documentation inaccuracies: capacity "60" vs 56, phantom deliverables, missing OVERVIEW.md, name mismatches

- **Severity:** Low (docs; several actively mislead)
- **Location / Evidence:**
  1. `README.md:10` — "**60 IC capacity**: 240 bytes for component data" and `CHANGELOG.md:15` — "60 component capacity". Actual: `CAP_MAX_COMPONENTS = 56`, and the component region is 224 bytes documented / 224-usable-of-232 actual (see R-002). 60 appears to be stale from an earlier layout.
  2. `CHANGELOG.md:18-19` — claims "Manufacturing programmer with LED feedback" and "Self-provisioning module for first-boot auto-configuration". Neither exists anywhere in the repo.
  3. `CLAUDE.md` says "See OVERVIEW.md for complete architecture" and lists it in File Organization; **OVERVIEW.md does not exist**. README's file-structure block (line 284-299) omits it too.
  4. Naming drift: repo dir `esp32-hardware-discovery`, registry name `ptudor/esp_hardware_discovery` (README:21), manual-install clone URL `github.com/ptudor/esp_hardware_discovery.git` (README:27) — at least one of these is wrong for any given hosting reality; `idf_component.yml` URLs point at `ptudor.net` paths that should be confirmed live.
  5. `README.md:275` "Write Time: 5ms per page" is correct for the chip and currently contradicted by the code (fixed by R-001).
- **Fix specification:** Correct 60→56 (both files); delete or implement the phantom CHANGELOG items (if the programmer/self-provisioning code lives in another repo, say so with a pointer); either write OVERVIEW.md or remove the references from CLAUDE.md; reconcile repo/package/clone-URL naming to whatever the registry actually hosts. All doc-only; must follow R-002's layout decision so numbers are written once, correctly.
- **Verification:** `grep -rn '60\|OVERVIEW' README.md CHANGELOG.md CLAUDE.md` shows no stale claims; clone URL and registry coordinates resolve.

---

## Summary Table

| ID | Severity | Area | One-line summary |
|------|----------|------|------------------|
| R-001 | Critical | `eeprom_write_bytes` | Writes >8 bytes per transaction; 24AA02E64 page wraparound corrupts all boards with ≥3 components |
| R-002 | Critical | Layout macros / read+write paths | Code stores components at byte 8; docs say byte 16 with a timestamp at 8–15 that is never persisted |
| R-003 | High | `eeprom_write_capabilities` guard | Transient I2C error reads as "not programmed" → overwrite guard bypassed |
| R-004 | High | `eeprom_write_capabilities` ordering | Magic/count committed before descriptors; failed write leaves valid-looking garbage; no read-back verify |
| R-005 | High | examples file | Does not compile: wrong header include, undefined `PROJECT_GALMON`/`GALMON_PCB_MAIN`, missing FreeRTOS includes, defines `app_main` |
| R-006 | Medium | `eeprom_read_capabilities` | `is_valid=true` left set when descriptor read fails |
| R-007 | Medium | module-wide | No mutex around read-modify-write (`eeprom_update_ic_status`); thread-safety contract undocumented |
| R-008 | Medium | low-level I2C | Port hardcoded to `I2C_NUM_0` |
| R-009 | Medium | low-level I2C | Legacy `driver/i2c.h` API — deprecated in IDF 5.2, removed in IDF 6 |
| R-010 | Medium | examples | Manufacturing example omits the mandatory `components[0]` EEPROM self-reference; boards programmed from it fail validation forever |
| R-011 | Low | `eeprom_scan_bus` | Any foreign EEPROM at 0x50–0x57 with byte 0 ∈ 1–254 is reported as a programmed board — *needs investigation* |
| R-012 | Low | `eeprom_read_capabilities` | Unique-ID read failure silently ignored |
| R-013 | Low | `eeprom_find_category` | Casts away `const`, returns mutable pointer into const object |
| R-014 | Low | low-level I2C | No `mem_addr + len ≤ 256` bounds check (device address counter wraps) |
| R-015 | Low | low-level I2C | Command-link builder return values unchecked (`ESP_ERR_NO_MEM`, NULL handle) |
| R-016 | Low | logging | `%d` used for `size_t` (lines 177, 269) |
| R-017 | Low | `eeprom_update_ic_status` | First-match-only on duplicate (category,id); no address-qualified variant |
| R-018 | Low | header | Dead fields/macros: `reserved_footer`, `CAP_OFFSET_COMPONENTS`, `CAP_COMPONENT_SIZE`, `CAP_OFFSET_FOOTER` |
| R-019 | Low | CMakeLists | `REQUIRES driver` should be `PRIV_REQUIRES` |
| R-020 | Low | docs | 60-vs-56 capacity, phantom CHANGELOG features, missing OVERVIEW.md, repo/package name drift |

**Counts:** Critical 2 · High 3 · Medium 5 · Low 10 — total 20.

## Suggested Fix Order

Ordered by dependency, then severity:

1. **R-002** — decide the canonical on-chip layout first (requires the owner to confirm whether field-programmed boards exist and which layout they carry). Every write-path fix and every doc number depends on this decision.
2. **R-001** — page-chunked writes (the write path is unusable for real boards until this lands). Independent of R-002's *choice* but touches the same function family — do immediately after.
3. **R-004** — commit-ordering + read-back verify (same function as R-001; sequence the two edits together).
4. **R-003** — tri-state guard in `eeprom_write_capabilities` (small, isolated, same function).
5. **R-006** — `is_valid` correctness (isolated one-liner, unblocks trustworthy testing of 1–4).
6. **R-009 + R-008 + R-015 + R-019** — as one work item: migrating to the new I2C driver naturally resolves the hardcoded port (bus handle), makes R-015 moot, and settles the CMake dependency. If the driver migration is deferred (downstream IDF pins — investigate), do R-008 (static port variable), R-015, and R-019 individually on the legacy code.
7. **R-007** — module mutex (do after the driver decision so the lock wraps the final transaction shape).
8. **R-014, R-012, R-016** — small hardening/logging fixes, any order.
9. **R-005 + R-010** — repair the examples (compile fixes + self-reference convention) once the library behavior above is final, so the examples demonstrate the corrected API.
10. **R-011, R-013, R-017** — owner-decision items (scan strictness, const-correct return type, address-qualified status update); batch into one API-review conversation since all three may touch public headers.
11. **R-018 + R-020** — dead-symbol cleanup and documentation corrections last, after the layout (R-002) and capacity numbers are settled, so docs are written once.

**Explicitly out of scope / not flagged:** style nitpicks; the placeholder `init_ok = true` blocks in the examples (clearly labeled reference scaffolding); `LICENSE.txt` (standard MIT, fine).
