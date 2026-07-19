CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
LDLIBS ?=

DEPS := libwebsockets libcjson openssl libcurl sqlite3
CFLAGS += -pthread
LDLIBS += -pthread
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(DEPS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(DEPS))

# COVERAGE=1 builds instrumented objects for gcov/gcovr.
ifeq ($(COVERAGE),1)
CFLAGS += -O0 -g --coverage
LDFLAGS += --coverage
endif

BUILD := build
OBJ := $(BUILD)/obj
BIN := $(BUILD)/hollow-grid-c
TEST_BIN := $(BUILD)/test-core

SRC := \
	src/main.c \
	src/event/event.c \
	src/grid/remote.c \
	src/store/store.c \
	src/transport/server.c \
	src/world/items.c \
	src/world/world.c
TEST_SRC := \
	tests/test_core.c \
	src/event/event.c \
	src/grid/remote.c \
	src/store/store.c \
	src/world/items.c \
	src/world/world.c

OBJS := $(SRC:%=$(OBJ)/%.o)
TEST_OBJS := $(TEST_SRC:%=$(OBJ)/%.o)

.PHONY: all clean check test smoke

all: $(BIN)

$(OBJ)/%.c.o: %.c | $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(TEST_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS) $(LDLIBS)

$(BUILD) $(OBJ):
	mkdir -p $@

test: $(TEST_BIN)
	./$(TEST_BIN)

check: $(BIN) test
	./$(BIN) --help >/dev/null

# Blocking upstream smoke.mjs (definition of done). See tests/smoke.sh.
smoke: $(BIN)
	chmod +x ./tests/smoke.sh
	./tests/smoke.sh ./$(BIN)

clean:
	rm -rf $(BUILD)
