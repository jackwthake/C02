# Memory Map for Ben Eater 65C02 Kit Computer

- RAM from `0x0000` - `0x3FFF`
  - ZP: `$00` - `$FF`
  - Hardware stack (CPU Managed): `$0100` - `$01FF`
  - Program Memory: `$0200` - `$3FFF`
- VIA needs A13, and A14 set, `0x6000` - `0x7FFF`
- ROM from `0x8000` - `0xFFFF`
  - `$8000` -> program start
