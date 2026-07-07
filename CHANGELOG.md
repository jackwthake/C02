# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/) - while
the project is in `0.x`, breaking changes may land in MINOR releases; PATCH
releases are reserved for bug fixes only.

## [Unreleased]

A ground-up re-architecture of the compiler: the monolithic `cc02` binary is
being split into a multi-stage toolchain — a Haskell **frontend**
(lexer/parser/analysis/IR generation), a planned OCaml **linker/optimizer**,
and a C **backend** (`c02-as`) — coordinated by a `c02c` driver. The serialized
IR object (`.o`) is the contract between stages. The v1.0 language and its 6502
ROM output are unchanged; what changes is how the compiler is built and invoked.

### Added

- **`c02-as` — standalone code-generation backend.** Consumes a serialized IR
  object file and emits a 32K 6502 ROM, with no dependency on the frontend,
  AST, or symbol table. Lifted from `cc02`'s IR-object → codegen path (the one
  part of the old toolchain that survives the rewrite) and trimmed to a
  self-contained module: `ir.h` no longer pulls in the parser/analyzer, the
  serializer keeps only its read half, and the loaded-IR context drops all
  generation-side state. Output is verified byte-identical to `cc02` across the
  example programs and the struct/field regression cases;

- **Multiple `c02-as` build variants** — `make` (release, `-O3`), `make debug`
  (`-O0 -ggdb`), and `make sanitize` (`-ggdb -fsanitize=address,undefined`),
  each in its own build directory so the three coexist without clobbering each
  other. The sanitized build runs clean (ASan + UBSan, leak detection on) over
  the full example set;

- **`c02-frontend` — Haskell frontend scaffold.** A `cabal` project that will
  host the rewritten lexer, parser, semantic analysis, and IR generation.
  Currently a stub that does not yet parse arguments or emit an object;

- **`c02c` — compiler driver.** A script orchestrating the pipeline
  (frontend → link/optimize → codegen), installed into `bin/` alongside the
  binaries it invokes and able to resolve them whether run from `bin/` or the
  repo root. The codegen stage is already wired for real, so `c02c prog.o -o
  prog.bin` produces a ROM today; the frontend → codegen `.o` handoff is a TODO
  pending the frontend;

### Changed

- **The IR object format is now a cross-language wire contract.** With the
  frontend moving to Haskell, `ir-serial.c`'s read layout (magic, version,
  length-prefixed strings, native endianness and integer widths) is the
  interface any writer must match byte-for-byte, rather than an internal
  same-binary detail. Documented as such in `ir.h` and `ir-serial.c`;

- **Top-level build and CI rewired.** `make` now builds `c02-frontend`,
  `c02-as`, and `c02-objdump` and installs `c02c`; the `cc02`-only `test` /
  `update-tests` Makefile targets and the CI compile-examples step are gone;

- **`.gitignore`** — added Haskell/cabal artifacts (`dist-newstyle/`, GHC
  environment files, `cabal.project.local`, `*.hi`);

### Removed

- **`cc02` — the monolithic single-file compiler.** Its frontend
  (lexer/parser/analyzer/IR generator) is being rewritten in Haskell and its
  backend extracted into `c02-as`, so the combined binary and its test suite
  are removed from this tree. It remains available in the pre-rewrite `main`
  history;

## [v1.0.0] 2026-07-01

- **v1.0 milestone: Complete Single-File Language** — every must-have and
  should-have feature from `docs/roadmap.md`'s v1.0 checklist is now
  implemented: function calls, pointer store, address-of, implicit widening,
  string literal locals, pointer arithmetic (`*(ptr + i)`), break/continue,
  struct field access, multiply/divide/modulo, and bitwise/shift operators.
  The only items left unchecked are explicitly-optional "nice-to-have"s
  (short-circuit `&&`/`||` codegen outside boolean-context use, and the
  `&=`/`|=`/`^=`/`<<=`/`>>=` compound assignment forms) — the roadmap's own
  goal, "someone can sit down and write a non-trivial 65C02 program without
  hitting an unimplemented wall," is met;

- **`break` and `continue`** — both now supported inside `while` and `for`
  loops. The analyzer tracks a `loop_depth` counter (incremented for the
  duration of a loop body) and rejects either statement outside a loop
  (`ERR_BREAK_OUTSIDE_LOOP` / `ERR_CONTINUE_OUTSIDE_LOOP`). The IR generator
  maintains a `loop_ctx_t { continue_label, break_label }` stack; both
  statements desugar to a plain `TAC_JUMP` to the appropriate label, so
  codegen needed no changes. For `for` loops, `continue` jumps to a new
  label sitting between the body and the incrementer, so the incrementer
  still runs before the next condition check. See
  `docs/break-continue-implementation.md` for the full design writeup,
  including a bonus fix along the way: the `for`-loop incrementer was being
  lowered with `lower_expr` instead of `lower_stmt`, so `i = i + 1`-style
  incrementers (as opposed to `++i`) were silently generating no code;

- **Local string literal initializers** — `u8 *p = "some string";` now works
  inside function bodies, not just at global scope. The string data is placed in
  the ROM data section via the existing `data_fixup_t` backpatch mechanism (same
  as global strings); the pointer value (ROM address of the string) is written
  into the local's ZP slot during function entry via `TAC_COPY` with an
  `OPERAND_CONST_STR` source. Callee-saves push and pop the pointer ZP slot
  across nested calls to preserve it across function boundaries. The pointer
  occupies 2 bytes of ZP for the lifetime of the function;

- **Variable shadowing is now a semantic error** — a variable declaration
  (or function parameter) that reuses a name still visible from an enclosing,
  still-live scope now raises `ERR_SHADOWED_DECLARATION` instead of silently
  compiling. Root cause: the IR/codegen identify a variable purely by its bare
  name (`OPERAND_VAR.name`, matched via `strcmp` in `zp_map_build`), with no
  per-scope qualifier, so a shadowed inner declaration (e.g. a `for (u8 i = 0;
  ...)` nested inside a function that already has an outer `u8 i`) aliased the
  *same* zero-page storage as its outer namesake — the inner loop's own
  init/exit value silently clobbered the outer variable. This surfaced as
  `break` firing on the wrong iteration in `arithmetic_demo.c02`. Reusing a
  name across scopes that never overlap on the scope stack (e.g. two sibling
  `for` loops each declaring their own `i`) is unaffected and remains legal;

