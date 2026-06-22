BIN_DIR = ../bin
BUILD_DIR = ../../build
SRC_DIR = ../../src
CC = gcc
CFLAGS = -Wall -Wextra -ggdb \
         -Wshadow \
         -Wstrict-prototypes \
         -Wmissing-prototypes \
         -Wconversion \
         -Wno-sign-conversion \
         -Werror \
         -Wno-format-extra-args \
         -I$(SRC_DIR)/common -I$(SRC_DIR)/lexer -I$(SRC_DIR)/parser -I$(SRC_DIR)/analysis -I$(SRC_DIR)/ir-gen -I$(SRC_DIR)/code-gen

SRCS = $(wildcard *.c)
BINS = $(patsubst %.c,$(BIN_DIR)/%,$(SRCS))

SRC_OBJS = $(filter-out $(BUILD_DIR)/main.o, $(wildcard $(BUILD_DIR)/*.o))

.PHONY: all clean

all: $(BIN_DIR) $(BINS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/%: %.c $(SRC_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SRC_OBJS)

clean:
	rm -f $(BINS)
