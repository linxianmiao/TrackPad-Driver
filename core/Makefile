CC ?= clang
CFLAGS ?= -std=c17 -Wall -Wextra -Werror -pedantic -O2
INCLUDES = -Iinclude
BUILD_DIR = build
CORE_SOURCE = src/amtptp_core.c

.PHONY: all clean test

all: $(BUILD_DIR)/amtptp-cli

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/amtptp-cli: $(CORE_SOURCE) tools/amtptp_cli.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(CORE_SOURCE) tools/amtptp_cli.c -o $@

$(BUILD_DIR)/test-core: $(CORE_SOURCE) tests/test_core.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(CORE_SOURCE) tests/test_core.c -o $@

test: $(BUILD_DIR)/test-core
	./$(BUILD_DIR)/test-core

clean:
	rm -f $(BUILD_DIR)/amtptp-cli $(BUILD_DIR)/test-core
