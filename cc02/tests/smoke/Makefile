BIN_DIR = ../bin
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
         -I$(SRC_DIR)

SRCS = $(wildcard *.c)

# Use filter-out to remove $(BIN_DIR)/main.o from the generated object list
SRC_OBJS = $(filter-out $(BIN_DIR)/main.o, $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%.o,$(wildcard $(SRC_DIR)/*.c)))

BINS = $(patsubst %.c,$(BIN_DIR)/%,$(SRCS))

.PHONY: all clean

all: $(BIN_DIR) $(SRC_OBJS) $(BINS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_DIR)/%: %.c $(SRC_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SRC_OBJS)

clean:
	rm -f $(BINS) $(SRC_OBJS)