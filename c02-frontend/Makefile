BIN_DIR = ../bin
BIN = $(BIN_DIR)/c02-frontend

all: $(BIN_DIR)
	cabal build
	cp "$$(cabal list-bin c02-frontend)" $(BIN)

clean:
	cabal clean
	rm -f $(BIN)