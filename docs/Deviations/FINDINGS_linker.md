# Audit Findings — `c02-ld` (OCaml)

**Status:** pinned to `main` @ `6d619f6` (2026-07-17). Companion to
[`FINDINGS_frontend.md`](FINDINGS_frontend.md) /
[`FINDINGS_codegen.md`](FINDINGS_codegen.md) /
[`FINDINGS_ir_format.md`](FINDINGS_ir_format.md); format follows
[`DEVIATIONS_hs_impl.md`](../DEVIATIONS_hs_impl.md).

`SPEC.md` defines language semantics only — it never mentions a linker beyond
one sentence in §7.4 (entry-point existence is "enforced by the linker/driver").
Nearly every entry here is therefore **none — undocumented architecture**;
where a finding touches §4.6 (`decl`) or §7.4 it is cited.

**Verification level**: **Executed** = reproduced with `bin/c02-frontend` +
`bin/c02-ld` at the pinned commit; **Source** = read from
`c02-ld/bin/*.ml`, not executed.

Severity: **T** over-strict rejection · **P0** silent wrong output ·
**P1** loud failure on a reasonable program · **P2** latent gap · **G** quirk.

## Quick Reference

| ID | Sev | Verified | Summary | Spec section |
|---|---|---|---|---|
| [LD-1](#ld-1) | T | Executed | Extern dedup compares parameter *names*: identical `decl`s with different param names conflict | [§4.6](../SPEC.md#46-forward-declarations-decl) |
| [LD-2](#ld-2) | T | Executed | Function-local structs leak into the module struct table and collide cross-module | none — undocumented architecture |
| [LD-3](#ld-3) | G | Source | Cross-kind conflict diagnostics name the wrong kind | none — undocumented architecture |
| [LD-4](#ld-4) | P2 | Source | Registers cannot be forward-declared across modules | [§4.6](../SPEC.md#46-forward-declarations-decl) / [§4.3](../SPEC.md#43-registers-reg) |
| [LD-5](#ld-5) | P2 | Source | `List.init` over a mutating cursor depends on left-to-right application order | none — undocumented architecture |
| [LD-6](#ld-6) | P2 | Source | The linker validates only the symbol *table*, never instruction operands | none — undocumented architecture |
| [LD-7](#ld-7) | — | Source | "Full link" is not a linker concept: finality lives in `c02-as`, `-c` only in the driver | [§7.4](../SPEC.md#74-main) |
| [LD-8](#ld-8) | — | Source | Test-suite coverage gaps: the 19 cases vs. what the seams actually need | none — undocumented architecture |

## Entries

### LD-1: Extern-vs-extern dedup compares parameter names

```c
// m1.c02                          // m2.c02
decl fn send(u8 a) -> void;        decl fn send(u8 b) -> void;
fn use1() -> void { send(1); }     fn use2() -> void { send(2); }
```
```
$ c02-ld m1.o m2.o -o out.o
Fatal error: exception Failure("Conflicting externs")     # rc=2
```

`combine` resolves two externs with `if e1 = e2 then a else failwith …` —
OCaml structural equality over the whole record, whose `extern_params` are
`named_type` triples **including the parameter name**. Two `decl`s of the same
function that differ only in a param name are spuriously rejected whenever the
definition isn't in the same link (the moment a definition is present, the
extern-vs-def path correctly strips names via `param_types`). Param names in a
`decl` are documentation; §4.6's cross-TU intent clearly wants signature
equality. Renaming a parameter in a header that not all TUs re-pull, or two
authors writing the same prototype independently, breaks the link. With
identical spellings the same link succeeds (Executed control).

**Fix:** compare `(extern_is_func, extern_name, extern_type,
param_types extern_params)` — the same projection the def path already uses.

**Spec:** §4.6.

**Verified:** Executed — both the reject and the same-name control.

### LD-2: Function-local structs leak into the module struct table and collide cross-module

```c
// s1.c02                         // s2.c02
fn f() -> void {                  fn g() -> void {
  struct Local { u8 a; }            struct Local { u16 b; }
}                                 }
```
```
$ c02-ld s1.o s2.o -o out.o
Fatal error: exception Failure("Struct: Local redeclaration")   # rc=2
```

The frontend's module struct table contains every struct laid out anywhere in
the TU, including statement-position structs (SPEC §4.4 explicitly allows
those; see [FE-17](FINDINGS_frontend.md#fe-17)). `dedup_structs` treats the
table as a flat global namespace: same name + different shape = hard conflict.
Two modules whose *private, function-scoped* structs happen to share a name
therefore cannot be linked — a scoping violation the language definition never
implies. Same-name-same-shape locals silently dedup to one entry, which is
also wrong in principle (they're distinct types) but benign given layout
equality.

The right fix is upstream (don't serialize function-local layouts under bare
names — mangle or scope them); the linker seam is where it detonates.

**Spec:** none — undocumented architecture (struct table semantics are
specified nowhere).

**Verified:** Executed.

### LD-3: Cross-kind conflict diagnostics name the wrong kind

`combine`'s extern-vs-def arm ends in catch-alls:
a **function** extern resolved against a `Reg` or `Global` reports
"declared as function, defined as variable" (even when the def is a register);
a **variable** extern resolved against a `Func` or `Reg` reports
"declared as variable but defined as a register" (even when the def is a
function). The rejects themselves are correct — only the diagnostic text lies,
and none of these messages name the symbol or the offending files. Worth
fixing alongside LD-1 since it's the same match.

**Spec:** none — undocumented architecture.

**Verified:** Source (`link.ml:39-52`); the func-vs-var and var-vs-reg test
cases pass because only the exit code is asserted.

### LD-4: Registers cannot be forward-declared across modules

There is no `decl reg` form (§4.6 offers only `decl fn` and `decl type name`),
and a plain `decl u8 PORTB;` against another module's
`reg u8 PORTB @ 0x6000;` hits `combine`'s variable-extern arm, which accepts
only `Global` — a `Reg` def is a hard failure. Consequence: every TU that
touches a hardware register must repeat the full `reg … @ addr;` line
(tolerable — identical regs dedup), but the address is then duplicated source
truth in N files, and one edited copy becomes an intentional-looking
"Conflicting registers" error. Undocumented as a design decision; §4.6's
"intended for cross-translation-unit references" reads as if `decl` covered
all shared names.

**Spec:** §4.6 / §4.3 (silent on the interaction).

**Verified:** Source.

### LD-5: `List.init` over a mutating cursor depends on application order

`serializer.ml` reads every sequence as
`List.init count (fun _ -> read_x cursor)` — the element reader *mutates the
cursor*, so decoding is only correct if `List.init` applies `f 0, f 1, …` in
ascending order. The file's own header RULE ("never put two cursor reads in
the same tuple — OCaml leaves evaluation order unspecified") names exactly
this hazard and then relies on `List.init`'s ordering, which the stdlib
documentation only pinned down in recent releases (current 5.x applies in
order; older manuals left it unspecified, and the historic implementation
switched strategies at a length threshold). It works on the toolchain's OCaml
(5.5), and probably everywhere — but it's the one place the wire decode
depends on library behavior rather than written sequencing. Cheap to make
explicit (a `let rec` loop or `List.init` → `Array.init`+`to_list` with a
comment).

**Spec:** none — undocumented architecture.

**Verified:** Source.

### LD-6: The linker validates the symbol table, never the instructions

`link` reconciles the *declared* namespace (externs/globals/regs/cfgs) and
dedups structs — it never walks a single instruction. Consequences it
therefore cannot catch, all deferred (at best) to `c02-as`:

- an `OPERAND_VAR` naming a symbol that exists in no module (can't come from
  this frontend, but the linker is the merge point where a hand-built or stale
  `.o` would be caught — it isn't);
- a `TAC_CALL` to a name that is neither a CFG nor an extern (caught later,
  loudly, by `resolve_func_fixups` — but only if codegen runs);
- struct-typed operands whose struct name is missing from the merged table
  (codegen's `full_type_size` then silently sizes the type as **1 byte** —
  see [IR-6](FINDINGS_ir_format.md#ir-6));
- label ids ≥ the CFG's `next_label` (an out-of-bounds write inside `c02-as`,
  [FE-21](FINDINGS_frontend.md#fe-21)).

A linker is the natural place for these whole-program checks; today it is
purely a namespace merger.

**Spec:** none — undocumented architecture.

**Verified:** Source.

### LD-7: "Full link" is not a linker concept

`c02_ld.ml` always emits a relocatable object and never requires `main` —
deliberately (the comment cites §7.4; entry-point existence is enforced by
`c02-as`'s `emit_call_main`, the only stage that produces an executable).
`-c` exists only in the **driver** (`c02c`), where it merely stops the
pipeline after `c02-ld`. Two undocumented consequences:

1. §7.4's sentence "existence … is a whole-program property, enforced by the
   linker/driver" is now *false for the linker half* — the enforcement point
   is the generator. (The per-memory note: analyzer checks signature only,
   generator checks existence, linker checks neither.)
2. Unresolved externs pass a *final* link exactly as they pass a partial one.
   The safety net is `c02-as`: unresolved extern **functions** fail loudly
   only if actually called (`resolve_func_fixups`), and unresolved extern
   **variables** fail loudly because `emit_compiler_extern_inits` whitelists
   only `__heap_start`/`__memory_top` and rejects everything else. That
   whitelist is the entire cross-stage contract for extern variables, and it
   lives in the codegen stage, not the link stage.

Not a bug today — but the division of responsibility exists only in code
comments and this document, and `SPEC.md` §7.4 should be amended to match.

**Spec:** §7.4 (stale wording).

**Verified:** Source (behavior consistent with the `library_no_main` /
`unresolved_extern_partial` test cases, which exercise the partial path only).

### LD-8: Test-suite coverage vs. the actual seams

The 19 cases in `test/linker/tests/` pin one instance of each *kind* of
merge/reject; the seams found above sit precisely in the combinations they
skip. Gaps worth closing, in rough value order:

1. **Diamond include** — the single most common real topology: the same
   `decl` (via a shared header) in *two* modules plus the definition in a
   third. Exercises the extern≡extern fold *and* extern-vs-def in one link,
   and would have caught LD-1 the day a header's param name drifted. No case
   has more than two modules.
2. **Extern dedup with differing param names** (LD-1) — the reject that
   shouldn't be.
3. **Local structs across modules** (LD-2) — both the same-shape (should
   link; arguably shouldn't share an entry) and different-shape (currently a
   reject of a valid program) variants.
4. **Both-tentative merge**: `u8 g;` in two modules with *no* initializer
   anywhere (`IRInitNone`+`IRInitNone`) — the one `combine` global arm no
   case reaches (`global_tentative_merge` covers None+Init only).
5. **Full (non-`-c`) link with an unresolved extern** — asserting the
   *generator's* loud failure, since that's where LD-7 says the net is; and
   the extern-function-declared-but-never-called variant, which links and
   generates silently by design (worth pinning as intended).
6. **Struct/symbol namespace independence**: a struct named `foo` in one
   module, a function `foo` in another — legal today (separate namespaces in
   the linker, though *not* per-TU in the frontend, §7.2); untested either
   way, and a behavior change here would be silent.
7. **`decl` variable vs. `reg` definition** (LD-4) — pin the current reject
   as intended or fix it; today it's neither tested nor documented.
8. **Same `.o` linked twice** — currently a loud `Redefined function`, which
   is fine, but untested; and **registers at the same address under different
   names** (aliasing, currently allowed silently — probably fine for
   hardware, worth pinning).
9. **`is_interrupt` across the linker** — no case links an interrupt handler
   from a secondary module and runs it; the flag round-trips (Source) but
   nothing pins it.

None of these require new machinery — every one fits the existing
`spec`-directive format.

**Spec:** none — undocumented architecture.

**Verified:** Source (suite read; gaps 1–4 cross-checked against the
`combine`/`dedup_structs` code paths they would exercise; LD-1/LD-2 gap
instances Executed above).
