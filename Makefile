.PHONY: all clean

all:
	@printf '\n==> cc02\n'
	@$(MAKE) -C cc02
	@printf '\n==> c02-objdump\n'
	@$(MAKE) -C c02-objdump

test:
	python3 cc02/tests/test.py

update-tests:
	python3 cc02/tests/test.py --update

clean:
	@printf '\n==> cc02 clean\n'
	@$(MAKE) -C cc02 clean
	@printf '\n==> c02-objdump clean\n'
	@$(MAKE) -C c02-objdump clean