BIN_DIR = ../bin
BIN = $(BIN_DIR)/c02-objdump

.PHONY: all clean

all: $(BIN_DIR) $(BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN): Cargo.toml src/main.rs src/disassembler.rs
	cargo build --release
	cp target/release/c02-objdump $@

clean:
	cargo clean
	rm -f $(BIN)
