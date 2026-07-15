.PHONY: all clean c02-frontend c02-as c02-objdump emu-test

BIN_DIR = bin

all: $(BIN_DIR)/c02c c02-frontend c02-as c02-objdump

# Runtime tests: compile each test/emu/*.c02 to a ROM and execute it in py65,
# checking the code generator's output against embedded EXPECT directives.
# Depends on `all` so the driver and both stages are current first.
emu-test: all
	@printf '\n==> emu-test\n'
	@python3 test/emu/run.py --no-build

# Install the driver script alongside the binaries it invokes.
$(BIN_DIR)/c02c: scripts/c02c.py | $(BIN_DIR)
	@printf '\n==> c02c\n'
	install -m 755 $< $@

$(BIN_DIR):
	mkdir -p $@

c02-frontend: c02-frontend/
	@printf '==> c02-frontend\n'
	@$(MAKE) -C c02-frontend

c02-as: c02-as/
	@printf '==> c02-as\n'
	@$(MAKE) -C c02-as

c02-objdump: c02-objdump/
	@printf '\n==> c02-objdump\n'
	@$(MAKE) -C c02-objdump

clean:
	@printf '==> c02-frontend clean\n'
	@$(MAKE) -C c02-frontend clean
	@printf '==> c02-as clean\n'
	@$(MAKE) -C c02-as clean
	@printf '\n==> c02-objdump clean\n'
	@$(MAKE) -C c02-objdump clean
	rm -f $(BIN_DIR)/c02c
