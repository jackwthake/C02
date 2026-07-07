.PHONY: all clean c02-frontend c02-as c02-objdump

BIN_DIR = bin

all: c02-frontend c02-as c02-objdump $(BIN_DIR)/c02c

# Install the driver script alongside the binaries it invokes.
$(BIN_DIR)/c02c: c02c | $(BIN_DIR)
	@printf '\n==> c02c\n'
	install -m 755 c02c $@

$(BIN_DIR):
	mkdir -p $@

c02-frontend: c02-frontend
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
