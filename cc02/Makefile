BIN_DIR = ../bin
BIN = $(BIN_DIR)/cc02
CC = gcc
CFLAGS = -Wall -Wextra -g
SRCS = src/main.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(BIN_DIR) $(BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)
