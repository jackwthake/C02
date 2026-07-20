# Audit Findings — the IR/TAC object format as a cross-stage contract

**Status:** pinned to `main` @ `6d619f6` (2026-07-17). Cross-cutting companion
to the per-stage docs ([frontend](FINDINGS_frontend.md),
[linker](FINDINGS_linker.md), [codegen](FINDINGS_codegen.md)); format follows
[`DEVIATIONS_hs_impl.md`](../DEVIATIONS_hs_impl.md).

The subject here is the `.o` wire format and its three hand-synchronized
implementations — writer `c02-frontend/src/C02/Lowering/Serialize.hs`,
reader+writer `c02-ld/bin/serializer.ml` + `tac.ml`, reader
`c02-as/src/ir-serial.c` + `ir.h` — asking one question per entry: **what is
actually guaranteed between stages, what is merely assumed, and where does the
contract break silently if one side changes?**

Every entry's spec reference is **none — undocumented architecture**:
`SPEC.md` §Scope restricts itself to language surface and static semantics;
the object format, its numbering, and its invariants are defined nowhere
except the three code bases themselves (each carrying "must match X
byte-for-byte" comments). That absence is itself the meta-finding.

**Verification level**: **Executed** = demonstrated end-to-end at the pinned
commit; **Source** = traced through two or three of the implementations.

Severity: **P0** silent wrong output · **P1** loud failure ·
**P2** latent/robustness · **G** quirk.

## Quick Reference

| ID | Sev | Verified | Summary |
|---|---|---|---|
| [IR-1](#ir-1) | P2 | Source | Positional enum numbering ×3 implementations; one whole-file version number is the only guard |
| [IR-2](#ir-2) | P0 | Executed | Length-prefixed strings decay to C strings: embedded NUL, `""`≡absent, non-ASCII truncation |
| [IR-3](#ir-3) | P2 | Source | "Native endian, native width" is really "x86-64 LP64" — unchecked by any header field |
| [IR-4](#ir-4) | P2 | Source | Counts and ids are trusted, not validated: `next_label` is an OOB write waiting in `c02-as` |
| [IR-5](#ir-5) | P2 | Source | Semantic invariants riding on top of the syntax: never-emitted ops, `is_ptr` redundancy, single-block CFGs |
| [IR-6](#ir-6) | P2 | Source | Struct identity is a bare name with pre-baked layout; a missing table entry silently sizes as 1 byte |
| [IR-7](#ir-7) | P2 | Source | Extern resolution is a codegen-stage whitelist, not a format or linker concept |

## Entries

### IR-1: Three hand-synchronized copies of positional numbering, one version number

The tags for `tac_op` (37 values), type kind (6), operand kind (5), and init
kind (3) exist as: a C `enum` (`ir.h`), a Haskell pattern-match table
(`TAC.hs` — its comment: "This MUST match the order in c02-as/src/ir.h"), and
an OCaml array (`tac.ml`). All three are positional. The **only** wire-level
guard is the whole-file pair `magic + ir_version` (currently 3, hardcoded in
all three places).

Silent-break scenario: an op inserted mid-enum on one side (say a future
`TAC_ASM` slotted where cc02 had it) renumbers everything after it; every
`.o` still passes magic/version, and each instruction decodes as a *different
valid op* — the OCaml side fails only if a tag lands out of range, the C side
only when the generator's `default:` happens to be hit. Nothing detects
value-level skew; the failure mode is miscompiled output or a confusing
far-downstream error, not "format mismatch". Mitigations worth having: bump
`ir_version` as a ritual for *any* numbering change (currently a convention
in comments only), and/or a shared generated numbering source, and/or a
round-trip test (`c02-frontend` → `c02-ld` → `c02-as --dump-ir`) diffing op
names — the pieces all exist (`--dump-ir` decodes a `.o`), no test wires them
together.

**Verified:** Source (three-way tag-table diff performed during audit —
currently in sync).

### IR-2: Length-prefixed on the wire, NUL-terminated in the consumers

The format stores every string as `[u32 len][bytes]` — fully binary-safe. The
consumers are not:

- **Embedded NUL** (reachable from source: `"ab\0cd"`, §1.4's `\0` escape):
  `ir-serial.c`'s `read_str` preserves the bytes but returns a bare `char*`;
  `emit_data_section` then measures with `strlen` and dedups with `strcmp` —
  data after the first NUL is dropped from ROM and distinct literals coalesce.
  **Executed**: `*(s+3)` of `"ab\0cd"` reads the `C` of the adjacent "C02S"
  symbol-table magic ([CG-7](FINDINGS_codegen.md#cg-7)). The length crosses
  the wire intact and is thrown away at the last hop.
- **`len == 0` means NULL/absent** in both readers (`read_str` → `NULL`,
  OCaml `str` → `None`) — so an *empty string literal* is indistinguishable
  from *no string*. Exactly one consumer site defends against the resulting
  NULL (`emit_data_section`'s coalesce, with a comment); `u8 *s = "";` works
  today (Executed) because that's the only path an empty literal reaches, but
  any new consumer of `str_val`/`name` inherits a latent NULL deref. The
  OCaml side turns the same length-0 record into `""` via
  `Option.value ~default` for most fields but `failwith` for others (VAR
  names, function names) — three different absent-string semantics across the
  two readers.
- **Non-ASCII**: the Haskell writer emits `string8` (low byte of each `Char`)
  with `length` counted in *characters* — any source string outside Latin-1
  silently mangles (`'€'` → byte `0xAC`), and even Latin-1 survives only
  byte-per-char by accident. Neither reader can detect it.

The contract should say: strings are byte sequences with explicit length,
NULs legal (then fix `emit_data_section` to use the length), or: strings are
NUL-free ASCII (then the *frontend* must reject `\0` escapes and non-ASCII
loudly). Today each implementation assumes a different one of these.

**Verified:** Executed (NUL truncation, `""` path) / Source (non-ASCII,
NULL-propagation inventory).

### IR-3: "Native endian, native width" actually means x86-64 LP64

All three implementations note the format is native-endian/native-width for
same-machine reuse. But the *widths* aren't native-symmetric:

- The Haskell writer commits to **little-endian** explicitly (`word32LE`,
  `int64LE`) — on a big-endian host it would emit LE while the C reader
  `fread`s native, a silent value-scramble that magic (`0x43303249` read
  reversed = fails) would luckily catch, but only via an "Invalid object
  file" with no hint why.
- The C reader reads the 8-byte fields into `unsigned long`/`long`
  (`read_u64`/`read_i64`) — **LP64-only**. On any LLP64 or 32-bit target
  (`long` = 4 bytes) `fread(v, 8, 1, f)` overflows the destination: stack
  corruption, not a format error.
- The OCaml reader narrows `Int64.to_int` (63-bit) silently; irrelevant for
  real 6502 addresses, but a garbage `.o` wraps instead of failing.

No header field records endianness or word size, so none of this is
*checkable* — the comments are the enforcement mechanism. One `u8`
endianness/width byte after the version, validated by both readers, converts
all of these from silent/UB to loud.

**Verified:** Source.

### IR-4: Counts and ids cross the wire unvalidated

The readers trust every count and id, and the *consumers* assume invariants
the format never states:

- **`cfg.next_label`** — `c02-as` allocates `next_label * sizeof(uint16_t)`
  (`emit_function_from_cfg`) and indexes it with each instruction's raw
  `label` field, unchecked. A writer bug (or hand-edited `.o`) with
  `label >= next_label` is an out-of-bounds heap **write** (in
  `TAC_LABEL`) / read (jumps) inside the assembler. The frontend comment
  "codegen trusts them, so they must be right" is the entire contract.
- **`next_temp`** is similarly trusted but happens to be harmless today (ZP
  slots come from a fixed-size map with its own bounds check).
- **Sequence counts** (`instr_count`, `call_arg_count`, section counts)
  multiply directly into arena allocations; garbage counts on truncated files
  fail cleanly (`fread` short-read → `goto fail`) *before* most damage, but a
  huge count with enough trailing bytes just exhausts memory
  (`arena_alloc` → `exit(1)`).
- **Tag ranges**: OCaml validates every tag (`failwith "bad tac_op"`, "bad
  operand kind", "bad type kind"). C validates **none** — `op->kind` and
  `ins->op` are cast from raw `u32`; an unknown operand kind falls through
  `read_operand`'s switch and **returns success with an uninitialized union**
  (no `default`), deferring to whatever the generator does with garbage. The
  two readers disagree about whether the reader is a validation boundary.

Given the linker also re-*writes* the format, `c02-ld` is the natural
choke-point to make these loud (it already fails on bad tags); `c02-as` needs
at minimum the `label < next_label` check, which is one comparison per
control-flow instruction.

**Verified:** Source.

### IR-5: Semantic invariants that ride on top of the syntax

Things every consumer assumes about a *well-formed* module that the format
neither expresses nor checks — each one a silent seam if the writer changes:

- **Four ops are dead on the wire.** `TAC_AND`, `TAC_OR`, `TAC_BREAK`,
  `TAC_CONTINUE` exist in all three enums; the frontend never emits them
  (`&&`/`||` lower to short-circuit control flow matching `bin/cc02`;
  break/continue become jumps), and the generator has **no cases** for them —
  emitting one produces "codegen: unhandled TAC op" at the last stage rather
  than a format-level error at the first. A frontend contributor who "just
  uses TacAnd" gets a working `.o` that fails only under `c02-as`.
- **`is_ptr` duplicates `ptr_depth`** — and the sides disagree about which is
  authoritative. The OCaml reader discards `is_ptr` (recomputes from depth);
  the C side stores both and **sizes by `is_ptr`** (`codegen_type_size`
  checks `type.is_ptr` first). A writer emitting `is_ptr=0, ptr_depth=1`
  (trivial bug — the Haskell writer derives `is_ptr` from depth today) gives
  OCaml a pointer and C a 1-byte scalar: the same `.o` means two different
  programs to the two consumers. Either drop `is_ptr` from the format
  (version bump) or have readers reject disagreement.
- **CFGs have exactly one block, id 0.** The format supports arbitrary
  blocks; `successors` is explicitly not serialized ("rebuild from
  terminators after load" says `tac.ml`) and *nobody rebuilds it* —
  the generator walks blocks linearly and resolves labels flat, which is
  only correct because block boundaries carry no meaning in today's
  single-block modules. Multi-block emission (the frontend's own "CFG
  carving" is a stated next step) will work by accident or break subtly
  depending on emission order, with no format-level statement of which.
- **Instruction shape is "fat"**: every field present for every op, absent =
  empty form (0-length string, zeroed type, `OPERAND_NONE`). This one *is*
  consistently implemented in all three — worth writing down as the rule new
  fields must follow (append-only, always-present), because the first
  conditionally-present field will desync readers that don't branch
  identically.

**Verified:** Source (each point traced in at least two implementations).

### IR-6: Struct identity is a bare name with pre-baked layout

The format carries struct layouts (name, fields, offsets, total size) *and*
struct-typed operands that reference them **by name only**. Consequences:

- Offsets/sizes are computed once, by the frontend, and consumed verbatim —
  `c02-as` never recomputes (by design: "codegen never recomputes a layout",
  `TAC.hs`). So the layout table is load-bearing for every field access and
  every struct-sized copy, but nothing ties an *operand's* struct name to a
  *table entry's* existence.
- If the name lookup misses, `full_type_size` in `c02-as` silently returns
  **1** (its scalar default) — a struct-typed temp gets a 1-byte ZP slot and
  copies collapse to a byte, with no diagnostic. Today the frontend always
  emits an entry for every name it uses; the case is reachable the moment the
  emission set and the use set are computed by different code (exactly what
  [FE-17](FINDINGS_frontend.md#fe-17)'s name-collision handling will touch).
  `lookup_struct_field` *does* fail loudly — so field access is guarded, but
  sizing is not: the two lookups have different failure semantics on the same
  missing entry.
- Because identity is the bare name, scoping is unexpressable: function-local
  structs serialize under their source name and collide in the linker's flat
  dedup ([LD-2](FINDINGS_linker.md#ld-2)), and the same-name-different-layout
  case inside one module is resolved by silent map-overwrite before
  serialization even happens (FE-17). The format would need scoped/mangled
  names (or per-CFG tables) to represent what the language allows.

**Verified:** Source (fallback path `generator.c:60-68`; collision behavior
Executed under FE-17/LD-2).

### IR-7: Extern resolution is a codegen-stage whitelist, not a format concept

The `externs` section is the format's entire cross-TU story, and its
consumption is asymmetric in ways no document states:

- `c02-ld` resolves externs against definitions (loudly checking signatures)
  and passes *unresolved* ones through — for **any** link, there being no
  format distinction between a relocatable and a final object
  ([LD-7](FINDINGS_linker.md#ld-7)).
- `c02-as` then consumes externs via `emit_compiler_extern_inits`: extern
  **variables** are checked against a hardcoded whitelist
  (`__heap_start`, `__memory_top` — the §4.6 "compiler implicit globals") and
  anything else is a loud error; extern **functions** are skipped entirely
  and surface only if a call site's JSR fixup fails to resolve — a declared,
  never-called extern function vanishes silently (legitimate, but nowhere
  stated).
- So the answer to "which stage guarantees all symbols resolve?" is: *no
  single stage* — variables at codegen unconditionally, functions at codegen
  only-if-called, and the linker never. The compiler-implicit globals
  themselves (§4.6 says "injected automatically — no `decl` needed") are in
  fact **not injected** by this frontend: they work only if the program
  writes `decl u16 __heap_start;` itself or links `libc02` (whose header
  does — with the wrong type; see
  [FE-22](FINDINGS_frontend.md#fe-22), Executed), a frontend/spec gap that
  happens to be *observable* purely through this extern pathway.

Worth one paragraph in a real format spec: the externs section's lifecycle,
who may leave it non-empty, and the whitelist. Today that knowledge lives in
one C function and this document.

**Verified:** Source (whitelist `generator.c:1868-1890`; linker pass-through
Executed in the `unresolved_extern_partial` suite case).
