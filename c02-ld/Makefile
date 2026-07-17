BIN_DIR = ../bin
BIN = $(BIN_DIR)/c02-ld

all: $(BIN_DIR)
	opam exec -- dune build
	install -m 755 _build/default/bin/c02_ld.exe $(BIN_DIR)/c02-ld

clean:
	opam exec -- dune clean
	rm -f $(BIN)