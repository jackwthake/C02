# 65C02 Disassembler

Decodes compiled C02 `.bin` ROM files back into annotated 65C02 assembly. Resolves jump targets to named labels and understands the C02 ROM layout (code, data, and vector sections).

## Usage

```shell
c02-objdump [OPTIONS] <BIN FILE>
```

### Options

| Flag | Description |
| :--- | :--- |
| *(none)* | Disassemble `.text` section only |
| `-a`, `--all` | Disassemble code + hex dump of `.data` section |
| `-d`, `--data` | Hex dump of `.data` section only |
| `-s`, `--sections` | Print section layout and vector table only |

### Examples

**Default** — disassemble code, stop before data:

```assembly
L1:
0057: LDA $0200
005A: STA $05
005C: LDA $0201
005F: STA $06

L2:
0061: LDY #$00
0063: LDA ($05),Y
0065: STA $09
...
0091: RTS
```

**`-a`** — code disassembly followed by data hex dump:

```
...
L3:
0091: RTS

.data  $8092–$809C  (11 bytes)

8092: 48 65 6C 6C 6F 20  43 30 32 21 00  Hello C02!.
```

**`-s`** — section map:

```
ROM size:  32768 bytes (0x8000)

  .text    $8000–$8091  (146 bytes)
  .data    $8092–$809C  (11 bytes)
  vectors  $FFFA–$FFFF  (6 bytes)

  NMI      $0000
  Reset    $8000
  IRQ      $0000
```

## Implementation Details

### Section Detection

The C02 code generator writes a 2-byte code/data boundary address at `$FFF8` (just before the 6-byte vector table). The disassembler reads this to determine where `.text` ends and `.data` begins. For binaries without this marker, it falls back to scanning for the last non-NOP (`$EA`) byte.

### Label Generation

- JSR and JMP instructions create labels at their target addresses
- Labels are named sequentially (L0, L1, L2, ...) in address order
- A pre-scan pass walks the full instruction stream using a 256-entry opcode size table to stay in sync with instruction boundaries
- The reset vector is read to translate between ROM buffer offsets and absolute addresses

## References

