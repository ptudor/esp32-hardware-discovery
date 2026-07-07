# Host-side unit tests for esp_hardware_discovery.
#
# Builds src/esp_hardware_discovery.c against mock ESP-IDF headers and a
# simulated 24AA02E64 (8-byte page buffer, write-cycle busy, fault injection).
# No ESP-IDF installation required:
#
#   make            # build and run the tests
#   make syntax-examples   # syntax-check the reference examples file

CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror=format -g -pthread \
         -Imocks -I../../include

all: run

hw_discovery_tests: test_main.c mock_i2c.c mock_i2c.h \
                    ../../src/esp_hardware_discovery.c \
                    ../../include/esp_hardware_discovery.h
	$(CC) $(CFLAGS) test_main.c mock_i2c.c -o $@

run: hw_discovery_tests
	./hw_discovery_tests

syntax-examples:
	$(CC) $(CFLAGS) -fsyntax-only ../../esp_hardware_discovery_examples.c
	@echo "examples: syntax OK"

clean:
	rm -f hw_discovery_tests
	rm -rf hw_discovery_tests.dSYM

.PHONY: all run clean syntax-examples
