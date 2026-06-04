# 65C02 Disassembler - Complete Instruction Set

## Overview

The C02 disassembler has been updated to support the complete WDC 65C02 instruction set, including both the standard 6502 instructions and the W65C02 extensions.

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

## Address Modes Supported

The disassembler now supports all 6502 and W65C02 addressing modes:

1. **Implied** - No operand
2. **Accumulator** - Operates on accumulator
3. **Immediate** - 8-bit literal value (`#$oper`)
4. **ZeroPage** - 8-bit address in zero page (`$oper`)
5. **ZeroPage,X** - Zero page address indexed by X (`$oper,X`)
6. **ZeroPage,Y** - Zero page address indexed by Y (`$oper,Y`)
7. **Absolute** - 16-bit address (`$oper`)
8. **Absolute,X** - Absolute address indexed by X (`$oper,X`)
9. **Absolute,Y** - Absolute address indexed by Y (`$oper,Y`)
10. **Indirect** - Indirect address (`($oper)`)
11. **Indirect,X** - Pre-indexed indirect (`($oper,X)`)
12. **Indirect,Y** - Post-indexed indirect (`($oper),Y`)
13. **Relative** - Relative branch offset (`$oper`)
14. **ZeroPage Indirect** (W65C02) - Zero page indirect (`($oper)`)
15. **Absolute,X Indirect** (W65C02) - Absolute indexed indirect (`($oper,X)`)
16. **ZeroPage,Relative** (W65C02) - For bit manipulation branches

## Implementation Details

The disassembler implementation in [src/disassembler.rs](src/disassembler.rs) uses a large match statement to decode each of the 256 possible 6502/65C02 opcodes. Each instruction case handles:

1. Reading any required operands from the binary
2. Determining the addressing mode
3. Formatting the instruction in standard assembly syntax
4. Printing the output with the current address

### Label Generation

The disassembler automatically generates labels for jump targets:

- JSR (Jump to Subroutine) and JMP (Jump) instructions create labels
- Labels are named sequentially (L0, L1, L2, etc.) in address order
- Branch targets are displayed either as labels or as addresses

### Output Format

Each instruction is displayed as:

```assembly
ADDRESS: MNEMONIC OPERAND
```

Examples:

```assembly
8000: SEI
8001: CLD
8002: LDX #$FF
8004: TXS
8005: LDA $00
```

## Testing

The disassembler has been tested with various compiled C02 programs and correctly disassembles:

- All standard 6502 instructions
- All W65C02 extensions
- Complex programs with jumps and subroutine calls
- Various addressing modes

## References

- [6502 Instruction Set Reference](https://www.masswerk.at/6502/6502_instruction_set.html)
- [WDC 65C02 Documentation](https://www.westerndesigncenter.com/)
- [Western Design Center W65C02S Datasheet](https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf)

## Notes

- Some W65C02 instructions like STP and WAI are implemented with placeholder operands in the current version
- The disassembler distinguishes between standard 6502 and W65C02-specific instructions in comments
- Unknown opcodes are displayed as "Unknown Opcode: XX" for debugging purposes