- [6502 Instruction Set Reference](https://www.masswerk.at/6502/6502_instruction_set.html)
- [WDC 65C02 Documentation](https://www.westerndesigncenter.com/)
- [Western Design Center W65C02S Datasheet](https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf)

## Total Instructions Implemented: 256 Opcodes

### Standard 6502 Instructions

#### Load/Store Instructions

- **LDA** - Load Accumulator (Immediate, ZeroPage, ZeroPage,X, Absolute, Absolute,X, Absolute,Y, Indirect,X, Indirect,Y)
- **LDX** - Load X Register (Immediate, ZeroPage, ZeroPage,Y, Absolute, Absolute,Y)
- **LDY** - Load Y Register (Immediate, ZeroPage, ZeroPage,X, Absolute, Absolute,X)
- **STA** - Store Accumulator (ZeroPage, ZeroPage,X, Absolute, Absolute,X, Absolute,Y, Indirect,X, Indirect,Y)
- **STX** - Store X Register (ZeroPage, ZeroPage,Y, Absolute)
- **STY** - Store Y Register (ZeroPage, ZeroPage,X, Absolute)

#### Arithmetic Instructions

- **ADC** - Add with Carry (Immediate, ZeroPage, ZeroPage,X, Absolute, Absolute,X, Absolute,Y, Indirect,X, Indirect,Y)
- **SBC** - Subtract with Carry (Immediate, ZeroPage, ZeroPage,X, Absolute, Absolute,X, Absolute,Y, Indirect,X, Indirect,Y)

#### Logical Instructions

- **AND** - Logical AND (Immediate, ZeroPage, ZeroPage,X, Absolute, Absolute,X, Absolute,Y, Indirect,X, Indirect,Y)
- **EOR** - Exclusive OR (Immediate, ZeroPage, ZeroPage,X, Absolute, Absolute,X, Absolute,Y, Indirect,X, Indirect,Y)
- **ORA** - Logical OR (Immediate, ZeroPage, ZeroPage,X, Absolute, Absolute,X, Absolute,Y, Indirect,X, Indirect,Y)

#### Shift and Rotate Instructions

- **ASL** - Arithmetic Shift Left (Accumulator, ZeroPage, ZeroPage,X, Absolute, Absolute,X)
- **LSR** - Logical Shift Right (Accumulator, ZeroPage, ZeroPage,X, Absolute, Absolute,X)
- **ROL** - Rotate Left (Accumulator, ZeroPage, ZeroPage,X, Absolute, Absolute,X)
- **ROR** - Rotate Right (Accumulator, ZeroPage, ZeroPage,X, Absolute, Absolute,X)

#### Increment/Decrement Instructions

- **DEC** - Decrement Memory (ZeroPage, ZeroPage,X, Absolute, Absolute,X)
- **DEX** - Decrement X Register (Implied)
- **DEY** - Decrement Y Register (Implied)
- **INC** - Increment Memory (ZeroPage, ZeroPage,X, Absolute, Absolute,X)
- **INX** - Increment X Register (Implied)
- **INY** - Increment Y Register (Implied)

#### Comparison Instructions

- **BIT** - Bit Test (ZeroPage, Absolute)
- **CMP** - Compare Accumulator (Immediate, ZeroPage, ZeroPage,X, Absolute, Absolute,X, Absolute,Y, Indirect,X, Indirect,Y)
- **CPX** - Compare X Register (Immediate, ZeroPage, Absolute)
- **CPY** - Compare Y Register (Immediate, ZeroPage, Absolute)

#### Branch Instructions

- **BCC** - Branch on Carry Clear (Relative)
- **BCS** - Branch on Carry Set (Relative)
- **BEQ** - Branch on Equal (Relative)
- **BMI** - Branch on Minus (Relative)
- **BNE** - Branch on Not Equal (Relative)
- **BPL** - Branch on Plus (Relative)
- **BVC** - Branch on Overflow Clear (Relative)
- **BVS** - Branch on Overflow Set (Relative)

#### Jump & Subroutine Instructions

- **JMP** - Jump (Absolute, Indirect)
- **JSR** - Jump to Subroutine (Absolute)
- **RTS** - Return from Subroutine (Implied)
- **RTI** - Return from Interrupt (Implied)
- **BRK** - Break/Interrupt (Implied)

#### Stack Instructions

- **PHA** - Push Accumulator (Implied)
- **PHP** - Push Processor Status (Implied)
- **PLA** - Pull Accumulator (Implied)
- **PLP** - Pull Processor Status (Implied)
- **TSX** - Transfer Stack Pointer to X (Implied)
- **TXS** - Transfer X to Stack Pointer (Implied)

#### Register Transfer Instructions

- **TAX** - Transfer Accumulator to X (Implied)
- **TAY** - Transfer Accumulator to Y (Implied)
- **TXA** - Transfer X to Accumulator (Implied)
- **TYA** - Transfer Y to Accumulator (Implied)

#### Flag Instructions

- **CLC** - Clear Carry (Implied)
- **CLD** - Clear Decimal (Implied)
- **CLI** - Clear Interrupt Disable (Implied)
- **CLV** - Clear Overflow (Implied)
- **SEC** - Set Carry (Implied)
- **SED** - Set Decimal (Implied)
- **SEI** - Set Interrupt Disable (Implied)

#### Other Instructions

- **NOP** - No Operation (Implied)

### W65C02 Extensions

#### New Address Modes

- **ZeroPage Indirect** - `(oper)` - Used by ADC, AND, CMP, EOR, LDA, ORA, SBC, STA
- **Absolute,X Indirect** - `(oper,X)` - Used by JMP instruction
- **Immediate** - `#oper` - Used by BIT instruction (W65C02 enhancement)

#### New Instructions with Additional Address Modes

- **ADC** - (ZeroPage Indirect) opcode 0x72
- **AND** - (ZeroPage Indirect) opcode 0x32
- **BIT** - Immediate 0x89, Absolute,X 0x3C, ZeroPage,X 0x34
- **CMP** - (ZeroPage Indirect) opcode 0xD2
- **DEC** - Accumulator 0x3A (W65C02)
- **EOR** - (ZeroPage Indirect) opcode 0x52
- **INC** - Accumulator 0x1A (W65C02)
- **JMP** - (Absolute,X Indirect) opcode 0x7C (W65C02)
- **LDA** - (ZeroPage Indirect) opcode 0xB2
- **ORA** - (ZeroPage Indirect) opcode 0x12
- **SBC** - (ZeroPage Indirect) opcode 0xF2
- **STA** - (ZeroPage Indirect) opcode 0x92
- **STZ** - Store Zero (ZeroPage 0x64, ZeroPage,X 0x74, Absolute 0x9C, Absolute,X 0x9E)
- **TRB** - Test and Reset Memory Bit (ZeroPage 0x14, Absolute 0x1C)
- **TSB** - Test and Set Memory Bit (ZeroPage 0x04, Absolute 0x0C)

#### Stack Instructions (W65C02)

- **PHX** - Push X Register (opcode 0xDA)
- **PHY** - Push Y Register (opcode 0x5A)
- **PLX** - Pull X Register (opcode 0xFA)
- **PLY** - Pull Y Register (opcode 0x7A)

#### New Branch Instructions (W65C02)

- **BRA** - Branch Always (Relative) opcode 0x80

#### Bit Manipulation Instructions (W65C02)

- **BBR0-7** - Branch on Bit Reset (ZeroPage, Relative) opcodes 0x0F, 0x1F, 0x2F, 0x3F, 0x4F, 0x5F, 0x6F, 0x7F
- **BBS0-7** - Branch on Bit Set (ZeroPage, Relative) opcodes 0x8F, 0x9F, 0xAF, 0xBF, 0xCF, 0xDF, 0xEF, 0xFF
- **RMB0-7** - Reset Memory Bit (ZeroPage) opcodes 0x07, 0x17, 0x27, 0x37, 0x47, 0x57, 0x67, 0x77
- **SMB0-7** - Set Memory Bit (ZeroPage) opcodes 0x87, 0x97, 0xA7, 0xB7, 0xC7, 0xD7, 0xE7, 0xF7

#### Control Instructions (W65C02)

- **STP** - Stop/Sleep Mode (opcode 0xDB)
- **WAI** - Wait for Interrupt (opcode 0xCB)

---

## Third-Party Licenses

This tool depends on [clap](https://github.com/clap-rs/clap) for
command-line argument parsing, dual-licensed under MIT or Apache-2.0.
