# Known Deviations — this repo's toolchain vs. `SPEC.md`

**Status:** pinned to branch `rewrite/c02-haskell-frontend` @ `f3fad46`
(2026-07-14), against `SPEC.md` at the same checkout.

Companion to [`DEVIATIONS_c_impl.md`](DEVIATIONS_c_impl.md), but for the
**other** implementation. That file records where the upstream C compiler
(`cc02`) diverges from `SPEC.md`; this one records where **this repo's**
toolchain — the Haskell frontend (`c02-frontend`) plus the C code generator
(`c02-as`) — diverges from it. `SPEC.md` stays the normative contract; both
deviation docs are the living known-issues oracles.

The two docs tend to lean opposite ways. `cc02`'s deviations are mostly
*laxity* — it accepts or miscompiles things the spec forbids. This
implementation's deviations so far are the reverse: it is **stricter** than
`SPEC.md`, rejecting programs the spec requires it to accept. That is often a
deliberate design stance, but a stricter-than-spec rule is still a deviation
until either the frontend relaxes or `SPEC.md` is amended to match — this doc
is where each such gap is pinned in the meantime.

**Verification level** is marked per entry:
- **Executed** — reproduced by actually compiling the exact snippet shown
  (and, for any codegen-level entry, running it in py65) while authoring this
  document.
- **Source** — derived from reading the frontend/codegen sources, not
  independently executed. Re-verify before relying on one as an oracle.

Severity legend:
- **T** — type/semantic over-strictness: the analyzer rejects a program
  `SPEC.md` requires it to accept.
- **P0** — silent miscompile: analysis accepts the program, `c02-as` emits
  wrong machine code, no diagnostic.
- **P1** — loud failure: a stage crashes or fails unsafely on a reasonable
  program.
- **P2** — robustness: a real gap, but latent or requiring an unusual program
  to trigger.
- **G** — grammar/lexer quirk.

## Quick Reference

| ID | Sev | Verified | Summary | Spec section |
|---|---|---|---|---|
| [HS-1](#hs-1-untyped-literals-adopt-a-targets-width-but-not-its-signedness) | T | Executed | Untyped integer literals adopt a target's width but **not** its signedness, so `i8 b = 47` is rejected | [§3.4](SPEC.md#34-integer-literal-typing) |

## Entries

### HS-1: Untyped literals adopt a target's width but not its signedness

```c
i8  b = 47;          // REJECT: "type mismatch in b: expected i8, found u8"
i16 w = 300;         // REJECT: "type mismatch in w: expected i16, found u16"
u8  n = -1;          // REJECT: "type mismatch in n: expected u8, found i8"

i8  b = -5;          // ACCEPT — negative literal is already i8 (§3.4 table)
i8  b = (i8)47;      // ACCEPT — explicit cast
u16 w = 5;           // ACCEPT — width IS adopted (u8-range lit -> u16)
i16 w = -5;          // ACCEPT — width adopted, signedness already matches
```

`SPEC.md` §3.4 says an **untyped literal** "adopts the signedness/width of its
context (assignment target, binary operand, call argument)." This
implementation adopts the **width** half but not the **signedness** half: a
literal is fixed at the signedness implied by its value in the §3.4 table
(non-negative → unsigned, negative → signed via `NODE_UNARY(-)`), and is
then run through the ordinary signedness-compatibility check (§3.2 rule 8 /
§6.3) against its context. A non-negative literal therefore never becomes a
signed target's type, and a negative literal never becomes an unsigned one —
regardless of whether the value is in range for the target.

The behavior is identical and consistent across all three contexts §3.4
names:

```c
i8 a = -5; i8 c = a + 47;   // binary operand: REJECT ("...in binary operation...")
fn f(i8 x) -> void { }
f(47);                       // call argument:  REJECT ("...in function call...")
```

**Reachable workarounds** (used throughout `test/emu/*.c02`): write the
signed value as a negative literal where possible (`i8 a = -5;`), or attach an
explicit sign-reinterpreting cast (`(i8)47`, `(i16)300`). The cast is exact —
§3.4 defines signed↔unsigned casts as bit-pattern-preserving.

**Relationship to `cc02`:** the upstream compiler accepts `i8 b = 47`
outright — its `is_types_compatible` checks width only and never compares
signedness at all (see [`DEVIATIONS_c_impl.md` §S-2](DEVIATIONS_c_impl.md#s-2-no-signedness-checking)).
This implementation correctly *added* the signedness check that `SPEC.md` §3.2
requires and `cc02` omits; HS-1 is that the check is also applied to untyped
literals, without first performing the §3.4 context-adoption that would let a
literal legitimately take the target's signedness. It is an over-application
of an otherwise-correct fix, not a reintroduction of `cc02`'s laxity.

**Resolution is open:** either (a) `SPEC.md` §3.4 is amended so untyped
literals adopt only width and require an explicit cast to change signedness —
making this implementation conformant and matching its intentional strictness
— or (b) the analyzer performs the §3.4 signedness adoption for the bare-literal
case before the compatibility check. Until one lands, this is a deviation.

**Verified:** Executed — every snippet above was compiled with
`bin/c02-frontend` at the pinned commit; accept/reject outcomes and the quoted
diagnostics are its actual output.