## [v0.2.17] 2026-06-27

- **ROM overflow detection** — the `EMIT()` macro now bounds-checks `code_pos`
  against `ROM_SIZE` before writing, setting an `overflow` flag on `emitter_t`
  instead of silently writing past the end of the 32 KB buffer. A new
  `PATCH_BYTE(pos, val)` macro applies the same guard to all branch-offset and
  address backpatches. `resolve_func_fixups` and `resolve_local_fixups`
  early-return when overflow is already set. `generate_rom` checks `e.overflow`
  independently after the code section, data section, and symbol-table phases,
  printing a targeted diagnostic and returning `NULL` on failure. Previously,
  programs that grew past 32 KB would corrupt the ROM buffer without any error.
  `emit_symbol_table` now also writes through `EMIT()` instead of raw pointer
  writes, inheriting the overflow guard.

- **`__heap_start` implicit compiler global** — programs may declare
  `decl u16 __heap_start;` to read the address of the first free RAM byte after
  all user globals are allocated. The driver injects the declaration
  automatically so no user `decl` is required in practice. The value is
  initialized during the bootstrap sequence. Intended as a base pointer for
  simple bump allocators.

- **`__memory_top` implicit compiler global** — `decl u16 __memory_top;`
  evaluates to `$3FFF`, the top of the general-purpose RAM region (see
  `docs/memmap.md`). Injected alongside `__heap_start`. Both constants are
  defined as `RAM_TOP` in the codegen memory map.

- **Compiler extern two-pass allocation** — `emit_compiler_extern_inits` is
  refactored into an explicit two-pass design: pass 1 allocates all RAM slots
  (settling `e->ram_pos`), pass 2 emits initializers. This ensures
  `__heap_start` captures the correct first-free-RAM address regardless of
  declaration order. Unknown non-function externs now print a diagnostic and
  cause codegen to fail instead of being silently skipped.
  - `ALLOC_COMPILER_SLOT` / `EMIT_COMPILER_VALUE` — pair of local macros that
    collapse the slot-allocation and value-emission boilerplate to one line each;
    both are `#undef`'d immediately after the function.

## [v0.2.16] 2026-06-27

- **16-bit multiply / divide / modulo (`__mul16`, `__div16`, `__sdiv16`)** —
  `TAC_MUL`, `TAC_DIV`, and `TAC_MOD` on `u16`/`i16` operands now compile to
  subroutine calls rather than erroring. Three new helpers:
  - `__mul16` — 16-iteration shift-and-add. Correct for both `u16` and `i16`
    because the low 16 bits of a two's-complement product are sign-agnostic.
    Overflow silently wraps to the low 16 bits (same as C).
  - `__div16` — 16-iteration shift-subtract with CMP-based comparison. Uses a
    `BCS dosub` guard before the 16-bit subtract so divisors with bit 15 set
    (≥ `$8000`) are handled correctly; the naive SEC-before-compare approach
    clobbers the overflow carry and produces wrong quotients for those values.
  - `__sdiv16` — sign wrapper around `__div16`, mirroring `__sdiv8`: encodes
    signs in `HELPER_SIGN` (`$EC`, bit 7 = negate quotient, bit 6 = negate
    remainder), negates both operands, calls `__div16`, then restores signs.
    Follows C truncation-toward-zero convention. `needs_sdiv16 = 1` implies
    `needs_div16 = 1`.
  - New 16-bit helper ZP zone: `$E0–$E7` (`HELPER16_ARG1`/`ARG2`/`RES`/`REM`,
    2 bytes each), below the existing 8-bit zone at `$E8–$EC`.
  - ZP operand map upper bound tightened from `$EE` to `$DF` to reflect the
    new reserved zone; `zp_map_add` now enforces this with an address guard
    (previously only a count guard existed).
  - `EMIT_ARITH8(ROUTINE, RES_SLOT, NEEDS_FLAG)` /
    `EMIT_ARITH16(ROUTINE, RES_SLOT, NEEDS_FLAG)` — pair of local macros
    replacing the three verbose switch arms; bit width, arg-loading, and
    result-storing all collapse to one line per dispatch branch.
  - Emulator tests: `mul_u16` (300 × 13 = 3900), `mul_u16_wrap` (256 × 256 = 0,
    overflow), `div_u16` (50000 / `$C001` = 1, remainder = 847 — exercises the
    high-bit-divisor path).

- **Symbol table embedded in ROM** — compiled binaries now carry a `"C02S"`
  symbol table in the NOP fill area between the data section and `$FFF6`,
  letting `c02-objdump` show real function names instead of auto-generated
  labels. The table is always emitted by default; `--strip-debug` omits it.
  Binary size stays exactly 32 KB so EEPROM flashing is unaffected.
  - Footer layout: `$FFF6–$FFF7` = little-endian pointer to the table (or
    `$EAEA` NOP fill if absent); `$FFF8–$FFF9` = code/data boundary
    (unchanged); `$FFFA–$FFFF` = NMI/Reset/IRQ vectors.
  - Format: magic `C02S` (4 bytes) + u16 entry count (LE) + entries of
    u16 address (LE) + null-terminated name. All user-defined functions and
    emitted helpers (`__mul8`, `__div16`, etc.) are included.
  - Old binaries degrade gracefully: `$EAEA` at `$FFF6` passes the range
    check but fails the magic-byte check, so the disassembler falls back to
    `L0`/`L1`/… auto-labels without error.
  - `c02-objdump`: `parse_symbols` reads the table and merges it into the
    jump-target label map; `scan_end` for the data-section boundary scan
    stops at the symbol table start rather than `$FFF8` to avoid
    misidentifying table bytes as data.
  - `driver.h`: `params_t` gains `int strip_debug`; `main.c` adds
    `--strip-debug` to `long_options`.

- **ZP map overflow error propagation** — previously, `zp_map_add` printed a
  diagnostic to stderr and silently continued, potentially generating corrupt
  code. `zp_map_add`, `zp_map_add_operand`, and `zp_map_build` now all return
  `int` (0 = failure); `emit_function_from_cfg` checks `zp_map_build` and
  returns 0, propagating to `generate_rom` which returns `NULL` — the same
  path as all other codegen failures, ultimately exiting with
  `CODE_GEN_ERROR_RET_CODE` (7).

