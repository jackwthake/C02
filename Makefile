.PHONY: all clean cc02 c02-objdump test update-tests

all: cc02 c02-objdump

cc02: cc02/
	@printf '==> cc02\n'
	@$(MAKE) -C cc02

c02-objdump: c02-objdump/
	@printf '\n==> c02-objdump\n'
	@$(MAKE) -C c02-objdump

test: cc02
	@printf '==> build cc02 smoke tests\n'
	@$(MAKE) -C cc02/tests/smoke
	python3 cc02/tests/test.py

update-tests: cc02
	python3 cc02/tests/test.py --update

clean:
	@printf '\n==> cc02 clean\n'
	@$(MAKE) -C cc02 clean
	@printf '\n==> c02-objdump clean\n'
	@$(MAKE) -C c02-objdump clean