.PHONY: all clean

all:
	$(MAKE) -C cc02
	$(MAKE) -C c02-objdump

clean:
	$(MAKE) -C cc02 clean
	$(MAKE) -C c02-objdump clean