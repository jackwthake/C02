BIN_DIR = ../bin
BUILD_DIR = build
BIN = $(BIN_DIR)/cc02
CC = gcc
CFLAGS = -Wall -Wextra -O3 \
         -Wshadow \
         -Wstrict-prototypes \
         -Wmissing-prototypes \
         -Wconversion \
         -Wno-sign-conversion \
         -Werror \
         -Wno-format-extra-args \
         -Isrc/common -Isrc/lexer -Isrc/parser -Isrc/analysis

SRC_DIRS = src src/common src/lexer src/parser src/analysis
SRCS = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJS = $(addprefix $(BUILD_DIR)/,$(notdir $(SRCS:.c=.o)))

vpath %.c $(SRC_DIRS)

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(BIN)
