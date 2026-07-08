# Known Deviations — c02 vs. `SPEC.md`

**Status:** pinned to [jackwthake/C02](https://github.com/jackwthake/C02) commit
`9a9375e` (2026-07-02, branch `v1.1`), same baseline as `SPEC.md`.

This is the living half of the c02 language documentation — `SPEC.md` is the
stable normative contract; this file is the known-issues oracle a fuzz
harness should consult before treating a finding as *new*. Every entry here
is a place where `cc02`'s actual behavior diverges from what `SPEC.md`
specifies. If a fuzz run reproduces one of these, it's confirming a known
issue, not discovering one — only a *behavior change* from what's recorded
here (a deviation that no longer reproduces, or reproduces differently) is
noteworthy.

**Verification level** is marked per entry:
- **Executed** — reproduced by compiling (and, for the P-numbered items,
  running in the py65 emulator) the exact snippet shown, either while
  authoring this document or via upstream `cc02/tests/bug_test.py`
  (`python3 cc02/tests/bug_test.py` from the `C02` checkout — 23 of 24
  P-numbered regression tests are currently red; only P2-5 passes, and only
  by the coincidental memory-layout luck that test itself documents as
  non-robust).
- **Source** — derived from direct reading of `tokenizer.c` / `parser.c` /
  `analyzer.c`, not independently executed while authoring this document.
  Treat these as high-confidence but unconfirmed; re-verify before relying
  on one for a fuzzer's pass/fail oracle, especially if `cc02` changes.

Severity legend:
- **P0** — silent miscompile: analysis accepts the program, wrong machine
  code is emitted, no diagnostic anywhere.
- **P1** — loud failure: the compiler crashes or otherwise fails unsafely on
  a reasonable program.
- **P2** — robustness: a real gap, but latent or requiring an unusual
  program to trigger.
- **S** — semantic laxity: the analyzer accepts something the current spec
  (`SPEC.md`) already requires it to reject or check — an implementation
  gap in `cc02`, not a rule the spec leaves intentionally permissive.
- **G** — grammar quirk: a parser/lexer-level surprise, not itself
  incorrect, but easy to trip over or silently discard information.

## Quick Reference

| ID | Sev | Verified | Summary | Spec section |
|---|---|---|---|---|
| [G-1](#g-1-unterminated-block-comment-silently-swallowed) | G | Executed | Unterminated `/* ` swallows to EOF, no lexer error | [§1.5](SPEC.md#15-comments) |
| [G-2](#g-2-decl-fn--interrupt-drops-the-qualifier) | G | Executed | `decl fn ... interrupt` parses but the qualifier is unrepresentable in the AST | [§4.6](SPEC.md#46-forward-declarations-decl) |
| [G-3](#g-3-for-init-cannot-reuse-an-existing-variable) | G | Executed | `for (i = 0; ...)` reusing an existing var is a parse error | [§5.5](SPEC.md#55-for-loop-clauses) |
| [G-4](#g-4-for-init-and-block-statements-disambiguate-differently) | G | Source | `for`-init and block-statement disambiguation use different mechanisms for the same shape | [§5.5](SPEC.md#55-for-loop-clauses), [§7.1](SPEC.md#71-statementdeclaration-disambiguation) |
| [G-5](#g-5-struct-name-shadowing-misparses-a-cast) | G | Source | A local var shadowing a struct name misparses `(Name) - 1` as a cast | [§6.6](SPEC.md#66-casts-vs-grouped-expressions) |
| [G-6](#g-6-binary-literals-undocumented-upstream) | G | Source | `0b`/`0B` literals exist, undocumented upstream (informational) | [§1.3](SPEC.md#13-integer-literals) |
| [P0-1](#p0-1-nested-struct-field-writes-are-lost) | P0 | Executed (bug_test.py) | Writes to nested struct fields never reach the object | n/a (codegen) |
| [P0-2](#p0-2-cast-binds-to-the-whole-following-expression) | P0 | Executed (bug_test.py) | `(u8)w / 2` computes `(u8)(w / 2)`, not `((u8)w) / 2` | [§6.6](SPEC.md#66-casts-vs-grouped-expressions) |
| [P0-3](#p0-3-narrow-store-through-wide-pointerregister-leaves-stale-high-byte) | P0 | Executed (bug_test.py) | Storing a narrow value through a wide pointer/register leaves the high byte stale | n/a (codegen) |
| [P0-4](#p0-4-increment-through-deref-or-register-doesnt-store-back) | P0 | Executed (bug_test.py) | `++*p` / `++REG` modify a temp and never store back | [§6.2](SPEC.md#62-unary-prefix-operators) |
| [P0-5](#p0-5-non-literal-global-initializers-are-silently-dropped) | P0 | Executed (bug_test.py) | Global initializers other than a bare literal are silently dropped | [§4.5](SPEC.md#45-global-variables) |
| [P0-6](#p0-6-by-value-struct-paramsreturns-are-truncated) | P0 | Executed (bug_test.py) | By-value struct params/returns truncated to ≤2 bytes | n/a (codegen) |
| [P0-7](#p0-7-writes-through-a-pointer-to-a-callers-local-are-undone-on-return) | P0 | Executed (bug_test.py) | A callee's write through `&callerLocal` is undone by its own epilogue | [§6.2](SPEC.md#62-unary-prefix-operators) |
| [P0-8](#p0-8-narrow-signed-return-zero-extends-instead-of-sign-extending) | P0 | Executed (bug_test.py) | Returning a narrow signed value from a wider-signed-return function zero-extends | n/a (codegen) |
| [P0-9](#p0-9-interrupt-handlers-can-corrupt-mainline-computation) | P0 | Executed (bug_test.py) | A handler that calls a function or does `*`/`/`/`%` can corrupt interrupted mainline state | [§4.2](SPEC.md#42-interrupt-functions) |
| [P1-1](#p1-1-address-of-a-struct-field-segfaults-the-compiler) | P1 | Executed (bug_test.py) | `&expr.field` / `&*p` segfaults `cc02` | [§7.3](SPEC.md#73-lvalues) |
| [P2-1](#p2-1-reg-addresses-above-0xffff-are-silently-truncated) | P2 | Executed (bug_test.py) | `reg` address `> 0xFFFF` truncated, not rejected | [§4.3](SPEC.md#43-registers-reg) |
| [P2-2](#p2-2-struct-values-accepted-as-arithmeticcondition-operands) | P2 | Executed (bug_test.py) | Struct values accepted as arithmetic operands and as `if`/`while` conditions | [§5.4](SPEC.md#54-control-flow), [§6.3](SPEC.md#63-binary-operators) |
| [P2-3](#p2-3-interrupt-handlers-can-be-called-directly) | P2 | Executed (bug_test.py) | `irq()` callable directly, corrupting the stack (`RTI` vs `RTS` mismatch) | [§4.2](SPEC.md#42-interrupt-functions) |
| [P2-4](#p2-4-separators-are-optional-not-required) | P2 | Executed (bug_test.py) | Commas between args/params/init-fields are optional | [§5.2](SPEC.md#52-struct-initializer-expressions), [§6.5](SPEC.md#65-calls) |
| [P2-5](#p2-5-deep-loop-nesting-overflows-a-fixed-stack) | P2 | Executed (bug_test.py) | 65+ nested loops overflow a fixed 64-entry stack (currently self-consistent, doesn't visibly fail) | n/a (codegen) |
| [P2-6](#p2-6-global-allocation-never-checks-the-ram-ceiling) | P2 | Executed (bug_test.py) | Enough globals silently allocate past available RAM | n/a (codegen) |
| [P2-7](#p2-7-pointer-arithmetic-is-unscaled) | P2 | Executed (bug_test.py) | Pointer arithmetic always steps by 1 byte regardless of pointee size | [§6.3](SPEC.md#63-binary-operators) |
| [S-1](#s-1-voidnull-literal-conflation) | S | Executed | Any `void*` *value*, not just literal `0`, is compatible with any destination type | [§3.2](SPEC.md#32-type-compatibility) |
| [S-2](#s-2-no-signedness-checking) | S | Executed | Width-only check, no signedness comparison at all — same-width (`i8`↔`u8`) and cross-width (`i8`→`u16`) mismatches both freely interconvert | [§3.2](SPEC.md#32-type-compatibility) |
| [S-3](#s-3-pointer-arithmetic-is-order-sensitive) | S | Executed | `ptr + 5` compiles, `5 + ptr` is a type error (spec now requires both to compile) | [§6.3](SPEC.md#63-binary-operators) |
| [S-4](#s-4-casts-to-non-struct-types-are-unchecked) | S | Source | Casts to any non-struct destination have no relatedness check | [§3.3](SPEC.md#33-casts) |
| [S-5](#s-5-lvalue-checking-is-shallow) | S | Source | Lvalue check is node-kind only, not chain-aware | [§7.3](SPEC.md#73-lvalues) |
| [S-6](#s-6-struct-initializers-neednt-be-complete-or-unique) | S | Source | Omitted/duplicate struct-init fields aren't flagged | [§5.2](SPEC.md#52-struct-initializer-expressions) |
| [S-7](#s-7-decl-cannot-prototype-a-same-file-definition) | S | Source | `decl` + same-file definition collide as `ERR_REDECLARATION` | [§4.6](SPEC.md#46-forward-declarations-decl) |
| [S-8](#s-8-shadowing-check-is-asymmetric) | S | Source | Local `struct` decls aren't shadow-checked like vars/params are | [§7.2](SPEC.md#72-scope-stack--shadowing) |
| [S-9](#s-9-mains-signature-is-unchecked) | S | Source | `main`'s return type/params are entirely unchecked | [§7.4](SPEC.md#74-main) |
| [S-10](#s-10-asm-blocks-are-unvalidated) | S | Source | No analyzer-level check on `asm` mnemonic legality | [§5.7](SPEC.md#57-inline-assembly-asm) |
| [S-11](#s-11-unused-variable-diagnostics-are-unimplemented) | S | Source | `WARN_UNUSED_*` exist in the enum but are never emitted | [§8.2](SPEC.md#82-warnings) |
| [S-12](#s-12-missing-return-detection-is-shallow) | S | Source | Last-statement-kind check only, not control-flow analysis | [§7.5](SPEC.md#75-missing-return-detection) |
| [S-14](#s-14-struct-field-errors-arent-poisoned) | S | Source | A bad struct field can re-trigger diagnostics at every access site | n/a |
| [S-15](#s-15-parameter-poisoning-doesnt-reach-the-function-signature) | S | Source | Call-site argument checks use the original, unpoisoned parameter type | n/a |
| [S-16](#s-16-err_wrong_arg_type-is-dead-code) | S | Source | Argument type mismatches always report as generic `ERR_TYPE_MISMATCH` | [§8.1](SPEC.md#81-errors) |
| [S-17](#s-17-bare-literal-0-bypasses-the-destination-type-check-entirely) | S | Executed | Literal `0` compatible with any destination, not just pointer types (e.g. `Point p = 0;`) | [§3.2](SPEC.md#32-type-compatibility) |
| [S-18](#s-18-void-accepts-pointers-of-any-depth-not-just-depth-1) | S | Source | `void*` accepts a pointer of any depth (`u8**`, etc.), not just matching `ptr_depth 1` | [§3.2](SPEC.md#32-type-compatibility) |
| [S-19](#s-19-pointer-pointee-types-silently-widen-like-scalar-values) | S | Executed | `u16 *q = someU8Ptr;` accepted — pointee mismatch treated as scalar widening | [§3.2](SPEC.md#32-type-compatibility) |
| [S-20](#s-20-logical-operators-widen-their-result-instead-of-always-producing-u8) | S | Source | `&&`/`||` widen their result to the wider operand's type instead of always producing `u8` | [§6.3](SPEC.md#63-binary-operators) |

---

## Lexer & Parser Quirks

### G-1: Unterminated block comment silently swallowed

```c
fn main() -> void { }
/* unterminated
```

**Verified:** `cc02 --syntax-check-only` exits `0` — clean, no diagnostic.
An unterminated `/*` consumes everything to EOF silently, asymmetric with
an unterminated string literal, which *does* error. A stray `/*` near the
end of a file (e.g. from a botched edit) silently discards the rest of the
file rather than failing loudly.

### G-2: `decl fn ... interrupt` drops the qualifier

```c
decl fn irq() interrupt -> void;
fn main() -> void { }
```

**Verified:** parses cleanly (`--syntax-check-only` exit `0`); `--ast-dump`
shows `ForwardDecl fn irq() -> void` with no trace of `interrupt`. The
`interrupt` token is consumed syntactically after the parameter list, but
the forward-declaration AST node (`node_t.fwd_decl`) has no field to store
it — the qualifier is unrepresentable downstream of the parser, not merely
unused.

### G-3: `for`-init cannot reuse an existing variable

```c
fn main() -> void { u8 i = 0; for (i = 0; i < 10; i += 1) { } }
```

**Verified:** `cc02 --syntax-check-only` → `error: unexpected token ... ';'
expected after for loop initialiser` (exit `4`). The init clause's
non-declaration branch parses a bare expression (no `=` support) — only a
fresh declaration (`for (u8 i = 0; ...)`) or an assignment-free expression
is legal in that slot. The incrementer clause, by contrast, does support
`=`/compound-assign.

### G-4: `for`-init and block statements disambiguate differently

The identical token shape `a * b` is resolved by two different mechanisms
depending on position:
- As a block statement, disambiguation is purely shape-based (skip `*`
  tokens, check for a following identifier) — `a * b;` **always** parses as
  declaring `b : a*`, regardless of whether `a` is a real type.
- As a `for`-init clause, disambiguation instead uses the whole-file
  struct-name prescan (same mechanism as cast disambiguation, §6.6/G-5) —
  `for (a * b; ...)` is a **multiplication expression** unless `a` is a
  known struct name.

So `for (Point p = get_point(); ...)` only declares `p` correctly because
`Point` was found by the prescan; the equivalent statement at block scope
would declare correctly regardless.

*(Source: derived from reading `parse_for_initializer_clause` vs.
`parse_stmt`'s identifier-led branch in `parser.c`; not independently
executed.)*

### G-5: Struct-name shadowing misparses a cast

```c
struct Point { u8 x; u8 y; }
fn f() -> void {
  u8 Point = 5;         // local shadows the struct name
  u8 y = (Point) - 1;   // misparses as a CAST to struct Point, not "5 - 1"
}
```

Cast-vs-grouped-expression disambiguation (§6.6) is driven by a one-time,
whole-file, scope-blind prescan of struct names — it has no way to know a
local variable has shadowed a struct name by the time it reaches this
expression. This fails loudly downstream (a struct-cast-by-value or type
error), not silently — accepted upstream as a known, intentional edge case,
not fixed.

### G-6: Binary literals undocumented upstream

`0b`/`0B`-prefixed binary integer literals (`0b1010`) are a real, fully
supported literal form at the lexer level, but the upstream `README.md`'s
prose omits them (only mentions decimal/hex explicitly in one place).
Purely a documentation gap — `SPEC.md` §1.3 now covers it. Not a behavioral
deviation.

---

## Codegen: Verified Silent Miscompiles & Robustness Gaps

Full detail and line references live in the upstream `docs/BUG_REPORT.md`;
this section is a condensed, fuzzer-facing restatement. Regression tests
for all of these exist in upstream `cc02/tests/bug_test.py`.

### P0-1: Nested struct field writes are lost

```c
struct Inner { u8 v; u8 w; }
struct Outer { u8 pad; Inner nest; }
fn main() -> void {
  Outer o;
  o.nest.w = 9;      // stores into a temp copy of o.nest, discarded
  PORTB = o.nest.w;  // reads the real (never-written) o -> garbage
}
```

For a one-level access (`p.x = 5`) the base lowers to a real variable and
the store hits real storage. For a *nested* access, the base is itself a
field access, so it lowers via a load into a temporary, and the store
mutates that temporary — the original object is never written. Also
affects `++`/`--` on a nested field, and any `a.b.c = ...` through a
pointer base. **Verified:** `PORTB` reads `0`, not `9`.

### P0-2: Cast binds to the whole following expression

```c
u16 w = 511;
u8 x = (u8)w / 2;   // computes (u8)(w / 2) = 0xFF, not ((u8)w) / 2 = 0x7F
```

The cast operand is parsed at `logical_or` precedence (the top of the
expression grammar), not `unary` — so a cast binds looser than any C
programmer would expect. The companion trap: `u16 r = (u16)a * b;` casts
the *product* `a * b`, not `a` alone, so the multiply still runs at 8-bit
width and the high byte of the "widened" result is always `0` — since
casting is the only way to request a widened multiply, this silently
defeats that idiom entirely. **Verified:** result is `0xFF`, expected
`0x7F`; `20 * 20` widened-multiply stores `144` with high byte `0` instead
of `400` (`$0190`). Most likely footgun for hand-written or generated test
programs to hit by accident.

### P0-3: Narrow store through wide pointer/register leaves stale high byte

```c
u16 g = 0xFFFF;
fn main() -> void {
  u16 *p = &g;
  *p = 5;    // g becomes 0xFF05, not 0x0005
}
```

`TAC_STORE` writes only the source operand's width; unlike `TAC_COPY`, it
never widens the RHS to the destination's (pointee/register) type. Same
issue for a `u16` hardware register. **Verified:** `g` = `0xFF05`.

### P0-4: Increment through deref or register doesn't store back

```c
u8 g = 10;
fn main() -> void {
  u8 *p = &g;
  ++*p;      // increments a temp copy; g stays 10
}
```

Only the field-access operand shape of `++`/`--` gets a full
load/modify/store; any other operand shape (dereference, hardware
register) increments whatever temporary the load produced, with no store
back. Plain `++local`/`++global` are correct. **Verified:** `g` stays `10`;
`PORTB = 5; ++PORTB;` likewise leaves the register unchanged. The analyzer
accepts both (dereference passes the lvalue check), so this is fully
silent.

### P0-5: Non-literal global initializers are silently dropped

```c
u8  a = 2 + 3;              // constant-fold candidate — dropped
i8  b = -5;                 // negated literal is NODE_UNARY — dropped!
Point p = Point{ .x = 7 };  // struct initializer — dropped
```

Only a bare number or string literal initializer is captured during IR
generation; every other shape falls through to "no initializer" with no
diagnostic — the analyzer type-checks and accepts the expression, then the
IR throws it away. The `-5` case is the sharpest trap: *positive* literals
initialize fine, so adding a minus sign silently un-initializes the
variable. **Verified:** all three leave the global's RAM bytes unwritten.

### P0-6: By-value struct params/returns are truncated

```c
struct Point { u8 x; u8 y; }
fn f(u8 dummy, Point p) -> u8 { return p.y; }
fn main() -> void {
  Point p = Point{ .x = 1, .y = 2 };
  PORTB = f(9, p);   // returns 1 (p.x!), not 2
}
```

The analyzer accepts by-value struct parameters and returns (only checks
the struct exists), but the ABI moves at most 2 bytes per argument and the
function prologue sizes a by-value struct at 1 byte. **Verified:** returns
`p.x` instead of `p.y`; a struct-returning function reads back `0` for a
field that was actually set. Note: a *single*-parameter version of this
test passes by accident (caller/callee happen to reuse the same storage
slot) — that aliasing is why this survived the existing test suite before
`docs/BUG_REPORT.md` was written.

### P0-7: Writes through a pointer to a caller's local are undone on return

```c
fn set(u8 *p) -> void { *p = 5; }
fn main() -> void {
  u8 x = 0;
  set(&x);
  PORTB = x;    // 0, not 5
}
```

A callee's prologue saves the *entire content* of every zero-page slot it
uses, and its epilogue restores those bytes on return. Caller locals live
in the same zero-page region a callee allocates from — so a write through a
pointer into the caller's frame lands on a slot the callee saved on entry,
and gets reverted when the callee returns. **Verified:** `x` stays `0`.
This makes out-parameters — the primary reason `&` exists — silently broken
whenever the target is a local (not a global; `&global` uses a RAM address,
unaffected).

### P0-8: Narrow signed return zero-extends instead of sign-extending

```c
fn f() -> i16 { return -5; }
fn main() -> void {
  i16 r = f();
  if (r < 0) { PORTB = 1; } else { PORTB = 2; }   // takes else — r is +251
}
```

The return path fabricates a `0x00` high byte for byte indices past the
source operand's width, rather than sign-extending — `TAC_COPY` and
`TAC_CAST` both handle this correctly elsewhere; `TAC_RETURN` doesn't.
**Verified:** takes the `else` branch (`r == 251`, not negative).

### P0-9: Interrupt handlers can corrupt mainline computation

```c
fn add(u8 a, u8 b) -> u8 { return a + b; }
fn nmi() interrupt -> void { u8 t = add(40, 40); }
fn main() -> void {
  u8 r = add(2, 3);   // if NMI fires here, between arg-staging and JSR...
  PORTB = r;           // ...r can come out as 80, not 5
}
```

Interrupt entry saves A/X/Y and the handler's own zero-page slots, but not
the shared ABI argument zone, the shared return-value slot, or the shared
arithmetic-helper zones — all of which are also used by whatever mainline
code the interrupt preempts. **Verified deterministically** by firing the
NMI at the exact emulator step where `main` has staged `add`'s arguments
but not yet executed `JSR`: `PORTB` reads `80` (`add(40,40)`, the handler's
own call) instead of `5`. On real hardware this presents as a
timing-dependent, rarely-reproducible wrong value. Handlers that avoid
calling functions and avoid `*`/`/`/`%` are unaffected.

### P1-1: Address-of a struct field segfaults the compiler

```c
struct Point { u8 x; u8 y; }
fn main() -> void {
  Point p;
  u8 *q = &p.x;   // SIGSEGV in cc02 itself
}
```

`&`'s codegen path looks up the operand by name unconditionally, but when
the operand is a field access or dereference, the lowered form is a
temporary, not a named variable — the name lookup dereferences a small
integer reinterpreted as a string pointer. **Verified:** `cc02` crashes
with SIGSEGV (`strcmp` in the backtrace). The analyzer accepts this program
— see [S-5](#s-5-lvalue-checking-is-shallow) for why. Even absent the crash,
the emitted code would be doubly wrong: address of a temp copy, plus
[P0-7](#p0-7-writes-through-a-pointer-to-a-callers-local-are-undone-on-return)
on top.

### P2-1: `reg` addresses above `0xFFFF` are silently truncated

```c
reg u8 X @ 0x10000;
fn main() -> void { X = 0x77; }
```

Register addresses are parsed as a raw literal with no range check anywhere
downstream (ordinary expression literals *do* get range-checked;
register addresses are the one literal that bypasses it). **Verified:**
compiles cleanly and the store lands at `$0000` — overwriting the
zero-page frame pointer.

### P2-2: Struct values accepted as arithmetic/condition operands

```c
struct Point { u8 x; u8 y; }
fn main() -> void {
  Point a; Point b;
  Point c = a + b;        // "type-checks" — accepted
}
// separately:
fn main() -> void {
  Point a;
  if (a) { }               // also accepted — no scalar/condition check
}
```

Same-named struct types are "compatible" by the struct-name-match rule, and
binary/condition operands are never required to be scalar. Codegen then
sizes the struct operand at 1 byte and only touches the first field's
worth of bytes, leaving the rest of the result garbage. **Verified:** both
forms compile (`rc=0`).

### P2-3: Interrupt handlers can be called directly

```c
reg u8 PORTB @ 0x6000;
fn irq() interrupt -> void { PORTB = 1; }
fn main() -> void { irq(); }
```

The analyzer knows a callee is an interrupt handler (it validated the
qualifier in pass 1) but doesn't reject a direct call to one. The handler
was emitted with a save/`RTI` epilogue; a plain `JSR` call pops bytes that
were never pushed and `RTI` misinterprets the return address as status +
PC. **Verified:** compiles (`rc=0`); runtime stack corruption follows.

### P2-4: Separators are optional, not required

```c
f(1 2);                          // parses as a call with args, comma missing
fn g(u8 a u8 b) -> u8 { ... }    // params, comma missing
Point{ .x = 1 .y = 2 };          // struct-init fields, comma missing
```

The list-parsing loops for call arguments, parameters, and struct-init
fields consume a comma *if present* but don't require one — each just
relies on the closing delimiter. **Verified:** all three parse (`rc=0`).
Harmless today, but makes a dropped-comma typo legal, and will read
differently once/if expressions can start with unary `-` in these
positions (`f(1 -2)` is one argument today; with a required comma, `f(1,
-2)` unambiguously reads as two).

### P2-5: Deep loop nesting overflows a fixed stack

80 levels of nested `while`/`for` write past a fixed 64-entry
loop-tracking array with no bounds check. **Verified as a real
out-of-bounds write** (confirmed via source, not black-box output) — but on
the *current* struct layout the corruption happens to be self-consistent
(the overwritten neighbor field mirrors the intended stack at a displaced
index), so `break`/`continue` targets still come out right and nothing
externally visible fails. This is the one `bug_test.py` entry that's
currently green, and the test itself documents that it cannot go red from
the outside on this particular build — it exists to catch the crash on a
future layout where the corruption lands somewhere live.

### P2-6: Global allocation never checks the RAM ceiling

Enough `u16` globals (~8000, i.e. ~16 KB) exceed the ~15.8 KB of available
RAM (`$0200`–`$3FFF`) with no diagnostic — allocation just keeps advancing
past the ceiling (and, in principle, could wrap the 16-bit address space
entirely). **Verified:** compiles cleanly (`rc=0`) where it should be a
codegen-stage error, the same way ROM overflow already is.

### P2-7: Pointer arithmetic is unscaled

```c
u16 *p = ...;
p + 1;   // advances by 1 byte, not by 2 (sizeof(u16))
```

`ptr + int` always advances by exactly one byte regardless of the pointee's
type/size — fine for the idiomatic `u8*` walk pattern (see `SPEC.md`
Appendix A), but silently diverges from C expectations on `u16*` or
struct-pointer arithmetic, with no diagnostic at any stage.

---

## Analyzer: Type-System & Semantic Laxity

### S-1: `void`/null-literal conflation

```c
fn main() -> void { void *vp; u8 x = vp; }
```

**Verified:** `cc02 --syntax-check-only` exits `0` — no diagnostic. `SPEC.md`
§3.2 requires rule 1's unconditional compatibility to apply only to the bare
literal `0`; a genuine `void*` value is supposed to be checked normally, as
an ordinary pointer type. `cc02` doesn't draw that distinction: the literal
`0` and every `void*`-typed *value* (not just the literal) share one
internal type representation (`void`, pointer depth 1), and the
type-compatibility check's very first rule fires on that shared
representation unconditionally, without ever inspecting the destination
type. So a genuine `void*` variable, exactly like the bare literal itself
(see [S-17](#s-17-bare-literal-0-bypasses-the-destination-type-check-entirely)),
is accepted as compatible with *any* destination, including non-pointer
scalars and by-value structs. This is the single most consequential
type-system gap in the language: it defeats static checking anywhere a
`void*`-shaped value flows into a differently-shaped destination.

### S-2: No signedness checking

```c
i8 x = 200;        // out of i8's range, fits u8 — accepted, wraps negative
u8 y = someI8Var;  // accepted, no diagnostic
u16 z = someI8Var; // cross-width AND cross-signedness — also accepted
```

Type compatibility never compares signedness at all, only *width*
(`type_width`) — this holds both at equal width (`i8`↔`u8`, `i16`↔`u16`
freely interconvert) and across widths (a signed value is accepted anywhere
its width fits, regardless of whether the destination is signed or
unsigned, e.g. `i8`→`u16`). `SPEC.md` §3.2 rule 8 requires `expected` and
`actual` to agree on signedness *in addition to* the width check;
`cc02`'s `is_types_compatible` only implements the width half.

**Verified:** confirmed directly against the analyzer, not merely inferred
from reading it — both the same-width and cross-width cases pass with no
diagnostic.

### S-3: Pointer arithmetic is order-sensitive

```c
fn g(u8 *p) -> void { u8 *b = p + 5; }   // compiles (rc 0)
fn g(u8 *p) -> void { u8 *b = 5 + p; }   // type error (rc 5):
                                          // "expected u8, found u8*"
```

**Verified both directions.** `SPEC.md` §6.3 requires `int + ptr` to compile
(addition is commutative), same as `ptr + int`. The pointer-arithmetic
special case in binary operator resolution only checks `left.is_ptr` — a
pointer on the *right* falls through to the generic compatibility check,
which rejects mixing pointer and non-pointer operands, so the commutative
form is unimplemented. `ptr - ptr` (both sides pointers) doesn't hit this
special case either — it doesn't need to: with both operands the identical
pointer type, the generic struct-name/kind/depth compatibility check already
accepts it and produces the pointer-typed address-difference result the
spec calls for (§6.3), so that case isn't a deviation.

*(Source: `analyzer.c` `NODE_BINOP` resolution; not independently
executed.)*

### S-4: Casts to non-struct types are unchecked

`(type)expr` for any non-struct destination type is accepted regardless of
the source expression's type — casting a struct to `u8`, or between
unrelated pointer types, produces no diagnostic. The source operand's type
is resolved only for its own side-effect diagnostics and is otherwise
discarded. (Struct destinations *are* checked — see `SPEC.md` §3.3 for the
`ERR_UNKNOWN_STRUCT`/`ERR_STRUCT_CAST_BY_VALUE` rules that do exist.)

*(Source: `NODE_CAST` handling in `analyzer.c`; not independently
executed.)*

### S-5: Lvalue-checking is shallow

```c
someFunctionCall().field = 5;   // accepted as an assignment target
```

The lvalue check inspects only the *top-level AST node kind*
(`NODE_IDENTIFIER` / `NODE_FIELD_ACCESS` / `NODE_DEREF`) — it never
recurses into whether the *base* of a field-access or deref chain is
addressable storage at all. A field access whose base is a call result (an
rvalue with no storage) is accepted purely because the outer node "looks
like" one of the three lvalue shapes. This is the root enabler of
[P1-1](#p1-1-address-of-a-struct-field-segfaults-the-compiler).

*(Source: `is_lvalue` in `analyzer.c`; not independently executed, but the
downstream P1-1 crash is empirically confirmed and consistent with this
explanation.)*

### S-6: Struct initializers needn't be complete or unique

```c
Point{ .x = 1 }              // .y silently left unset, no diagnostic
Point{ .x = 1, .x = 2 }      // duplicate field, no diagnostic
```

Fields omitted from a struct initializer produce no "missing field"
diagnostic, and the same field named twice in one initializer list is never
flagged as a duplicate — each `.field = expr` entry is checked
independently against the struct's field list.

*(Source: `NODE_STRUCT_INIT` handling in `analyzer.c`; not independently
executed.)*

### S-7: `decl` cannot prototype a same-file definition

```c
decl fn foo(u8 x) -> void;
fn foo(u8 x) -> void { }   // ERR_REDECLARATION
```

Pass 1 performs no merging between a `decl` forward declaration and a later
same-name definition in the same file — both are inserted into the
identical global-scope symbol table with no special-casing, so they
collide as an ordinary redeclaration. `decl` is intended for
cross-translation-unit references (multi-file linking); the familiar C
"prototype, then define later in the same file" idiom is not supported.

*(Source: `pass1_register_globals` in `analyzer.c`; not independently
executed.)*

### S-8: Shadowing check is asymmetric

A local variable or parameter that shadows a name visible in *any*
enclosing scope (including global) is rejected
(`ERR_SHADOWED_DECLARATION` — see `SPEC.md` §7.2). A **struct declared
inside a function body**, however, goes through a different registration
path that only checks same-scope redeclaration — it is never checked
against outer-scope shadowing. A local struct can legally share a name with
an outer-scope variable, function, global, or another struct, even though
"shadowing is disallowed" is otherwise a blanket rule in this language.

*(Source: `declare_local_variable` vs. the local-`NODE_STRUCT_DECL` path in
`analyzer.c`; not independently executed.)*

### S-9: `main`'s signature is unchecked

`SPEC.md` §7.4 requires exactly `fn main() -> void { ... }` — zero
parameters, `void` return, and an actual definition (not a `decl`). `cc02`
only verifies that "a symbol named `main` exists and is a function" — any
return type and any parameter list/count are accepted with no diagnostic,
and a `main` introduced solely via `decl` (with no matching same-file
definition) satisfies the same check a real definition would.

*(Source: the end-of-`analyze()` check in `analyzer.c`; not independently
executed.)*

### S-10: `asm` blocks are unvalidated

The analyzer performs zero semantic checking on `asm { ... }` blocks —
mnemonic legality is deferred entirely to codegen (explicitly noted in-code
as out of scope for this pass). An invalid mnemonic produces no
analysis-stage diagnostic.

*(Source: `analyze_stmt`'s `NODE_ASM_BLOCK` case in `analyzer.c`; not
independently executed.)*

### S-11: Unused-variable diagnostics are unimplemented

`WARN_UNUSED_VARIABLE`, `WARN_UNUSED_FUNCTION`, `WARN_UNUSED_STRUCT`, and
`WARN_UNUSED_FIELD` are all defined in the warning enum and have
print-dispatch code in `errors.c`, but no code path in `analyzer.c` ever
constructs one — each `case` in the warning-print dispatcher immediately
returns with a `// unimplemented` comment. Do not treat "no unused-variable
warning" as evidence a variable is actually used; the check simply doesn't
exist yet.

*(Source: `print_warning` in `errors.c` cross-referenced against
`analyzer.c`; not independently executed.)*

### S-12: Missing-return detection is shallow

```c
fn f(u8 x) -> u8 {
  if (x) { return 1; }     // one-armed if, no else — NOT flagged, despite
}                            // falling through with no return when x == 0

fn g() -> u8 {
  while (cond) { ... }      // NOT flagged, whether or not this loop can
}                            // ever exit or ever returns
```

The check looks only at the function's *last top-level statement*'s node
kind — `NODE_RETURN`, `NODE_IF`, `NODE_WHILE`, `NODE_FOR`, `NODE_BLOCK` all
pass with no diagnostic, regardless of whether that statement actually
guarantees a return on every path. There is no control-flow/path-coverage
analysis. Conversely, dead code placed *after* an early, unconditional
`return` (making it the new last statement) **is** flagged, since it isn't
one of the five "may-return" kinds — a technically-correct but
easily-misread positive.

*(Source: `stmt_may_return` in `analyzer.c`; not independently executed.)*

### S-14: Struct field errors aren't poisoned

Top-level declarations (vars/globals/registers/params) that fail type
validation are poisoned to an error type in their symbol-table entry, so
every later use resolves silently to "already reported" with no repeated
diagnostic. **Struct fields don't get this treatment** — a bad field
(unknown type, invalid self-reference, etc.) is reported once but never
written back into the struct's stored field list, so every subsequent
access to that specific field independently re-resolves from the still-bad
declaration and can trigger further downstream diagnostics.

*(Source: `validate_toplevel_types`'s per-field loop in `analyzer.c`; not
independently executed.)*

### S-15: Parameter poisoning doesn't reach the function signature

When a parameter's declared type fails validation, the poisoned type is
written into a fresh local-scope symbol used for checking *uses of that
parameter inside the function body* — but the function's own
`SYMBOL_FUNCTION` entry (used to type-check arguments at every *call site*)
is never touched. In principle, a function with one invalid parameter type
could drive a bad type comparison at every call site, rather than being
poisoned once, unlike the treatment globals/registers get.

*(Source: `pass2_entry` parameter handling vs. `validate_toplevel_types` in
`analyzer.c`; not independently executed — this asymmetry is rare to
trigger in practice since it requires an invalid struct name in a
parameter's type.)*

### S-16: `ERR_WRONG_ARG_TYPE` is dead code

`ERR_WRONG_ARG_TYPE` is defined in the error enum and has rendering support
in `errors.c`, but is never actually constructed anywhere in `analyzer.c`
— function-call argument type mismatches always report through the generic
`ERR_TYPE_MISMATCH` (context `"function call"`) instead. Do not expect to
see `ERR_WRONG_ARG_TYPE` in compiler output; treat it as reserved/unused.

*(Source: enum/renderer cross-reference against emission sites in
`analyzer.c`; not independently executed.)*

### S-17: Bare literal `0` bypasses the destination-type check entirely

```c
struct Point { u8 x; u8 y; }

fn main() -> void {
  Point p = 0;
}
```

**Verified:** compiles with no diagnostic. `SPEC.md` §3.2 rule 1 exempts the
bare literal `0` from ordinary type-checking only when `expected` is a
pointer type — the null-pointer-constant idiom. A by-value struct (or any
other non-pointer destination) is supposed to fall through to rule 3
(`is_ptr` mismatch, since the literal's own inferred type is `void` at
`ptr_depth 1`) and be rejected. `cc02`'s `is_types_compatible` never checks
`expected`'s pointer-ness before granting the literal's exemption — it fires
purely on `actual` being the literal `0`, regardless of what `expected` is.
So `0` is accepted as an initializer/assignment/call-argument value for
*any* destination type, including by-value structs, not just pointers. This
is not intended design — confirmed directly against the parser/analyzer,
not merely inferred from spec wording. Related to
[S-1](#s-1-voidnull-literal-conflation), which documents the same
missing-destination-check for non-literal `void*` *values*; this entry
covers the base case of the literal itself.

### S-18: `void*` accepts pointers of any depth, not just depth 1

```c
fn main() -> void {
  u8 **pp;
  void *vp = pp;  // ptr_depth mismatch (2 vs 1) — accepted with no diagnostic
}
```

`SPEC.md` §3.2 rule 2 requires `actual`'s pointer depth to match
`expected`'s (`ptr_depth 1`) before granting the `void*`-accepts-any-pointee
exemption — a `u8**` is supposed to fall through to the ordinary
depth-mismatch rejection (rules 3–4) and require an explicit cast, same as
any other pointer-depth mismatch. `cc02`'s `is_types_compatible` doesn't
compare depth in this branch at all — it treats *any* pointer, regardless
of depth, as compatible with a `void*` destination, collapsing the depth
information entirely.

*(Source: `is_types_compatible` in `analyzer.c`; not independently executed
for this document.)*

### S-19: Pointer pointee types silently widen like scalar values

```c
fn main() -> void {
  u8 x;
  u8 *p = &x;
  u16 *q = p;   // pointee mismatch (u8* -> u16*) — accepted with no diagnostic
}
```

`SPEC.md` §3.2 rule 8 is only supposed to apply when both `expected` and
`actual` are non-pointer values (`ptr_depth === 0`) — two pointers at the
same depth with mismatched, non-void/non-struct pointee kinds are supposed
to require an exact pointee match or an explicit cast, the same as a
struct-name mismatch would. `cc02`'s `is_types_compatible` doesn't scope its
width/signedness fallback to non-pointers: it applies the same
`width(actual) <= width(expected)` logic (and, per
[S-2](#s-2-no-signedness-checking), no signedness check either) to pointer
pointee kinds exactly as it does to plain scalar values. So a `u8*` is
silently accepted anywhere a `u16*` is expected, entirely bypassing pointer
type-safety.

**Verified:** confirmed directly against the compiler — `u16 *q = p;` (with
`p` declared `u8*`) compiles with no diagnostic.

### S-20: Logical operators widen their result instead of always producing u8

```c
fn main() -> void {
  u8 a;
  u16 b;
  u8 flag = a && b;  // intended: always u8 - actually rejected/widened, needs a cast
}
```

`SPEC.md` §6.3 states `&&`/`||` are the one exception to the general
result-type widening rule — they're truthiness tests, so they're meant to
always produce a `u8` result regardless of operand width. Per the IR
generation code, `cc02` doesn't special-case them at all: they go through
the same generic "result is the wider of the two operands" path as every
other binary operator, so `a && b` above actually has type `u16` (matching
`b`), not `u8` — meaning assigning it to a `u8`-declared destination fails
the ordinary width check (rule 8) and needs an explicit cast, when the
intended design says it never should.

*(Source: IR generation code for `&&`/`||`; not independently executed for
this document.)*
