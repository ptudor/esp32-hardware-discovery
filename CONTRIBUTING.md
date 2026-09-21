# Contributing

Report bugs and request hardware catalog additions through
[GitHub Issues](https://github.com/ptudor/esp32-hardware-discovery/issues).
Include the component tag or commit, ESP-IDF version, target, fitted EEPROM,
selected profile, and enough code or steps to reproduce the behavior.

## Development checks

Run from the repository root with a C11 compiler, Make, and POSIX threads:

```sh
make -C test/host all syntax-examples
```

The tests use simulated I2C devices and need no ESP-IDF installation or board.
They cover all EEPROM profiles, page boundaries, identities, protection,
transaction failures, and concurrency. Fault-injection cases intentionally
emit error logs; the process exit status and final summary indicate success.

With ESP-IDF activated, compile the integration test application:

```sh
cd test/idf
idf.py set-target esp32
idf.py build
```

CI builds the declared targets on ESP-IDF 5.5.4 and checks the minimum 5.2
series on ESP32. This application checks component integration and public
API linkage; it does not access physical hardware. For transport changes,
also record relevant board-level testing of reads, writes, power-cycle
retention, and shared-bus operation.

## Compatibility

The EEPROM format, existing enum values, and public APIs are shared with
downstream projects. Preserve the four-byte descriptor layout and append
new IDs within the appropriate category. Add a name mapping, catalog
documentation, and relevant coverage for new parts. EEPROM protocol support
requires an explicit profile; an I2C acknowledgement alone does not identify
the silicon. See [OVERVIEW.md](OVERVIEW.md) for the architecture.

Describe the problem, resulting behavior, compatibility effects, and checks
run in a pull request. Keep changes focused and update [CHANGELOG.md](CHANGELOG.md)
when behavior changes.

## Releases

A production release uses a `vMAJOR.MINOR.PATCH` tag matching
`idf_component.yml`. Before publishing, update the changelog and installation
examples, run the checks, and verify CI on the exact commit being tagged.
Release notes describe features, framework requirements, and validation scope.
GitHub provides the source archives; applications build their own firmware.
The component is currently distributed through GitHub.
