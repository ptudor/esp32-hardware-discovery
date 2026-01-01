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
- 60 component capacity per board
- Field-updateable status (7 status codes)
- Reserved bytes (4-6) for future features
- Manufacturing programmer with LED feedback
- Self-provisioning module for first-boot auto-configuration
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

### Planned Features
- CRC8 checksum in reserved byte 4
- Feature flags in reserved byte 5
- Extended component count in reserved byte 6
- Additional component categories as needed
- More IC IDs within existing categories
