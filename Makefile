.PHONY: all clean c02-frontend c02-ld c02-as c02-objdump emu-test linker-test

BIN_DIR = bin
LIB_DIR = $(BIN_DIR)/lib
INC_DIR = $(BIN_DIR)/include

all: $(BIN_DIR)/c02c compiler $(LIB_DIR)/libc02.o c02-objdump

compiler: c02-frontend c02-ld c02-as

# Runtime tests: compile each test/emu/*.c02 to a ROM and execute it in py65,
# checking the code generator's output against embedded EXPECT directives.
# Depends on `all` so the driver and both stages are current first.
emu-test: all
	@printf '\n==> emu-test\n'
	@python3 test/emu/run.py --no-build

linker-test: all
	@printf '\n==> linker-test\n'
	@python3 test/linker/run.py

# Install the driver script alongside the binaries it invokes.
$(BIN_DIR)/c02c: scripts/c02c.py | $(BIN_DIR)
	@printf '\n==> c02c\n'
	install -m 755 $< $@

$(BIN_DIR):
	mkdir -p $@

$(LIB_DIR): | $(BIN_DIR)
	mkdir -p $@

$(INC_DIR): libc02/include/
	mkdir -p $@
	cp -r libc02/include/* $@/

c02-frontend: c02-frontend/
	@printf '\n==> c02-frontend\n'
	@$(MAKE) -C c02-frontend

c02-ld: c02-ld/
	@printf '\n==> c02-ld\n'
	@$(MAKE) -C c02-ld

c02-as: c02-as/
	@printf '\n==> c02-as\n'
	@$(MAKE) -C c02-as

c02-objdump: c02-objdump/
	@printf '\n==> c02-objdump\n'
	@$(MAKE) -C c02-objdump

$(LIB_DIR)/libc02.o: libc02/ | $(LIB_DIR) $(INC_DIR) compiler
	@printf '\n==> Building standard lib\n'
	./bin/c02c -c libc02/*.c02 -o $(LIB_DIR)/libc02.o --no-stdlib

clean:
	@printf '==> c02-frontend clean\n'
	@$(MAKE) -C c02-frontend clean
	@printf '==> c02-ld clean\n'
	@$(MAKE) -C c02-ld clean
	@printf '==> c02-as clean\n'
	@$(MAKE) -C c02-as clean
	@printf '\n==> c02-objdump clean\n'
	@$(MAKE) -C c02-objdump clean
	@printf '\n==> standard lib clean\n'
	@rm -rf $(LIB_DIR)
	@rm -rf $(INC_DIR)
	rm -f $(BIN_DIR)/c02c