- **Bug fix: signed 8-bit division and modulo (`__sdiv8`)** — `TAC_DIV` and
  `TAC_MOD` on `i8` operands previously routed through the unsigned `__div8`
  helper, so `i8 -6 / 2` computed `250 / 2 = 125` instead of `-3`. A new
  `__sdiv8` helper wraps `__div8`: it saves operand signs into a scratch byte at
  `$EC` (HELPER_SIGN), negates both operands to their absolute values, calls
  `__div8`, then restores the correct sign on the quotient (bit 7 of SIGN) and
  remainder (bit 6 of SIGN) per C's truncation-toward-zero convention. Codegen
  routes `TAC_DIV`/`TAC_MOD` through `__sdiv8` when `is_signed_type(dst.type)`;
  `__sdiv8` always calls `__div8`, so `needs_sdiv8 = 1` implies `needs_div8 = 1`.
  New opcode emitters: `bpl_rel`. `$EC` added to the helper ZP zone.
- Emulator tests: `div_i8` (−6 / 2 = −3, PORTB = $FD), `mod_i8` (−7 % 2 = −1,
  PORTB = $FF).

- **Bug fix: binary op operand widening and sign normalisation** — binary ops
  derived both the result type and comparison signedness from the LEFT operand
  only (`ir.c`), ignoring the right operand entirely. Consequences: `u8 + u16`
  computed at 8 bits (the u16 high byte was silently dropped), while `u16 + u8`
  was accidentally correct; `i8 < u8` used a signed compare while `u8 < i8` used
  an unsigned compare — a trichotomy violation where both `a < b` and `b < a`
  could be simultaneously true. Fixed in the IR generator with four new helpers:
  - `ir_type_width` / `ir_is_signed` — predicates for 8-vs-16-bit and
    signedness without an `ir_gen_t *` context.
  - `binop_common_type(left, right)` — returns the wider type; for equal widths,
    unsigned wins (C's usual arithmetic conversions), eliminating operand-order
    dependence in mixed-sign comparisons.
  - `emit_widen_if_needed(gen, cfg, op, target)` — emits `TAC_CAST` when the
    operand type differs from the target; `OPERAND_CONST_INT` values are
    re-typed in place (no instruction emitted).
  - Binop lowering now normalises both operands to the common type before
    arithmetic and comparison ops. Shifts are guarded separately (result type =
    left, shift count is never widened). Pointer arithmetic skips widening.
- Emulator tests: `binop_widen` (`u8(1) + u16(500) = 501`, high byte = $01),
  `cmp_mixed_sign` (`i8(−1) < u8(100)` with unsigned-wins → $FF reinterpreted as
  255, 255 < 100 = false, branch not taken, PORTB = $01).

## [v0.2.15] 2026-06-26

- **Function call codegen (`TAC_CALL`)** — full caller/callee ABI using a fixed
  2-byte ZP slot per parameter in the `$EF–$FE` ABI zone. Caller copies each
  argument into its slot (`$EF`/`$F0` for arg 0, `$F1`/`$F2` for arg 1, etc.)
  via `emit_load_byte`, which zero-extends u8 args into the hi byte automatically.
  Callee prologue (`emit_function_prologue`) copies the ABI zone into the
  function's own ZP slots at function entry; no-arg functions (including `main`)
  skip the prologue entirely. Return values are already handled by the existing
  `TAC_RETURN`→`$02/$03` path; `TAC_CALL` copies `$02/$03` into the destination
  temp after `JSR`. Void calls skip the copy (`TYPE_VOID` guard). Max 8
  parameters enforced at codegen with a hard error. Note: signed narrower→wider
  widening at call sites (e.g. i8 arg into i16 param) is zero-extended, not
  sign-extended; a future IR pass should insert explicit `TAC_CAST` nodes for
  implicit widening in call arguments.
- **Callee-saves ZP preservation** — every function (except `main`) pushes all
  of its ZP slots onto the hardware stack at entry (via `PHA`) and pops them
  in reverse order before every `RTS` (via `PLA`). This ensures the caller's
  locals and temporaries survive across calls regardless of ZP slot overlap.
  As a direct consequence, bounded recursion is now supported — stack depth is
  limited to ≈256 / (function's ZP byte count) levels.
- Emulator tests: `func_call_u8` (add(10,32)=42), `func_call_void`
  (write_port(99)→PORTB=99), `func_call_u16` (sum16(200,300)=500, lo byte=244),
  `func_clobber` (x=5 survives callee overwriting its ZP slot; result=11),
  `func_recursive` (factorial(5)=120, exercises 5-level LIFO stack discipline).

- **Struct field access codegen (`TAC_FIELD_LOAD` / `TAC_FIELD_STORE`)** — field
  reads and writes for both by-value and pointer-to-struct operands. By-value
  structs use `emit_load_byte`/`emit_store_byte` with `field->offset + b` so
  global structs automatically get absolute addressing. Pointer-to-struct uses
  `LDY #(offset+b); LDA/STA ($ptr),Y` with RAM→ZP sync when the pointer is a
  global.
  - `full_type_size(e, type)` — struct-aware sizing that consults
    `ir_module_t.structs` for `TYPE_STRUCT`, replacing the silent `default:
    return 1` fallback. Used in ZP map allocation (`zp_map_add`),
    `allocate_globals`, `TAC_COPY`, and the new field ops. `zp_map_build` now
    takes `emitter_t *e` so struct sizes are available during map construction.
  - `lookup_struct_field(e, struct_name, field_name)` — field offset/type
    lookup from the IR module, used by both `TAC_FIELD_LOAD` and
    `TAC_FIELD_STORE`.
  - `ir_gen_t *gen` added to `emitter_t` so the module's struct table is
    reachable from all codegen paths.
- **Bug fix: `++field` / `--field` not writing back** — `NODE_INC`/`NODE_DEC`
  on a field-access target (`++str.val`) was loading the field into a temp,
  incrementing the temp, then silently discarding it. The field was never
  updated so the loop advanced zero steps each iteration. Fixed in the IR
  generator: field-access targets now emit `TAC_FIELD_LOAD → TAC_INC/DEC →
  TAC_FIELD_STORE` rather than loading a throwaway temp.
- **Pointer arithmetic (`ptr + int`, `ptr - int`)** — the analyzer's
  `NODE_BINOP` type-check now recognises pointer+integer as valid, returning
  the pointer type unchanged. The IR already lowered this to `TAC_ADD`; codegen
  and `TAC_LOAD` already handled the resulting pointer correctly, so no codegen
  changes were needed.
- **Zero-extension fix in `emit_load_byte` and `GLOBAL_AWARE_ALU_HELPER`** —
  both helpers were reading `ZP + byte` even when `byte >= operand_size`,
  pulling in whatever occupies the adjacent ZP slot as a phantom high byte.
  Classic symptom: `u8 i` used as an index into a `u8*` pointer computes
  `ptr + i + (adjacent_slot × 256)` — always a wrong address, always the same
  garbage byte. Both paths now emit `LDA #0` / `IMM_FN(e, 0)` for
  out-of-range byte indices. `OPERAND_CONST_INT` was already correct via
  shift+mask. Fixes `lcd_hello_world_simplified.c02` printing one garbage
  character in a loop instead of "Hello C02!".
- Emulator tests: `field_local`, `field_global`, `field_ptr`, `lcd_simplified`
  (verifies all 10 PORTB writes match "Hello C02!" in order).

## [v0.2.14] 2026-06-26

- **`TAC_ADDR_OF` (`&x`)** — address-of operator codegen. Globals resolve to
  their RAM address (`g->ram_addr`); locals/temporaries resolve to their ZP
  slot address. The 16-bit address is stored into the destination via two
  `LDA imm; STA zpg` pairs (lo byte then hi byte).
- **`TAC_STORE` pointer destination (`*p = val`)** — variable-destination
  pointer stores now emit byte-wise `LDA src; STA ($ptr),Y` indirect indexed
  writes. The pointer's ZP slot is kept in sync from its RAM address before
  indirect access when the pointer is a global.
- **Bitwise ops (`TAC_BAND`, `TAC_BOR`, `TAC_BXOR`, `TAC_BNOT`)** — width-aware
  byte loops. `AND`/`ORA`/`EOR` use global-aware helpers (`emit_and_byte`,
  `emit_ora_byte`, `emit_eor_byte`) dispatching imm/zpg/abs variants.
  `TAC_BNOT` byte-loop `EOR #$FF`s each byte. New opcode emitters: `and_imm`,
  `and_zpg`, `and_abs`, `eor_zpg`, `eor_abs`.
- **Shift ops (`TAC_SHL`, `TAC_SHR`)** — both constant-count and variable-count
  variants. Constant shifts unroll `ASL/ROL` (left) or `LSR/ROR` (right) pairs
  per byte. Variable shifts use an X-register counter loop with hardcoded
  relative branch offsets derived from fixed loop body sizes. Signed right shift
  (`i8`/`i16`) uses the `CMP #$80; ROR` pattern — CMP sets carry = sign bit,
  ROR shifts it in as the new MSB. New opcode emitters: `asl_zpg`, `rol_zpg`,
  `lsr_zpg`, `ror_zpg`, `tax`, `dex`, `bcs_rel`, `bcc_rel`.
- **`TAC_CAST` (type cast codegen)** — same-width copies, narrowing (low bytes
  only), and widening with zero-extension (u8→u16) or sign-extension via
  `CMP #$80; LDA #$FF; BCS +2; LDA #0` (i8→i16).
- **`TAC_COPY` implicit widening fix** — `TAC_COPY` previously used the
  destination size for all byte indices, reading garbage from adjacent ZP slots
  when widening (e.g. u8→u16). Now computes `src_size`, `dst_size`, and
  `copy_size = min(src_size, dst_size)`, then zero/sign-extends the remaining
  bytes when `dst_size > src_size`.
- **`TAC_MUL`, `TAC_DIV`, `TAC_MOD` software helpers** — 8-bit
  multiply/divide/modulo via subroutines `__mul8` (shift-and-add) and `__div8`
  (binary long-division). Arguments pass through fixed ZP slots `$E8`/`$E9`;
  quotient/product at `$EA`, remainder at `$EB`. Helpers use lazy emission:
  `needs_mul8`/`needs_div8` flags are set during CFG walk; helpers are emitted
  after all functions and registered as function labels so the existing JSR
  fixup system resolves their addresses. `TAC_DIV` and `TAC_MOD` share
  `__div8`, reading `$EA` vs. `$EB` for the result.
- **ZP arithmetic helper zone** — `$E8–$EB` carved from the scratch register
  range and reserved for `__mul8`/`__div8` argument slots. Scratch registers
  now span `$04–$E7`. `$EC–$EE` reserved for future helpers. README and ZP
  layout table updated.
- **LOC table reformatted** — `test.py --cloc` output is now a single
  tree-style ASCII table with `├─`/`└─` prefixes, aligned columns, and a grand
  total footer. Adding a new toolchain component = one `Section(...)` entry.
- Emulator tests: `addr_of_local`, `addr_of_global`, `ptr_store_local`,
  `ptr_store_global`, `bitwise_and`, `bitwise_or`, `bitwise_xor`,
  `bitwise_not`, `shl_const`, `shr_const`, `shr_signed`, `shl_var`,
  `implicit_widen`, `mul_u8` (7×6=42), `div_u8` (100÷7=14), `mod_u8`
  (100%7=2).

## [v.0.2.13] 2026-06-24

- **Codegen diagnostic for unhandled TAC ops** — the `default: break` in the
  TAC instruction switch has been replaced with a `fprintf(stderr)` + error
  return that names the unhandled op number. Programs using unimplemented
  features now fail loudly at compile time instead of silently producing wrong
  binaries. `emit_function_from_cfg` now returns `int` (0 on failure) so the
  error propagates to `generate_rom`.
- **Bootstrap emu tests decoupled from analyzer_basic.c02** — the eight
  bootstrap-verification tests (rom_size, reset_vector, etc.) now use
  `emu_store_const.c02` instead of `analyzer_basic.c02`, which uses TAC ops
  not yet implemented in the code generator.
- **Fix global/ZP bug class** — `TAC_INC`/`TAC_DEC` on global variables now
  emit `INC abs` ($EE) / `DEC abs` ($CE) targeting the global's RAM address
  instead of the stale ZP scratch slot. 16-bit global INC/DEC uses `BNE +3`
  (3-byte abs instruction) instead of `+2`. `COMPARE_OP` right-hand operands
  now route through a global-aware `emit_cmp_byte` helper that dispatches
  `CMP abs` ($CD) for globals. New opcode emitters: `inc_abs`, `dec_abs`,
  `cmp_abs`.
- Emulator tests: `inc_global` (global u8 increment), `cmp_global` (compare
  local against global with branch).
- **u8 arithmetic (`TAC_ADD`, `TAC_SUB`)** — `CLC; LDA src1; ADC src2; STA dst`
  for addition, `SEC; LDA src1; SBC src2; STA dst` for subtraction. Global-aware
  RHS helpers (`emit_adc_byte`, `emit_sbc_byte`) dispatch ADC/SBC abs ($6D/$ED)
  for globals. New opcode emitters: `clc` ($18), `sec` ($38), `adc_imm` ($69),
  `adc_zpg` ($65), `adc_abs` ($6D), `sbc_imm` ($E9), `sbc_zpg` ($E5),
  `sbc_abs` ($ED). CLC/SEC is emitted once outside the byte loop so u16
  carry propagation works correctly.
- Emulator tests: `add_u8`, `sub_u8`, `add_const`.
- **`TAC_NEG` (unary minus)** — `SEC; LDA #0; SBC [src1]; STA [dst]`. SEC is
  emitted once before the byte loop so borrow propagates correctly for u16.
- Emulator test: `neg_u8` (double negate round-trips back to original value).
- **u16 comparisons** — `COMPARE_OP` macro replaced with backpatched
  comparison handlers that support both u8 and u16 operands. u16 ordering
  (LT/GTE/GT/LTE) uses a high-byte-first pattern: compare high bytes first
  to determine definite ordering, fall through to low bytes when equal.
  u16 EQ/NEQ compare both bytes. All forward branch offsets use backpatching
  instead of hardcoded values, making them robust to variable-size loads
  (zpg=2 vs abs=3 vs imm=2).
- Emulator tests: `cmp_u16_lt` (255<256, high-byte decides), `cmp_u16_eq`
  (500==500, both bytes match), `cmp_u16_gt` (1000>255, high-byte decides).
- **Signed comparisons (i8/i16)** — ordering comparisons (LT/GTE/GT/LTE) now
  detect signed operand types and emit the N XOR V pattern: `SEC; SBC; BVC +2;
  EOR #$80; BMI true` for i8, with an extended high-byte-first pattern for
  i16 (signed high byte via N^V, unsigned low byte fallback via BCC). EQ/NEQ
  remain sign-agnostic. GTE inverts the LT result. GT/LTE swap operands.
  New opcode emitter: `bvc_rel` ($50).
- Emulator tests: `cmp_i8_lt` (-5<3), `cmp_i8_gt` (3>-5), `cmp_i8_neg`
  (-10<-3), `cmp_i16_signed` (-300<300).
- **u16 arithmetic** — `TAC_ADD`/`TAC_SUB` are width-aware from the start
  (CLC/SEC outside byte loop, carry propagates between bytes). Signed i8/i16
  arithmetic works via two's complement — same ADC/SBC instructions.
- Emulator tests: `add_u16` (300+200=500), `sub_u16` (1000-500=500),
  `add_i8` (-5+47=42), `add_i16` (-300+800=500).

## [v0.2.12] 2026-06-23

- **c02-objdump: label markers** — jump target labels (`L0:`, `L1:`, ...) now
  print at the correct positions in the disassembly. The reset vector is read
  to translate between ROM buffer offsets and absolute addresses.
- **c02-objdump: full opcode size table** — the label pre-scan uses a 256-entry
  instruction size table covering the entire 65C02 instruction set, replacing
  the previous partial list that would lose sync on unrecognized opcodes.
- **c02-objdump: section-aware output** — the code generator now writes a
  code/data boundary address at `$FFF8` in the ROM. The disassembler reads
  this to stop before the data section, with a NOP-fill heuristic fallback
  for older binaries.
- **c02-objdump: new CLI flags** —
  `-a` / `--all` shows disassembly followed by a `.data` hex dump with ASCII.
  `-d` / `--data` dumps just the `.data` section.
  `-s` / `--sections` prints the section layout (`.text`, `.data`, vectors
  with addresses).
  `-S` / `--size` prints an Arduino-style ROM usage summary with `.text`,
  `.data`, and vectors breakdown.

## [v0.2.11] 2026-06-23

- **Implicit void return in IR** — `lower_function` now emits a trailing
  `TAC_RETURN` when the last instruction isn't already a return, so void
  functions without an explicit `return` produce correct IR.
- **Data section & global variable support** — global variables are allocated
  RAM addresses ($0200 upward) and initialized in the bootstrap before
  `JSR main`. String literals are placed in ROM after all function code.
  A `data_fixup_t` backpatching system resolves string ROM addresses into
  the bootstrap init code after the data section is emitted.
  - `allocate_globals` assigns RAM addresses with type-aware stride.
  - `emit_global_init` emits `LDA #imm; STA abs` per global, with fixup
    placeholders for string-initialized pointers.
  - `emit_data_section` writes null-terminated string bytes into ROM and
    resolves all data fixups.
  - `emit_load_byte` / `emit_store_byte` detect globals and use absolute
    addressing (`LDA abs` / `STA abs`) instead of zero-page.
  - Bootstrap split into `emit_bootstrap` + `emit_call_main` so global init
    code runs between hardware setup and `JSR main`.
- **`TAC_LOAD` (pointer dereference & register reads)** — pointer dereference
  via `LDA ($nn),Y` indirect indexed addressing. Hardware register reads use
  `LDA abs`. Global pointers are copied from RAM to their ZP slot before
  indirect access.
- **16-bit `TAC_INC` / `TAC_DEC`** — `INC zpg; BNE +2; INC zpg+1` for
  pointers and u16 values. DEC uses `LDA zpg; BNE +2; DEC zpg+1; DEC zpg`
  to propagate borrow.
- New opcode emitters: `lda_abs` ($AD), `lda_ind_y` ($B1), `ldy_imm` ($A0),
  `bne_rel` ($D0).
- Emulator test: `string_deref` — global string pointer, loop with `*p`
  dereference and 16-bit `++p`, verifies correct characters reach PORTB.
- **Hardware verified** — `lcd_hello_world_simplified.c02` prints "Hello C02!" on a
  real 65C02 breadboard with HD44780 LCD.

## [v0.2.10] 2026-06-23

- **Line count: disassembler section** — refactored `count_lines` into a
  reusable `count_lines_in(dir, exts)` helper and added a Disassembler section
  for c02-objdump (Rust + Makefile). Also added `target` to `IGNORED_DIRS` to
  exclude Cargo build artifacts.
- **Control flow codegen** — complete control flow generation for the
  65C02 target, enabling `for`, `while`, and `if`/`else` to compile and run on
  real hardware.
  - **Local label system** — `TAC_LABEL` records label addresses during emission;
    `TAC_JUMP` emits `JMP abs` with backward-ref direct patching or forward-ref
    backpatching via `local_fixups`, resolved at the end of each function.
  - **`TAC_COND_JUMP`** — inverted-branch-over-JMP pattern (`BEQ +3; JMP target`)
    gives unlimited jump range from a 1-byte boolean source.
  - **`TAC_NOT`** — boolean negation via `EOR #$01`.
  - **All six comparison ops** — `TAC_LT`, `TAC_GTE`, `TAC_EQ`, `TAC_NEQ`,
    `TAC_GT`, `TAC_LTE` via a `COMPARE_OP` macro that stamps out the
    `CMP`/branch/`LDA` sequence with a single branch-opcode parameter. `GT` and
    `LTE` swap operands to reuse `LT`/`GTE` logic. Uses a branch-before-load
    pattern to avoid the 6502 `LDA #0` clobbering the Zero flag before
    `BEQ`/`BNE`.
  - **`TAC_INC` / `TAC_DEC`** — in-place `INC zpg` / `DEC zpg` for u8 variables.
  - New opcode emitters: `beq_rel`, `cmp_imm`, `cmp_zpg`, `eor_imm`, `inc_zpg`,
    `dec_zpg`.
  - Emulator tests: `forward_jump`, `cmp_gt`, `cmp_gte`, `cmp_eq`, `cmp_neq`,
    `cmp_lte`.
  - **Hardware verified** — `led_counter.c02` (nested while + for loop cycling PORTB
    through 0–254) compiled and flashed to real 65C02 breadboard.

### Known Limitations

- **Comparisons are u8-only** — `COMPARE_OP` loads only byte 0 of each operand.
  A u16 comparison will silently compare only the low byte, giving wrong results
  when values differ in the high byte.
- **INC/DEC are u8-only** — `INC zpg` / `DEC zpg` operate on a single byte with
  no carry into a high byte. Incrementing a u16 past `$00FF` or decrementing
  below `$0100` will wrap the low byte without touching the high byte.
- **Comparisons are unsigned-only** — the `CMP` + carry-flag branch sequence
  implements unsigned ordering. Signed comparisons (i8/i16) require checking
  the Negative and Overflow flags (`N ⊕ V`), which needs a different branch
  sequence not yet implemented.

## [0.2.9] 2026-06-22

- **CFG walk** — `generate_rom()` now iterates over `ir_module_t.cfgs` and
  emits real function bodies from the TAC instruction stream, replacing the
  previous stub main.
- **Zero-page operand map** — per-function allocation table that assigns each
  variable and temporary a zero-page slot ($04 upward), striding by type size
  (1 byte for u8/i8, 2 bytes for u16/i16/pointers). Params are seeded first
  from the CFG's parameter list, then locals and temps are collected from all
  instructions.
- **TAC_COPY** — variable/temporary assignment via `LDA`/`STA` through the
  operand map, with width-aware byte loops for 16-bit types.
- **TAC_STORE** — writes to absolute addresses (hardware registers), supporting
  both constant and variable sources with multi-byte emission for wider types.
- **TAC_RETURN** — emits `RTS` for void returns; for value returns, copies the
  result into the RET register ($02/$03) with width derived from the function's
  return type signature.
- **`emit_load_byte` helper** — byte-indexed operand loader that handles
  constants (shift + mask), variables, and temporaries uniformly, used by all
  TAC ops to avoid duplicating width logic.
- New opcode emitters: `lda_zpg` ($A5), `sta_abs` ($8D).
- Added emulator tests: `store_const_to_abs`, `copy_var_to_abs`,
  `u16_copy_and_return`, plus the existing py65 bootstrap tests.

## [0.2.8] 2026-06-22

- **Driver refactor** — extracted the compilation pipeline from `main.c` into
  `driver.c`/`driver.h`. Each stage (file loading, frontend, IR, codegen) is
  now a separate function chained by return-code checks, replacing the previous
  `goto finish` control flow. `main.c` is now just CLI parsing and the timing
  report.
- **Code generation stub** — added `generate_rom()` entry point in
  `src/code-gen/generator.c` with `emitter_t` struct for flat ROM buffer
  output. Dump flags (`--ast-dump`, `--symbol-dump`, `--ir-dump`) now skip
  codegen since they are for inspecting compiler internals, not building
  binaries.
- **Bootstrap runtime** — the code generator emits a 65C02 reset stub at the
  start of ROM: `SEI`, `CLD`, hardware stack init (`$01FF`), frame pointer
  init (`FP` at ZP `$00`), `JSR main`, and an infinite halt loop. Interrupt
  vectors are written at `$FFFA–$FFFF` with the reset vector pointing to
  ROM start.
- **Label resolution and fixup system** — function calls (`JSR`) record a
  fixup with a placeholder address at emit time. Function entry points are
  registered in a label table as they are emitted. After all code is emitted,
  fixups are resolved by patching the placeholder addresses. A parallel
  per-function system (`local_labels` / `local_fixups`) is in place for
  control-flow labels (`TAC_LABEL` / `TAC_JUMP` / `TAC_COND_JUMP`).
- **Opcode emitter macros** — `OP_EMITTER_NO_ARG`, `OP_EMITTER_SINGLE_ARG`,
  and `OP_EMITTER_ABS` generate typed emit functions from an opcode constant,
  keeping the codegen readable without raw hex throughout.
- **Zero-page layout revised** — scratch registers now span `$04–$EE`
  (compiler-managed temporaries, locals, and globals), with `$EF–$FF`
  reserved for function ABI parameter passing. The previous user-space
  carve-out (`$30–$FF`) is removed; all variable placement is
  compiler-managed.

## [0.2.7] 2026-06-22

- **Forward declarations (`decl`)** — added `decl fn name(...) -> type;` and
  `decl type name;` syntax for declaring functions and globals defined in other
  translation units. Forward declarations are registered in the symbol table
  (redeclaration in the same file is an error), validated in the type-checking
  pass, and collected into `ir_module_t.externs` for the linker. Extern symbols
  are fully serialized in the `.o` format (IR_VERSION bumped to 2).

## [0.2.6] 2026-06-22

### Fixed

- **Register reads now lower to `TAC_LOAD`** — reading a hardware register
  (e.g. `x = PORTA`) previously leaked the register name as a plain variable
  in the IR. The `NODE_IDENTIFIER` case in `lower_expr` now mirrors the
  existing register-write path: it looks up the name in `module.regs` and
  emits a `TAC_LOAD` from the fixed hardware address.
- **Out-of-order struct declarations rejected** — a by-value struct field
  referencing a struct declared later in the source (or referencing itself)
  previously produced silently wrong field offsets. The analyzer now checks
  that every by-value `TYPE_STRUCT` field names a struct already declared
  earlier in the source, and rejects self-referential structs with a clear
  diagnostic. New error kind `ERR_INCOMPLETE_STRUCT_FIELD`.
- **`&&` and `||` now short-circuit** — logical AND/OR previously lowered as
  flat binary TAC ops that unconditionally evaluated both operands. On a
  memory-mapped 6502 target this is a semantic bug (e.g. `flag && *p` would
  dereference `p` even when `flag` is false). Both operators now lower to
  conditional jumps so the right-hand side is only evaluated when needed.
- **Number literal types match the analyzer** — `NODE_NUMBER` previously
  recomputed its type in IR gen (`<= 0xFF → u8, else → u16`), discarding
  the analyzer's `resolved_type`. Negated literals like `-5` appeared as
  `u8` instead of `i8`. `NODE_NUMBER` now carries a `resolved_type` field
  stamped during semantic analysis, and IR gen uses it directly.

## [0.2.5] 2026-06-22

### Added

- **IR generation (in progress)** — new `src/ir-gen/` module for lowering the
  analysed AST into a self-contained intermediate representation:
  - `ir_module_t`: a complete IR output containing struct layouts (with
    computed field offsets and sizes), global variable declarations, register
    definitions, and one CFG per function — designed so codegen never needs to
    consult the AST or symbol table.
  - Pass 1 (declaration collection): walks top-level declarations and
    populates register definitions (name, type, hardware address), global
    variables (with integer or string initialiser support), and struct layouts
    with sequential field offsets.
  - Pass 2 (function lowering): complete expression lowering into TAC —
    numbers, strings, identifiers, binary ops, unary ops (including
    `TAC_INC`/`TAC_DEC` mapped to 6502 `INC`/`DEC`), address-of, pointer
    dereference (`TAC_LOAD`), casts, function calls with arguments, struct
    field access (`TAC_FIELD_LOAD`), and struct initialiser literals
    (`TAC_FIELD_STORE` per field).
  - Complete statement lowering: variable declarations, assignments
    (to variables, pointer derefs, struct fields, and hardware registers
    via `TAC_STORE` at fixed addresses), `return`, `if`/`else if`/`else`
    (with negate-and-skip conditional jumps), `while` loops, `for` loops
    (with optional init/cond/increment), and nested blocks.
  - `--ir-dump` CLI flag for inspecting the IR module after lowering,
    with full TAC instruction printer showing readable output for all
    instruction types (hex addresses for register stores, labelled
    jumps for control flow).
  - IR generation timing integrated into `--time-report`.
  - Test harness properly generates goldens for ir test files.
- **Incremental compilation** (`-c` flag) — compiles a `.c02` source file
  through the full frontend and IR generation, then serializes the
  `ir_module_t` to a binary `.o` file. The `.o` can be loaded back with
  `cc02 file.o` to skip the frontend entirely and resume from the IR.
  - Binary format uses length-prefixed strings, a magic header (`C02I` v1),
    and allocates all data from the arena on read — no dangling pointers,
    `ir_gen_free` works uniformly regardless of whether the IR came from
    source or a `.o` file.
  - `-o` flag specifies the output path (defaults to `a.o`).
- Smoke tests for IR generation covering the full pipeline (tokenize →
  parse → analyse → IR gen → free) with assertion coverage for
  declarations, CFG creation, expression lowering, statement lowering
  (var decl, register store, if/else, while, for), and serialization
  round-trip (write to `.o`, free all source data, read back, verify
  contents match).
- Per-module line count breakdown in `test.py --cloc` output.

### Changed

- `NODE_IDENTIFIER` refactored from a bare `char *` to a struct carrying
  the name and a `resolved_type` stamped by the analyzer during semantic
  analysis. Required for IR generation to know variable types without
  re-walking scopes.
- `NODE_CALL` extended with `resolved_return_type`, stamped by the analyzer.
- `NODE_FIELD_ACCESS` extended with `resolved_type`, stamped by the analyzer.

## [0.2.0] - 2026-06-21

### Added

- **Semantic analysis** — two-pass analyzer that validates the full AST:
  - Pass 1: registers all top-level declarations (functions, structs,
    registers, global variables) into the global symbol table with
    redeclaration checking.
  - Pass 2: recursive walk of function bodies with scoped symbol tables,
    checking for undeclared identifiers, type mismatches, wrong argument
    counts/types, unknown struct fields, and redeclarations.
  - `resolve_expr_type()` for full expression type resolution: literals
    (with smallest-fitting integer type), identifiers, function calls,
    binary/unary operators, dereferences, address-of, casts, field access,
    and struct initializers.
  - Integer widening: `u8` → `u16` is allowed implicitly; narrowing
    requires an explicit cast.
  - `null`/`0` is compatible with both pointer and integer types.
  - For-loops get their own scope so loop variables don't leak.
  - `main` function existence check after pass 1.
- `analyzer_print.c` and `--symbol-dump` CLI flag for printing the
  global symbol table after analysis.
- `analyzer_t` struct owning its own arena (mirroring `parser_t`), with
  `analyzer_init()`/`analyzer_free()` lifecycle.
- Smoke tests for the analyzer's scope stack, symbol insertion, lookup,
  and shadowing behavior.
- Compiler test cases covering analyzer error paths (undeclared
  identifiers, type mismatches, call errors, struct errors) and
  success paths (scoping, widening, pointers, basic analysis).
- **Analyzer hardening** — additional validation closing gaps where invalid
  programs were previously accepted silently:
  - Named struct types are checked for existence in *every* declaration form
    (locals, parameters, globals, registers, struct fields, function return
    types), not just struct initializers and field access. A symbol whose
    declared type is invalid is poisoned, so later uses don't cascade
    duplicate diagnostics.
  - A non-pointer `void` is rejected as a variable/parameter/field/global
    type (`void*` remains valid as a null pointer).
  - Integer literals that don't fit any supported type are now an error
    (previously swallowed); the lexer rejects literals that overflow `long`.
    Negative literals are typed from their value (`-5` is `i8`, `-300` is
    `i16`).
  - Assignment targets and the operands of `&`, `++`, and `--` must be
    lvalues.
  - Global variable initializers are type-checked (resolved in the global
    scope), just like local declarations - previously they were ignored.
  - A bare `return;` in a non-void function is rejected, and a non-void
    function that can fall off its end without returning a value is flagged.
    (This last check is shallow: a function ending in a control-flow statement
    — e.g. a one-armed `if` that falls through, or an `if`/`else` where only
    some branches return — is assumed to return and is not flagged. Full
    path-coverage analysis is future work.)
- Struct-typed globals (`Point p;` at file scope) now parse, matching the
  form used for locals.
- Field access auto-dereferences a single-level struct pointer (`c.field` on
  a `Struct*`), since there is no `->` operator.
- A negative-test corpus exercising each new check, plus positive tests for
  pointer field auto-deref and negative-literal typing.

### Changed

- Renamed `scope_stack_t` → `analyzer_t` and all associated functions
  to `analyzer_*` prefix for consistency with `parser_t`.
- Consolidated cleanup in `main.c` into a single `goto finish` exit path,
  eliminating repeated free-lists at each error point.
- Test harness now routes `--ast-dump` to parser tests and `--symbol-dump`
  to analyzer tests based on filename prefix.
- Refactored program error codes so each failure stage gets its own exit code.
- Generalized error handling and printing for parsing onwards
  (including sem. analysis). Moved source location tracking to a
  dedicated type, `token_location_t` to support pretty error messages
  after tokens aren't directly accessible.
- Updated test harness to run smoke binaries and check for memory leaks.
- Generalized the arena allocator out of `parser.c` into shared
  infrastructure, so semantic analysis can reuse it for its own
  allocations.
- Diagnostics now print the full pointer depth (`u16**`, not `u16*`) and name
  the actual type in field-access-on-non-struct errors; a malformed number
  literal and a repeated unknown-type are each reported once rather than per
  character / per use.

### Fixed

- Fixed a segfault when a parse error's offending token was a numeric literal.
- Fixed a memory leak in the arena allocator when a standard chunk allocation
  followed an oversized one.
- String literals with escape sequences no longer drop a trailing character
  per escape, and common escapes (`\n`, `\t`, `\\`, `\"`, ...) are decoded.

## [0.1.1] - 2026-06-20

No code changes - this release formalizes the project's licensing and
contribution process.

### Added

- `LICENSE`: GPLv3, with a compiler-output exception so programs compiled
  *with* C02 are not themselves subject to GPL terms.
- `CONTRIBUTING.md` documenting the branch naming, PR, and changelog
  conventions for the project.
- "Current Status & Limitations" section in the README, consolidating what's
  implemented vs. not in one place.
- Third-party license attribution for `c02-objdump`'s `clap` dependency
  (dual-licensed MIT/Apache-2.0).

## [0.1.0] - 2026-06-20

Initial public release. **Frontend only** - tokenizer, parser, and AST
printer. No semantic analysis or code generation yet; see the README's
"Current Status & Limitations" section for the full list of what's not
implemented.

### Added

- Source-tracking tokenizer with file/line/column on every token.
- Recursive descent parser covering:
  - Full expression precedence chain: `||  &&  |  ^  &  ==  !=  <  >  <=  >=  <<  >>  +  -  *  /  %`, plus unary `! - & ~ ++ -- * @`.
  - Statements: variable declarations, assignment (including compound `+= -= *= /= %=`) to any lvalue (identifier, field access, or dereference), `return`, `if` / `else if` / `else`, `while`, `for` (with optional clauses), function calls.
  - Top-level declarations: `fn`, `reg` (hardware register pinned to an absolute address), `struct`, and global variables.
  - Structs: field declarations, chained field access (`a.b.c`), and designated-initializer struct literals (`Point { .x = 0, .y = 0 }`).
  - C-style casts (`(u16)x`) and grouped expressions.
- Arena allocator for AST nodes, with growable scratch buffers for variable-length lists (statements, params, struct fields, call args) committed into the arena on completion.
- Clang-style error reporting: colorized output, caret-span source highlighting, and "expected / context" diagnostics for every parse error.
- AST printer (`--ast-dump`) producing a readable tree view of any parsed program.
- Golden-file test suite (Python harness) covering bitwise ops, conditionals, global vars, registers, and struct declarations.
- `--token-dump`, `--time-report`, and `--syntax-check-only` CLI flags.
- `c02-objdump`, a companion disassembler for decoding compiled `.bin` files back into annotated 65C02 assembly.

### Known Limitations

- No code generation: `cc02` does not yet produce a working 65C02 binary.
- No array type or subscript syntax.
- `->` is not implemented; field access through a pointer is intended to auto-dereference via semantic analysis once that exists.
