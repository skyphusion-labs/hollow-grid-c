CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
LDLIBS ?=

DEPS := libwebsockets libcjson openssl
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(DEPS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(DEPS))

BUILD := build
BIN := $(BUILD)/hollow-grid-c
TEST_BIN := $(BUILD)/test-core
SRC := \
	src/main.c \
	src/event/event.c \
	src/store/store.c \
	src/transport/server.c \
	src/world/world.c
TEST_SRC := \
	tests/test_core.c \
	src/event/event.c \
	src/store/store.c \
	src/world/world.c

.PHONY: all clean check test

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BIN): $(SRC) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(TEST_SRC) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_SRC) $(LDFLAGS) $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

check: $(BIN) test
	./$(BIN) --help >/dev/null

clean:
	rm -rf $(BUILD)
