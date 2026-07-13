BIN_DIR    = ../bin
BUILD_ROOT = build
CC = gcc

# Warnings are identical across every variant; only optimization / debug /
# sanitizer flags differ. See the release/debug/sanitize targets below.
WARNINGS = -Wall -Wextra \
           -Wshadow \
           -Wstrict-prototypes \
           -Wmissing-prototypes \
           -Wconversion \
           -Wno-sign-conversion \
           -Werror \
           -Wno-format-extra-args

CFLAGS = $(WARNINGS) -Isrc $(CFLAGS_EXTRA)

SRCS = $(wildcard src/*.c)

.PHONY: all release debug sanitize build clean

# Default build is the optimized release binary (../bin/c02-as).
all: release

# Each variant re-invokes make with its own flags, build dir, and binary
# name so the three can coexist without clobbering each other's objects.
release:
	@$(MAKE) --no-print-directory build VARIANT=release BIN_SUFFIX= \
	    CFLAGS_EXTRA="-O3"

debug:
	@$(MAKE) --no-print-directory build VARIANT=debug BIN_SUFFIX=-debug \
	    CFLAGS_EXTRA="-O0 -ggdb"

sanitize:
	@$(MAKE) --no-print-directory build VARIANT=sanitize BIN_SUFFIX=-asan \
	    CFLAGS_EXTRA="-O0 -ggdb -fsanitize=address,undefined -fno-omit-frame-pointer" \
	    LDFLAGS_EXTRA="-fsanitize=address,undefined"

# ---------------------------------------------------------------------------
# Parametrized build - never invoke directly; the targets above set VARIANT,
# BIN_SUFFIX, and the flag overrides that make these definitions resolve.
# ---------------------------------------------------------------------------
BUILD_DIR = $(BUILD_ROOT)/$(VARIANT)
BIN       = $(BIN_DIR)/c02-as$(BIN_SUFFIX)
OBJS      = $(addprefix $(BUILD_DIR)/,$(notdir $(SRCS:.c=.o)))
DEPS      = $(OBJS:.o=.d)

vpath %.c src

build: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_EXTRA)

# -MMD -MP writes a per-object .d listing the headers it #includes, so touching
# a header (e.g. ir.h) rebuilds every .o that depends on it. Without this, a
# struct-layout change in a header silently under-rebuilds and the mismatched
# objects crash at runtime.
$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_ROOT) \
	    $(BIN_DIR)/c02-as $(BIN_DIR)/c02-as-debug $(BIN_DIR)/c02-as-asan
