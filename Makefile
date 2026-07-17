# hollow-grid-c -- minimal scaffold binary
#
# Phase 0 will grow this into a WebSocket world server. Today it only proves
# the build and prints usage.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

BUILD := build
BIN := $(BUILD)/hollow-grid-c
SRC := src/main.c

.PHONY: all clean

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BIN): $(SRC) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -rf $(BUILD)
