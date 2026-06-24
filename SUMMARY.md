# Code Generation Steps 0–6 Summary

## Commits

```
920e29e Step 0: Replace silent default:break with unhandled TAC op diagnostic
6464905 Step 1: Fix global/ZP bug class in INC/DEC and COMPARE_OP
3421c6e Step 2: Implement u8 TAC_ADD/TAC_SUB with CLC/ADC and SEC/SBC
cdbf855 Step 3: Implement TAC_NEG (unary minus)
c44d39a Step 4: Verify u16 arithmetic and signed i8/i16 via emu tests
3072e08 Step 5: Extend COMPARE_OP to u16 with high-byte-first comparison
24536d4 Step 6: Implement signed comparisons (i8/i16) using N XOR V pattern
e8ae5a9 Fix emu_neg_u8 test to actually discriminate TAC_NEG
```

## What changed and why

### Step 0 — Safety net (`generator.c`)
Replaced `default: break` in the TAC switch with `fprintf(stderr) + return 0`.
Programs using unimplemented TAC ops now fail at compile time instead of silently
producing wrong binaries. `emit_function_from_cfg` returns `int` to propagate
the error. Bootstrap emu tests switched from `analyzer_basic.c02` (which uses
unimplemented ops) to `emu_store_const.c02`.

### Step 1 — Fix global/ZP bug class (`generator.c`)
`TAC_INC`/`TAC_DEC` on globals now emit `INC abs`/`DEC abs` ($EE/$CE) targeting
the global's RAM address instead of the stale ZP scratch slot. `COMPARE_OP` RHS
operands route through a new `emit_cmp_byte` helper that dispatches `CMP abs`
($CD) for globals. 16-bit global INC uses `BNE +3` (abs is 3 bytes, not 2).
New opcode emitters: `inc_abs`, `dec_abs`, `cmp_abs`.

### Step 2 — u8 arithmetic (`generator.c`)
`TAC_ADD`: `CLC; LDA src1; ADC src2; STA dst`.
`TAC_SUB`: `SEC; LDA src1; SBC src2; STA dst`.
Global-aware RHS helpers (`emit_adc_byte`, `emit_sbc_byte`) dispatch `ADC abs`
($6D) / `SBC abs` ($ED) for globals. CLC/SEC emitted once outside the byte loop
so carry propagates correctly for u16 (Step 4 came free).
New opcode emitters: `clc`, `sec`, `adc_{imm,zpg,abs}`, `sbc_{imm,zpg,abs}`.

### Step 3 — TAC_NEG (`generator.c`)
`dst = -src1` via `SEC; LDA #0; SBC src1; STA dst`. SEC emitted once before the
byte loop so borrow propagates for u16 negation.

### Step 4 — u16 arithmetic (emu tests only)
TAC_ADD/TAC_SUB were already width-aware from Step 2. This step added emu tests
proving u16 carry propagation and signed i8/i16 arithmetic (two's complement
makes ADD/SBC bit-identical for signed and unsigned).

### Step 5 — u16 comparisons (`generator.c`)
Replaced the `COMPARE_OP` macro with backpatched comparison handlers. u16
ordering (LT/GTE/GT/LTE) uses high-byte-first: compare high bytes to determine
definite ordering, fall through to low bytes when equal. u16 EQ/NEQ compare both
bytes. All forward branch offsets use backpatching (record `code_pos`, patch
after target is emitted) instead of hardcoded values, robust to variable-size
operand loads (zpg=2 vs abs=3 vs imm=2). GTE inverts the LT result with
`EOR #$01`.

### Step 6 — Signed comparisons (`generator.c`)
Ordering comparisons detect signed operand types (`TYPE_I8`/`TYPE_I16`) and emit
the N XOR V pattern: `SEC; SBC; BVC +2; EOR #$80; BMI true` for i8. i16 signed
uses N^V on the high-byte subtraction, falling through to unsigned low-byte
comparison (BCC) when high bytes are equal. EQ/NEQ are sign-agnostic (unchanged).
New opcode emitter: `bvc_rel` ($50). New helper: `is_signed_type()`.

## Key files

| File | Changes |
|------|---------|
| `cc02/src/code-gen/generator.c` | All codegen: new TAC handlers, opcode emitters, ALU helpers, comparison restructure |
| `cc02/tests/emu_test.py` | 15 new test functions, bootstrap tests decoupled from analyzer_basic.c02 |
| `cc02/tests/emu_*.c02` | 15 new emulator test source files |
| `CHANGELOG.md` | [Unreleased] section with all step summaries |

## Test count

118 tests (before) → 169 tests (after), all passing under `make test` with
valgrind leak checks.

## New emulator tests

| Test | Verifies |
|------|----------|
| `emu_inc_global` | INC on global variable hits RAM, not stale ZP |
| `emu_cmp_global` | CMP against global uses abs addressing |
| `emu_add_u8` | u8 addition (10+32=42) |
| `emu_sub_u8` | u8 subtraction (50-8=42) |
| `emu_add_const` | u8 add with constant RHS (40+2=42) |
| `emu_neg_u8` | Unary minus (-42 = 0xD6 = 214) |
| `emu_add_u16` | u16 addition with carry (300+200=500) |
| `emu_sub_u16` | u16 subtraction with borrow (1000-500=500) |
| `emu_add_i8` | Signed i8 addition (-5+47=42) |
| `emu_add_i16` | Signed i16 addition (-300+800=500) |
| `emu_cmp_u16_lt` | u16 255<256 (high byte decides) |
| `emu_cmp_u16_eq` | u16 500==500 (both bytes must match) |
| `emu_cmp_u16_gt` | u16 1000>255 (high byte decides) |
| `emu_cmp_i8_lt` | i8 -5<3 (negative < positive, unsigned would say false) |
| `emu_cmp_i8_gt` | i8 3>-5 (positive > negative) |
| `emu_cmp_i8_neg` | i8 -10<-3 (both negative) |
| `emu_cmp_i16_signed` | i16 -300<300 (16-bit signed ordering) |
