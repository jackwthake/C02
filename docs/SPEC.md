# The c02 Language — Formal Specification

**Status:** unofficial, derived from reading `cc02`'s tokenizer, parser, and
semantic analyzer source directly (not from the README alone). Pinned to
[jackwthake/C02](https://github.com/jackwthake/C02) commit `9a9375e`
(2026-07-02, branch `v1.1`).

**Scope:** this document specifies the language *surface* — lexical grammar,
syntax, and static semantics (types, scoping, declarations) — as accepted by
the frontend (tokenizer → parser → analyzer). It deliberately excludes code
generation, the ROM/RAM memory layout, the 65C02 ABI, and zero-page register
allocation; see `README.md` and `docs/memmap.md` for that.

**Purpose:** this is the ground-truth reference for
[C02-fuzz](https://github.com/jackwthake/C02-fuzz), a differential-testing
harness for the c02 compiler. Two things follow from that:

1. Every rule below is stated *normatively* — what a conforming program means,
   and what the frontend is supposed to accept or reject. Where today's
   `cc02` binary does not actually implement a stated rule, that gap is
   flagged inline with a terse `⚠ [ID](...)` marker linking to the full
   write-up in the companion document,
   [`DEVIATIONS_c_impl.md`](DEVIATIONS_c_impl.md) — see [§9](#9-known-deviations-from-this-spec).
   A fuzzer generating or classifying test cases should treat deviations as
   *expected, already-known* divergences, not new findings — unless
   behavior changes from what's recorded there. These deviations live in the
   current `v1.1` branch implementation. This branch's (`rewrite/c02-haskell-frontend`)
   goal is to address the frontend deviations.
2. Nothing here should be taken as "codegen is definitely correct" —
   `DEVIATIONS_c_impl.md` documents nine distinct silent-miscompile classes that
   are accepted by analysis but produce wrong machine code. A
   conforming-per-this-spec program can still misbehave at runtime today.

---

## Table of Contents

1. [Lexical Grammar](#1-lexical-grammar)
2. [Grammar Overview](#2-grammar-overview)
3. [Types](#3-types)
4. [Top-Level Declarations](#4-top-level-declarations)
5. [Statements](#5-statements)
6. [Expressions](#6-expressions)
7. [Scoping & Name Resolution](#7-scoping--name-resolution)
8. [Diagnostics Catalog](#8-diagnostics-catalog)
9. [Known Deviations From This Spec](#9-known-deviations-from-this-spec)

---

## 1. Lexical Grammar

### 1.1 Keywords

Reserved, case-sensitive, matched greedily against maximal identifier length
(so `fnord` lexes as one identifier, not `fn` + `ord`):

```
fn  decl  reg  struct  return  if  else  while  for  break  continue
interrupt  asm  void  u8  i8  u16  i16
```

`decl` is the forward-declaration keyword (spelled `decl`, not `extern`).

> **Not keywords, despite reading like them:** `true`, `false`, and `null`
> are lexed directly to numeric-literal tokens with values `1`, `0`, and `0`
> respectively, *before* identifier scanning runs. There is no boolean or
> null-pointer literal node in the AST — by the time the parser sees them
> they are indistinguishable from writing `1`, `0`, `0` outright. One
> practical consequence: they cannot be used as identifiers (pre-empted at
> the lexer, not merely reserved by the parser), but there is also no
> separate "boolean type" anywhere in the type system — `true` has type
> `u8`, not some `bool`.

### 1.2 Identifiers

`[A-Za-z_][A-Za-z0-9_]*`, standard C rules. No length limit enforced by the
lexer.

### 1.3 Integer Literals

Three radixes, `strtol`-parsed:

| Form | Example |
|---|---|
| Decimal | `42` |
| Hexadecimal (`0x`/`0X` prefix) | `0xFF` |
| Binary (`0b`/`0B` prefix) | `0b1010` |

- A literal overflowing the host `long` (i.e. absurdly long digit strings) is
  a **lexer error** ("integer literal is too large to represent").
- A malformed literal (`0x` with no hex digits following) is a **lexer
  error**; the lexer consumes the bad prefix and keeps tokenizing, so a file
  can accumulate multiple lexer errors before compilation aborts.
- Overflow of the literal against a *c02* integer type's range (i.e. it lexes
  fine as a host `long` but doesn't fit in `-32768..65535`) is **not** a
  lexer concern — it's `ERR_LITERAL_OUT_OF_RANGE`, raised during semantic
  analysis (§8).
- There are no floating-point literals and no character literals (`'x'` is
  not a token form at all — the tokenizer has no single-quote handling
  outside of the `\'` escape inside a string).

### 1.4 String Literals

Double-quoted. Recognized escapes: `\n \t \r \0 \\ \" \'`. Any other
`\<char>` silently drops the backslash and keeps `<char>` verbatim (no
lexer error for an unrecognized escape). A string cannot span a literal
newline — hitting `\n` or EOF before the closing `"` is a **lexer error**
("unterminated string literal").

Every string literal has type `u8*` — a single-level pointer to `u8`, with
no length/const information carried in the type (§3).

### 1.5 Comments

```c
// line comment, to end of line

/* block comment */
```

Block comments do **not** nest — the first `*/` closes it regardless of
intervening `/*`.

> ⚠ [G-1](DEVIATIONS_c_impl.md#g-1-unterminated-block-comment-silently-swallowed):
> an unterminated `/*` is silently consumed to EOF with no lexer error
> (asymmetric with the erroring unterminated-string case).

### 1.6 Operators & Punctuation

```
@ -> / /= ( ) { } ; & , . % %= + ++ - -- = * *= == += -= ! != < > <= >= && || | ~ ^ << >>
```

Notably **absent** from the token set entirely (not merely unimplemented —
these tokens do not exist, so no sequence of characters can ever lex to
them):

- `[` `]` — no array/subscript syntax at any level.
- `?:` — no ternary operator.
- `&=` `|=` `^=` `<<=` `>>=` — no compound bitwise/shift assignment. (The
  arithmetic compound forms `+= -= *= /= %=` do exist — see §5.3.)
- A postfix `->` — `->` is exclusively the function-return-type introducer
  (`fn f() -> u8`); there is no arrow member-access operator (`.` auto-derefs
  one struct-pointer level instead — §6.4).

---

## 2. Grammar Overview

EBNF-style summary; `?` = optional, `*` = zero-or-more, `|` = alternation.
Terminals are quoted; `IDENT`, `NUMBER`, `STRING` are lexer tokens from §1.

```ebnf
program        ::= toplevel*

toplevel       ::= function_decl | reg_decl | struct_decl
                  | global_var_decl | fwd_decl

function_decl  ::= "fn" IDENT "(" param_list? ")" "interrupt"? "->" type block
param_list     ::= param ("," param)* ","?
param          ::= type IDENT

reg_decl       ::= "reg" type IDENT "@" NUMBER ";"

struct_decl    ::= "struct" IDENT "{" field_decl* "}" ";"?
field_decl     ::= type IDENT ";"

global_var_decl ::= type IDENT ( ";" | "=" expr ";" )

fwd_decl       ::= "decl" ( "fn" IDENT "(" param_list? ")" "interrupt"? "->" type ";"
                           | type IDENT ";" )

type           ::= ( base_type | IDENT ) "*"*
base_type      ::= "u8" | "i8" | "u16" | "i16" | "void"

block          ::= "{" stmt* "}"

stmt           ::= var_decl | struct_decl | expr_or_assign_stmt
                  | if_stmt | while_stmt | for_stmt
                  | return_stmt | break_stmt | continue_stmt
                  | asm_stmt | block

var_decl       ::= type IDENT ( ";" | "=" expr ";" )

expr_or_assign_stmt ::= expr ( assign_op expr )? ";"
assign_op      ::= "=" | "+=" | "-=" | "*=" | "/=" | "%="

if_stmt        ::= "if" "(" expr ")" block
                    ( "else" "if" "(" expr ")" block )*
                    ( "else" block )?

while_stmt     ::= "while" "(" expr ")" ( block | ";" )

for_stmt       ::= "for" "(" for_init? ";" expr? ";" for_incr? ")" ( block | ";" )
for_init       ::= ( type IDENT ( "=" expr )? ) | ( expr ( assign_op expr )? )
for_incr       ::= expr ( assign_op expr )?

return_stmt    ::= "return" expr? ";"
break_stmt     ::= "break" ";"
continue_stmt  ::= "continue" ";"

asm_stmt       ::= "asm" "{" IDENT* "}" ";"?

expr           ::= logical_or
logical_or     ::= logical_and ( "||" logical_and )*
logical_and    ::= bitwise_or ( "&&" bitwise_or )*
bitwise_or     ::= bitwise_xor ( "|" bitwise_xor )*
bitwise_xor    ::= bitwise_and ( "^" bitwise_and )*
bitwise_and    ::= equality ( "&" equality )*
equality       ::= comparison ( ("==" | "!=") comparison )*
comparison     ::= shift ( ("<" | ">" | "<=" | ">=") shift )*
shift          ::= term ( ("<<" | ">>") term )*
term           ::= factor ( ("+" | "-") factor )*
factor         ::= unary ( ("*" | "/" | "%") unary )*
unary          ::= ("!" | "-" | "&" | "~" | "++" | "--" | "*" | "@") unary
                  | postfix
postfix        ::= primary ( "." IDENT )*
primary        ::= NUMBER | STRING | IDENT
                  | IDENT "(" arg_list? ")"                (* call *)
                  | IDENT "{" init_list? "}"                (* struct init *)
                  | "(" type ")" logical_or                 (* cast — see §6.6 *)
                  | "(" logical_or ")"                       (* grouping *)
arg_list       ::= expr ("," expr)* ","?
init_list      ::= "." IDENT "=" expr ("," "." IDENT "=" expr)* ","?
```

Each `BINOP_LEVEL` in the expression chain is strictly **left-associative**;
the unary chain is right-associative (self-recursive, so `!!x`, `--*p`,
`&*p` all stack). `assign_op` is **not** part of `expr` — see §6.1.

---

## 3. Types

### 3.1 Type Space

```
u8  i8  u16  i16          — 8-bit / 16-bit integers, unsigned / signed
void                       — only legal as a function return type, or as
                              the pointee of a pointer (`void*`)
StructName                  — a struct type, matched by name only (no
                              structural/anonymous struct types)
T*, T**, T***, ...          — pointer to T at arbitrary depth (parser places
                              no upper bound on `*` count)
```

There is no array type, no function-pointer type, no boolean type
(`u8` stands in for booleans — see §1.1), and no floating-point type.

### 3.2 Type Compatibility

`is_types_compatible(expected, actual)` governs every implicit
type-checking site (initializers, assignment, `return`, call arguments,
binary operands, struct-init fields). In order:

1. If `actual` is the **bare integer literal `0`** used with no other type
   information (the "null-literal" — §3.4) **and `expected` is a pointer
   type** (any `ptr_depth >= 1`, including `void*`), **compatible** —
   unconditionally, regardless of pointee type or depth (the
   null-pointer-constant idiom). This carve-out exists because the
   literal's own inferred type — "null type", `void` at `ptr_depth 1`
   (§3.4) — would otherwise fail rule 6's base-kind match against a
   concrete pointee like `u8`. For a **non-pointer** `expected` (a scalar or
   a by-value struct), the literal `0` is intended to get no special
   exemption: its `void`-at-`ptr_depth-1` inferred type is `is_ptr = true`,
   so it should be rejected by rule 3 like any other pointer-vs-non-pointer
   mismatch — `Point p = 0;` is not meant to be a well-formed program (see
   [S-17](DEVIATIONS_c_impl.md#s-17-bare-literal-0-bypasses-the-destination-type-check-entirely)
   for how `cc02` actually handles this today).
2. Else if `expected` is `void*` (`kind === "void"`, `ptr_depth === 1`) and
   `actual` is *any* pointer type **also at `ptr_depth === 1`**,
   **compatible** — regardless of pointee kind, but depth must match. A
   `u8**` (or any `ptr_depth >= 2` pointer) does **not** convert to `void*`
   under this rule; it falls through to the ordinary depth-mismatch
   rejection (rules 3–4) and needs an explicit cast, same as any other
   depth mismatch (see
   [S-18](DEVIATIONS_c_impl.md#s-18-void-accepts-pointers-of-any-depth-not-just-depth-1)
   for how `cc02` actually handles this today).
3. Else if `expected.is_ptr != actual.is_ptr`, **not compatible**.
4. Else if pointer depths differ, **not compatible**.
5. Else if both are struct types, compatible iff the struct **names** match
   exactly (no structural/field-list equivalence).
6. Else if the base kinds match (`u8`≡`u8`, but see the signedness note
   below), **compatible**.
7. Else if either side is a struct or `void`, **not compatible**.
8. Else, **only applicable when both `expected` and `actual` have
   `ptr_depth === 0`** (i.e. neither is a pointer — by this point pointers
   only reach here with matching depth and a non-void/non-struct pointee
   mismatch, e.g. `u8*` vs `u16*`, and there is no pointee-widening for
   pointers: an exact pointee-kind match, or an explicit cast, is required,
   same as if this were a struct-name mismatch — see
   [S-19](DEVIATIONS_c_impl.md#s-19-pointer-pointee-types-silently-widen-like-scalar-values)
   for how `cc02` actually handles this today). For two non-pointer
   values, **compatible iff `signedness(actual) === signedness(expected)`
   and `width(actual) <= width(expected)`** — implicit widening only within
   the same signedness (`u8`→`u16` OK; `u16`→`u8` requires an explicit
   cast). Crucially, this also means `u8`→`i16` (or any cross-signedness
   pair) is **not** compatible even though it fits width-wise — crossing
   signedness always requires an explicit cast, same as narrowing does.
   `width` is 1 for `u8`/`i8`, 2 for `u16`/`i16`; `signedness` is unsigned
   for `u8`/`u16`, signed for `i8`/`i16` (see
   [S-2](DEVIATIONS_c_impl.md#s-2-no-signedness-checking) for how `cc02` actually
   handles this today).

Rule 1 is exclusive to the **bare literal `0`** — it does not extend to a
`void*`-typed variable or any other non-literal expression of type `void*`.
A genuine `void*` value is an ordinary pointer (depth 1, pointee `void`) and
is checked like any other pointer under rules 2–4: compatible with a
`void*`-expecting destination (rule 2), or with a matching pointer
type/depth (rules 3–4) — **not** compatible with a non-pointer destination.

> ⚠ [S-1](DEVIATIONS_c_impl.md#s-1-voidnull-literal-conflation): `cc02` doesn't
> draw this distinction — internally, the literal `0` and every `void*`-typed
> *value* share one representation ("the null type": `void` with pointer
> depth 1), and rule 1 fires on that shared representation unconditionally.
> So a **named `void*` variable**, not just the literal `0`, is incorrectly
> compatible with any destination type — including non-pointer scalars and
> by-value structs.

> ⚠ [S-17](DEVIATIONS_c_impl.md#s-17-bare-literal-0-bypasses-the-destination-type-check-entirely):
> the literal `0` itself has the same problem, independent of S-1 — `cc02`'s
> compatibility check never inspects `expected`'s pointer-ness before
> granting the literal its exemption, so `0` is accepted against *any*
> destination, including a by-value struct (`Point p = 0;` compiles today).
> This is not the intended design (§3.2 rule 1 above states the intended,
> pointer-only scope).

**BinOP expression with mixed signedness**: A binary operator with one signed and one unsigned operand is a type error; an explicit cast on one operand is required.

> ⚠ [S-2](DEVIATIONS_c_impl.md#s-2-no-signedness-checking): `i8 x = 200;` and
> `u8 y = someI8Var;` both pass with no diagnostic — width is checked, sign
> is not.

### 3.3 Casts

`(type)expr` — see §6.6 for the precedence/binding subtlety. Semantically:

- Casting **to** a struct type by value (not `StructName*`) is always
  rejected: `ERR_STRUCT_CAST_BY_VALUE`.
- Casting **to** an unregistered struct name is rejected: `ERR_UNKNOWN_STRUCT`.
- Casting to any other destination type (including `u8`↔struct-unrelated
  scalars, or between unrelated pointer types) is accepted **with no
  relatedness check whatsoever** — the source expression's type is resolved
  (for its own diagnostics) but never compared against the destination.

### 3.4 Integer Literal Typing

When no typed context is available, the literal's value determines its type as follows:

| Range | Type |
|---|---|
| `0` | null type (`void*`-shaped) — see §3.2 |
| `1..255` | `u8` |
| `-128..-1` | `i8` |
| `256..65535` | `u16` |
| `-32768..-129` | `i16` |
| anything else | `ERR_LITERAL_OUT_OF_RANGE` |

Negative literals only arise as `NODE_UNARY(-)` wrapping a `NODE_NUMBER`
directly; the analyzer special-cases exactly that AST shape to re-derive a
signed type from the negated value.

**Untyped literals**: An untyped literal adopts the signedness/width of its context (assignment target, binary operand, call argument); with no context, it defaults to the narrowest type that fits.

**Explicit Casting between signed and unsigned**: An explicit signed ↔ unsigned cast is bit-pattern-preserving (two's-complement reinterpretation), matching the wraparound semantics used elsewhere in the integer model.

**Negation is type-preserving**: `-x` retains `x`'s exact type — signedness and width are unchanged (a negated `u8` is still `u8`), following standard two's-complement wraparound (§Appendix B); no range check is performed at compile or runtime. This applies to any negated expression other than a bare literal, which instead follows the literal-typing special case above.

---

## 4. Top-Level Declarations

A `.c02` file is a sequence of top-level items; each is one of:

### 4.1 Functions

```c
fn name(type param, ...) -> type {
  // body
}
```

- Parameter list is mandatory (`()` for none); no default values, no
  varargs.
- `-> type` is mandatory — no implicit-void return omission.
- Body is a mandatory `{ }` block; a `fn` with no body is a parse error (use
  `decl fn ...;` instead — §4.5).

### 4.2 Interrupt Functions

```c
fn irq() interrupt -> void { ... }
```

- `interrupt` sits exactly between `)` and `->`; nowhere else.
- Valid only when the function is named exactly `nmi` or `irq`
  (case-sensitive), returns plain `void` (not `void*`), and takes zero
  parameters. All three conditions must hold.
- **If any condition fails, this is a warning
  (`WARN_INVALID_INTERRUPT`), not an error** — the function compiles as an
  ordinary callable function, with `is_interrupt` cleared before codegen
  ever sees it. The program **builds successfully**; only stderr shows the
  warning. A conscious design choice, not a gap: a typo like
  `fn Nmi() interrupt -> void { ... }` builds successfully, and the vector
  table simply doesn't point at the intended handler — watch stderr.
- `irq()` is maskable (`__enable_interrupts()`, a compiler builtin
  implemented as `asm { CLI }`, must be called before it fires); `nmi()` is
  non-maskable.
- Calling `nmi()`/`irq()` directly like an ordinary function
  (`irq();`) is **currently accepted** by the analyzer with no check that
  the callee is an interrupt handler — see
  [P2-3](DEVIATIONS_c_impl.md#p2-3-interrupt-handlers-can-be-called-directly).

### 4.3 Registers (`reg`)

```c
reg u8 PORTB @ 0x6000;
```

- `type` follows the general grammar, including pointer stars (`reg u8 *X @
  ...;` parses, semantics unspecified/not analyzer-checked).
- The address must be a bare integer literal token (decimal/hex/binary), not
  an arbitrary constant expression.
- ⚠ The address is not range-checked against `0xFFFF` anywhere in the
  pipeline — see
  [P2-1](DEVIATIONS_c_impl.md#p2-1-reg-addresses-above-0xffff-are-silently-truncated).

### 4.4 Structs

```c
struct Point {
  u8 x;
  u8 y;
}
```

- Body is a sequence of `type name;` fields only — no field initializers, no
  nested struct-body definitions, no methods.
- Trailing `;` after `}` is optional.
- Empty struct bodies (`struct Empty {}`) are legal.
- **Struct declarations are legal as in-block statements too**, not just
  top-level (`parse_stmt` dispatches `struct` directly) — a struct can be
  declared inside a function body.
- **By-value fields require textual (declaration-order) precedence**: a
  field `Inner inner;` inside `struct Outer` is only legal if `struct Inner`
  was declared **earlier in the source file** than `struct Outer` — checked
  by a literal position scan over top-level items, independent of the
  otherwise fully forward-reference-tolerant symbol table. **Pointer**
  fields (`Inner *inner;`) have no such restriction, including
  self-reference (`struct Node { Node *next; }` is fine).
- A by-value field whose type is the struct's own name
  (`struct S { S s; }`) is always rejected (`ERR_INCOMPLETE_STRUCT_FIELD`,
  "cannot contain itself by value").

### 4.5 Global Variables

```c
u8 *msg = "Hello C02!";
u16 counter;
Point origin;
```

Same shape as a local declaration (§5.1): `type name;` or
`type name = expr;`. The initializer is type-checked in **global scope**
(so it may reference any other global/function declared anywhere in the
file, not just earlier ones).

> ⚠ [P0-5](DEVIATIONS_c_impl.md#p0-5-non-literal-global-initializers-are-silently-dropped):
> only a bare number/string literal initializer is actually captured —
> every other shape (`2 + 3`, `-5`, a struct initializer) is silently
> dropped, leaving the global's storage unwritten.

### 4.6 Forward Declarations (`decl`)

```c
decl fn send_byte(u8 b) -> void;
decl u8 counter;
```

- Function form: same signature grammar as `fn`, no body, terminated by
  `;`.
- Variable form: `decl type name;` — no initializer permitted (a parse
  error if one is written).
- Intended for cross-translation-unit references (multi-file linking,
  incremental `-c` compilation).

> ⚠ [S-7](DEVIATIONS_c_impl.md#s-7-decl-cannot-prototype-a-same-file-definition):
> a `decl` followed by a same-file definition of the same name is rejected
> as `ERR_REDECLARATION` — `decl` is cross-file only, not an in-file
> prototype idiom.
>
> ⚠ [G-2](DEVIATIONS_c_impl.md#g-2-decl-fn--interrupt-drops-the-qualifier):
> `decl fn irq() interrupt -> void;` parses, but the `interrupt` qualifier
> is silently discarded — the AST has no field to store it.

#### Compiler Implicit Globals

`__heap_start` and `__memory_top` (both `u16`) are injected automatically —
no `decl` needed, available in every translation unit. See the main repo's
`README.md` for their exact values (a codegen/runtime detail, out of scope
here).

---

## 5. Statements

### 5.1 Variable Declarations

```c
u8 x = 5;
Point p;                       // struct-typed, no initializer
Point *p2 = &p;
```

`type name;` or `type name = expr;`. If an initializer is given, its type
must be compatible (§3.2) with the declared type.

### 5.2 Struct Initializer Expressions

```c
p = Point{ .x = x, .y = 10 };
p = Point{};                   // zero fields given
```

Not a distinct statement form — `Name{ ... }` is parsed inside `primary()`
(§6), so it's legal anywhere an expression is, not only as an assignment
RHS (`foo(Point{.x=1,.y=2})` is syntactically legal).

- Fields: `.name = expr`, comma-separated, **trailing comma tolerated**
  (`Point{ .x = 1, }` parses).
- Field order in the initializer need not match declaration order.
- ⚠ [S-6](DEVIATIONS_c_impl.md#s-6-struct-initializers-neednt-be-complete-or-unique):
  omitted fields produce no diagnostic, and a duplicate field entry is
  never flagged.

### 5.3 Assignment

```c
x = x + 1;
x += 1;    // also: -= *= /= %=
*p = 5;
a.b.c = 5;
```

- Target is parsed as a **full expression**, not restricted to identifiers —
  lvalue-ness is checked *after* parsing (§7.3), not enforced by the
  grammar.
- Compound operators: **only** `+= -= *= /= %=` exist (checked at both the
  lexer and parser level — no bitwise/shift compound tokens exist anywhere).
  Desugars to `target = target OP rhs` in the AST; the target subtree is
  shared, not re-parsed.
- **Assignment is not an expression.** `=` never appears in the expression
  grammar (`expr`/`logical_or`/.../`primary`) — it is handled exclusively by
  statement-level and for-incrementer productions. Consequences:
  - No chained assignment: `a = b = c;` is a parse error.
  - `=` cannot appear inside a condition or call argument:
    `if (x = 5)` and `foo(x = 5)` are both parse errors.

### 5.4 Control Flow

```c
if (x > 0) { ... }
else if (true) { ... }
else { ... }

while (x < 10) { x += 1; }
while (cond);              // empty body

for (u8 i = 0; i < 10; i += 1) { ... }
for (;;) { ... }            // all three clauses independently optional
```

- `if`/`else if`/`else` chains are unlimited length.
- `while`/`for` accept a bare `;` in place of `{ }` for an empty body.
- `break`/`continue` are legal only inside a `while` or `for` body
  (including through nested `if`s — loop-depth tracking is not
  scope-local); otherwise `ERR_BREAK_OUTSIDE_LOOP` /
  `ERR_CONTINUE_OUTSIDE_LOOP`.
- `if`/`while`/`for` **conditions are not required to be scalar** — the
  analyzer resolves the condition's type but never checks it's not a struct
  or `void`. See
  [P2-2](DEVIATIONS_c_impl.md#p2-2-struct-values-accepted-as-arithmeticcondition-operands).

### 5.5 `for`-Loop Clauses

```ebnf
for_stmt  ::= "for" "(" for_init? ";" expr? ";" for_incr? ")" ( block | ";" )
for_init  ::= ( type IDENT ( "=" expr )? ) | ( expr ( assign_op expr )? )
for_incr  ::= expr ( assign_op expr )?
```

Each clause is independently optional (empty init/empty cond/empty incr are
all legal, signaled by an immediate `;` or `)`). The init clause's
non-declaration branch is now identical to the incrementer: a full expression
optionally followed by an assignment operator and right-hand side, so
`for (i = 0; ...)` reusing an existing variable is well-formed.

> ⚠ [G-3](DEVIATIONS_c_impl.md#g-3-for-init-cannot-reuse-an-existing-variable):
> `cc02` rejects reusing an existing variable via plain assignment in the
> init clause — `for (i = 0; ...)` is a parse error there unless `i` is
> freshly declared right there — even though the grammar above permits it.
> `cc02`'s incrementer clause *does* accept `=`/compound-assign, so the
> asymmetry is internal to `cc02`, not a property of the language.

This clause disambiguates "declaration vs. expression" using the struct-name
prescan (§6.6) — the same mechanism as casts and ordinary block statements
(§7.1). See [G-4](DEVIATIONS_c_impl.md#g-4-block-statements-disambiguate-by-shape-not-the-prescan)
for how `cc02`'s block-statement disambiguation diverges from that shared rule.

### 5.6 Return / Break / Continue

```c
return;      // only legal if the enclosing function's return type is void
return x;    // x's type must be compatible with the declared return type
break;
continue;
```

### 5.7 Inline Assembly (`asm`)

```c
asm {
  SEI
  NOP
  CLI
}
```

- A sequence of bare, no-operand opcode mnemonics, one per entry, emitted
  verbatim. No operands, addressing modes, or in-block labels.
- The parser accepts **any** bare identifier as a "mnemonic" with zero
  validation — legality of the specific mnemonic is deferred entirely to
  codegen (out of scope for this document; see the main repo's README for
  the currently-supported mnemonic list).
- ⚠ The analyzer performs **no check at all** on `asm` blocks — an invalid
  mnemonic produces no semantic-analysis diagnostic. See
  [S-10](DEVIATIONS_c_impl.md#s-10-asm-blocks-are-unvalidated).

### 5.8 Prefix-Only Increment/Decrement

```c
++x;   --x;   ++*p;   --field;
```

Only the **prefix** form exists at the token/grammar level (`s_plus_plus` /
`s_minus_minus` are recognized only inside `unary()`). `x++;` is a **parse
error** — after `x` parses as a primary identifier, a trailing `++` is left
unconsumed and the statement's required `;` fails to match.

---

## 6. Expressions

### 6.1 Precedence (loosest → tightest binding)

```
||  &&  |  ^  &  ==  !=  <  >  <=  >=  <<  >>  +  -  *  /  %  (unary)  (postfix)
```

Every binary level is strictly left-associative. This matches ordinary C
precedence, including shift sitting between relational and additive.

### 6.2 Unary (Prefix) Operators

Right-associative (self-recursive, so they stack: `!!x`, `--*p`, `&*p`):

| Operator | Meaning |
|---|---|
| `!` | logical not |
| `-` | negate |
| `&` | address-of (operand must be an lvalue — §7.3) |
| `~` | bitwise not |
| `++` / `--` | prefix increment/decrement (operand must be an lvalue) |
| `*` / `@` | pointer dereference — **`*p` and `@p` are interchangeable spellings of the identical operation** |

`!`, `-`, `~`, and `++`/`--` are all type-preserving: the result has the exact
same type as the operand, no widening or signedness change (§3.4 covers `-`'s
one exception, the bare-literal fold). Unlike `&&`/`||` (§6.3), `!` does not
force a `u8` result — `u8 a = !x;` on an `i16 x` is a type error, while
`i16 b = !x;` type-checks. **Verified.**

### 6.3 Binary Operators

`|| && | ^ & == != < > <= >= << >> + - * / %` — standard meanings.

- **Pointer arithmetic**:
  - `ptr + int` and `int + ptr` both produce a pointer of the same type as
    the pointer operand, bypassing the normal type-compatibility check
    entirely (any integer width/signedness accepted as the offset) —
    addition is **commutative**, regardless of which side the pointer is on.
  - `ptr - int` likewise produces a pointer of the same type as `ptr`;
    `int - ptr` is a type error — subtraction isn't commutative, there's no
    sensible "int minus pointer."
  - `ptr - ptr` (both operands the same pointer type) also produces a
    pointer of that same type — the address difference, not an integer
    count.
  > ⚠ [S-3](DEVIATIONS_c_impl.md#s-3-pointer-arithmetic-is-order-sensitive):
  > `cc02`'s binary-operator special case only checks `left.is_ptr` — so
  > `ptr + 5` compiles but `5 + ptr` is rejected as a type error; the
  > commutative `int + ptr` form isn't implemented.
  > ⚠ [P2-7](DEVIATIONS_c_impl.md#p2-7-pointer-arithmetic-is-unscaled): pointer
  > arithmetic is unscaled at codegen time — `p + 1` always advances one
  > byte regardless of pointee size (this applies to `ptr - ptr`'s address
  > difference too — it's a raw byte count, not scaled by pointee size).
- Struct-typed operands are **not rejected** by the analyzer as long as both
  sides name the same struct — `pointA + pointB` "type-checks." See
  [P2-2](DEVIATIONS_c_impl.md#p2-2-struct-values-accepted-as-arithmeticcondition-operands).
- **Result-type widening**: for any binary operator except `&&`/`||`, when
  the two operands share signedness but differ in width, the result type is
  the **wider** of the two operand types — the narrower operand is
  implicitly widened before the operation executes (see Appendix B).
  Mismatched signedness between operands is a type error regardless of
  width (see the signedness note above) — width-widening only applies once
  signedness already agrees. This includes comparisons: there is no
  distinct boolean/comparison result type — `a == b` has type "whichever
  operand is wider," not a fixed 1-byte boolean.
- `&&` and `||` are the one exception to the above: they always produce a
  **`u8`** result regardless of operand type or width, since they're
  truthiness tests rather than width-preserving arithmetic (see
  [S-20](DEVIATIONS_c_impl.md#s-20-logical-operators-widen-their-result-instead-of-always-producing-u8)
  for how `cc02` actually handles this today).

### 6.4 Postfix — Field Access

```c
a.b.c        // chains arbitrarily
ptr.field    // auto-derefs one level if ptr : Struct*
```

`.` is the **only** postfix operator (no `->`, no `[]`). It auto-peels
**exactly one** pointer level when the base type is `Struct*` (`ptr_depth ==
1`); a `Struct**` base does **not** get this treatment and is rejected as
"not a struct" (`ERR_NOT_A_STRUCT`) — an explicit `(*pp).field` is required
(the inner deref brings it to `Struct*`, which then auto-derefs the
remaining level).

### 6.5 Calls

`name(arg1, arg2, ...)` — only directly after a bare identifier. The result
of a call or field access cannot itself be called
(`getStruct().method()`-style chaining is not a grammar form — there is no
`->`/methods at all, and only `IDENT(...)` is a call site, not
`expr(...)`). Arguments are comma-separated with a **tolerated trailing
comma**.

- Argument **count** mismatch (`ERR_WRONG_ARG_COUNT`) is checked before any
  argument is type-checked — a wrong-arity call never evaluates its
  arguments' types at all.
- The **first** incompatible argument aborts type-checking for the whole
  call — at most one `ERR_TYPE_MISMATCH` (generic context `"function
  call"`, not naming which argument) is ever emitted per call site.

### 6.6 Casts vs. Grouped Expressions

```c
(u16)x        // cast
(a) - b       // grouped expression, then subtraction
```

Both start `( IDENT ...`, so disambiguation requires knowing whether the
identifier names a type. The parser resolves this with a **one-time,
whole-file, scope-blind prescan**: before parsing begins,
`prescan_struct_names` walks the entire flat token stream collecting every
`struct Name {` pattern (regardless of scope, order, or validity elsewhere)
into a flat name set. A leading `(` is treated as a cast iff the token
immediately after is a base-type keyword (`u8`/`i8`/`u16`/`i16`/`void`) or
an identifier in that prescanned set.

> ⚠ [G-5](DEVIATIONS_c_impl.md#g-5-struct-name-shadowing-misparses-a-cast): a
> local variable that shadows a struct name breaks this — `(Point) - 1`
> misparses as a cast when `Point` is a local shadowing the struct, not a
> subtraction. Fails loudly downstream, not silently; accepted upstream
> as-is.

**Cast operand precedence:** once recognized as a cast, the operand is parsed
at **`unary`** precedence — the standard C cast-expression rule. A cast binds
tighter than any binary operator, so it reaches only as far as the next
unary/postfix term; it still stacks over prefix operators and nested casts
(`(u8)-w`, `(u8)(u16)x`):

```c
u16 w = 511;
u8 x = (u8)w / 2;    // parses as ((u8)w) / 2  →  0x7F
u16 r = (u16)a * b;  // casts a alone, then multiplies — the widened-multiply idiom holds
```

To cast a whole binary expression, parenthesize it explicitly: `(u8)(w / 2)`.

> ⚠ [P0-2](DEVIATIONS_c_impl.md#p0-2-cast-binds-to-the-whole-following-expression):
> `cc02` instead parses the cast operand at `logical_or` precedence (the top
> of the expression grammar), so a cast swallows the entire following
> expression — `(u8)w / 2` computes `(u8)(w / 2)` = `0xFF`, not `0x7F`, and
> `(u16)a * b` casts the *product*, silently defeating the widened-multiply
> idiom. A verified silent miscompile, and the single most common footgun for
> hand-written or generated test programs.

### 6.7 Literals

Decimal/hex/binary integers, double-quoted strings, `true`/`false`/`null`
(numeric aliases — §1.1). No array/list literals, no floating point, no
character literals.

---

## 7. Scoping & Name Resolution

### 7.1 Statement/Declaration Disambiguation

At both block-statement level and top level, an identifier-led line is
disambiguated by the **whole-file struct-name prescan** (§6.6) — the *same*
mechanism the cast-vs-grouping (§6.6) and `for`-init (§5.5) disambiguations
use. A leading identifier begins a type-led declaration
(`type name;` / `type name = expr;`) iff it names a base type
(`u8`/`i8`/`u16`/`i16`/`void`) **or** an identifier in the prescanned
struct-name set; the pointer stars and trailing name then parse as the rest
of the declaration. Otherwise the line falls through to an
expression/assignment statement.

```c
Point * p;    // struct Point declared anywhere in the file → declares p : Point*
foo * bar;    // foo is not a known struct → multiplication expression statement
```

The prescan is whole-file and scope-blind, so a struct-pointer declaration
resolves correctly even when the `struct` definition appears **later** in the
file than the use (forward reference). Disambiguation depends only on whether
the leading identifier is a known type name — never on declaration order or on
a symbol-table lookup at the use site.

Two consequences follow:

- An unknown type name in declaration position (`Widget * w;`, `Widget` never
  declared as a struct) parses as a **multiplication expression**, not a
  declaration. Its operands then fail to resolve at analysis
  (`ERR_UNDECLARED_IDENTIFIER`), rather than the declaration reaching analysis
  as `ERR_UNKNOWN_STRUCT`. Both are exit-5 analyzer errors on the same
  program; only the diagnostic identity differs. Unlike a purely shape-based
  rule, "multiply two identifiers as a statement, discarding the result" *is*
  writable here whenever the leading name isn't a known type.
- Because the prescan is scope-blind, a local variable that shadows a struct
  name still parses `Name * x;` as a declaration of `x : Name*`, not as a
  multiply of the shadowing local — the `*`-declaration twin of the cast
  misparse in
  [G-5](DEVIATIONS_c_impl.md#g-5-struct-name-shadowing-misparses-a-cast).

All three identifier-vs-type disambiguations in the grammar — block/top-level
statements (here), casts (§6.6), and `for`-init clauses (§5.5) — share this
one prescan mechanism, so the identical token shape resolves **identically**
regardless of position.

> ⚠ [G-4](DEVIATIONS_c_impl.md#g-4-block-statements-disambiguate-by-shape-not-the-prescan):
> `cc02` disambiguates block- and top-level statements by a purely shape-based
> rule (skip `*` tokens, check for a following identifier) rather than the
> prescan — so `foo * bar;` there **always** parses as a declaration of
> `bar : foo*`, even when `foo` is not a known type. `cc02` uses the prescan
> only in the `for`-init clause, so the identical shape resolves differently
> by position within `cc02` itself.

### 7.2 Scope Stack & Shadowing

Scopes: one global scope (functions, structs, `reg`s, globals, `decl`s all
share this single namespace), plus one scope per function body (covering
its parameters), one per `{ }` block, and one per `for` loop (covering its
init/cond/incr/body together, in addition to any further scope its body
block pushes). `if` and `while` do **not** push their own scope — only a
nested `{ }` block does.

Name lookup walks **innermost outward to global**, returning the first
match.

**Redeclaration** (same name, same scope frame): always `ERR_REDECLARATION`,
regardless of symbol kind — a global variable can't share a name with a
function, struct, or register, since they all share one symtab.

**Shadowing** (same name, visible in an *enclosing* — not current — scope):
for local **variables and function parameters only**, checking **every**
enclosing scope up to and including global, this is
`ERR_SHADOWED_DECLARATION` — deliberately disallowed, unlike C. Codegen
identifies storage by bare name, so a shadowed name would alias its outer
namesake's storage; this rule exists to prevent that, not merely for
style. Two **sibling** scopes (e.g. two separate `for (u8 i...)` loops) are
unaffected, since each is fully popped before the next is pushed.

> ⚠ [S-8](DEVIATIONS_c_impl.md#s-8-shadowing-check-is-asymmetric): this check
> applies only to variables and parameters — a struct declared inside a
> function body is only checked for same-scope redeclaration, never
> outer-scope shadowing.

### 7.3 Lvalues

The assignment target, and the operand of `&`, `++`, `--`, must be an
lvalue. Lvalue-ness is checked **structurally and shallowly**: the
*top-level node kind* must be one of `NODE_IDENTIFIER`, `NODE_FIELD_ACCESS`,
or `NODE_DEREF`. There is no recursion into whether the *base* of a
field-access/deref chain is itself addressable storage.

> ⚠ [S-5](DEVIATIONS_c_impl.md#s-5-lvalue-checking-is-shallow):
> `someFunctionCall().field = 5;` is accepted as a valid assignment target
> purely because the outermost node "looks like" an lvalue shape — this is
> exactly what lets `&p.x` reach codegen and crash the compiler
> ([P1-1](DEVIATIONS_c_impl.md#p1-1-address-of-a-struct-field-segfaults-the-compiler)).

### 7.4 `main`

Exactly one **function definition** (not a `decl` forward declaration —
§4.6) named exactly `main` is required in the linked program, with the
signature `fn main() -> void { ... }` — zero parameters, return type exactly
`void`.

Responsibility for this splits by compilation stage, because the toolchain
supports multi-file linking and incremental compilation:

- **Existence** — that some translation unit defines `main` — is a
  whole-program property, enforced by the linker/driver, **not** the per-TU
  analyzer. A single translation unit cannot know whether `main` is defined
  in another unit, so the analyzer never reports a missing `main`.
- **Signature** is checked by the analyzer for any `main` it *does* see
  defined: a non-`void` return type or a non-empty parameter list is
  `ERR_BAD_MAIN_SIGNATURE`. A `main` introduced only via `decl` is a forward
  declaration, not a definition, so it is not signature-checked here.

> ⚠ [S-9](DEVIATIONS_c_impl.md#s-9-mains-signature-is-unchecked): `cc02` only
> verifies that a symbol named `main` exists and is a function — any return
> type and any parameter list/count are accepted silently, and a `main`
> introduced solely via `decl` (with no defining body) satisfies the check
> just as well as a real definition.

### 7.5 Missing-Return Detection

Only applies to functions whose declared return type is not exactly
non-pointer `void`. The check is **purely syntactic and positional**: it
looks only at the function's **last top-level statement** and passes
(no diagnostic) if that statement's node kind is one of `NODE_RETURN`,
`NODE_IF`, `NODE_WHILE`, `NODE_FOR`, or `NODE_BLOCK` — regardless of
whether that statement is actually guaranteed to return on every path.

```c
fn f(u8 x) -> u8 {
  if (x) { return 1; }   // one-armed if, no else — NOT flagged, despite
}                          // falling through with no return when x == 0

fn g() -> u8 {
  while (cond) { ... }    // NOT flagged, whether or not this loop can
}                          // ever exit or ever returns

fn h() -> u8 {
  return 1;
  do_side_effect();        // dead code, but IS the last statement and
}                           // isn't one of the 5 "may-return" kinds —
                            // flagged (ERR_MISSING_RETURN), even though
                            // the function does return, just not last.
```

There is no control-flow/path-coverage analysis — do not rely on the
absence of `ERR_MISSING_RETURN` as proof every path returns. See
[S-12](DEVIATIONS_c_impl.md#s-12-missing-return-detection-is-shallow).

---

## 8. Diagnostics Catalog

### 8.1 Errors

All increment the analyzer's error count and do **not** stop analysis (the
analyzer always walks the whole program and reports everything it can,
unlike the parser — §8.3).

| Error | Fires when |
|---|---|
| `ERR_UNDECLARED_IDENTIFIER` | Identifier (value use or call target) not found in any visible scope. |
| `ERR_NOT_A_FUNCTION` | Call target resolves to a non-function symbol. |
| `ERR_UNKNOWN_STRUCT` | A struct-typed name doesn't resolve to a registered struct (declared type, cast target, struct-init target). |
| `ERR_REDECLARATION` | Same name inserted twice into one scope frame (§7.2). |
| `ERR_SHADOWED_DECLARATION` | A local var/param reuses a name visible in an enclosing scope (§7.2). |
| `ERR_TYPE_MISMATCH` | Generic incompatibility: initializer, assignment, return, call argument, binary operand, dereference-of-non-pointer, struct-init field value. |
| `ERR_WRONG_ARG_COUNT` | Call argument count ≠ declared parameter count. |
| `ERR_UNKNOWN_FIELD` | Named field doesn't exist on the target struct (`.field` access or struct-init). |
| `ERR_NOT_ASSIGNABLE` | A function or struct name used where a value was expected. |
| `ERR_BAD_MAIN_SIGNATURE` | A defined `main` isn't `void main()` — non-`void` return type or a non-empty parameter list (§7.4). Existence of `main` is a link-time check, not the analyzer's. |
| `ERR_LITERAL_OUT_OF_RANGE` | Integer literal outside `-32768..65535` (or its type-specific band). |
| `ERR_NOT_LVALUE` | `&`, `++`, `--`, or assignment LHS on a non-lvalue-shaped node (§7.3). |
| `ERR_VOID_VARIABLE` | Non-pointer `void` used as a variable/param/field/global type. |
| `ERR_NOT_A_STRUCT` | `.field` on something that (after one-level auto-deref) still isn't a bare struct. |
| `ERR_MISSING_RETURN` | Shallow last-statement check fails on a non-void function (§7.5). |
| `ERR_INCOMPLETE_STRUCT_FIELD` | By-value struct field is self-referential, or names a struct not declared earlier in the file (§4.4). |
| `ERR_BREAK_OUTSIDE_LOOP` / `ERR_CONTINUE_OUTSIDE_LOOP` | `break`/`continue` with loop-depth 0. |
| `ERR_STRUCT_CAST_BY_VALUE` | `(StructName)expr` cast with no pointer level. |
| `ERR_WRONG_ARG_TYPE` | Defined in the enum, but **never actually emitted** — `ERR_TYPE_MISMATCH` (context `"function call"`) is used instead. Treat as dead/reserved. ([S-16](DEVIATIONS_c_impl.md#s-16-err_wrong_arg_type-is-dead-code)) |

### 8.2 Warnings

| Warning | Fires when | Notes |
|---|---|---|
| `WARN_INVALID_INTERRUPT` | `interrupt`-qualified function fails name/return-type/param-count checks (§4.2). | The only warning that actually prints. |
| `WARN_UNUSED_VARIABLE` / `_FUNCTION` / `_STRUCT` / `_FIELD` | Never — defined in the enum with print-dispatch plumbing, but no code path ever constructs one (`// unimplemented`). | Do not rely on these appearing; no unused-anything detection exists today. ([S-11](DEVIATIONS_c_impl.md#s-11-unused-variable-diagnostics-are-unimplemented)) |

### 8.3 Parser Errors

The parser reports **only its first error** and stops (syntax errors leave
the token stream ambiguous, so continuing isn't attempted) — unlike the
analyzer. Shared error kinds: `ERR_UNEXPECTED_EOF`, `ERR_UNEXPECTED_TOKEN`,
`ERR_ALLOCATION_FAILED`.

### 8.4 Stage Exit Codes

`lexer=3, parser=4, analyzer=5, IR=6, codegen=7` (a program is expected to
fail at the exact stage its first error belongs to).

---

## 9. Known Deviations From This Spec

Every `⚠` marker throughout this document flags a specific point where
`cc02` at commit `9a9375e` does not actually implement the rule just
stated. Full write-ups, verified reproductions, and a quick-reference index
— grouped by category: **G** lexer/parser quirks, **P** codegen
silent-miscompiles, **S** analyzer type-system laxity — live in the
companion document, **[`DEVIATIONS_c_impl.md`](DEVIATIONS_c_impl.md)**.

That document is also the fuzz harness's known-issues oracle: the
P-numbered items are encoded as live regression tests in the upstream
repo's `cc02/tests/bug_test.py` (`python3 cc02/tests/bug_test.py` from the
`C02` checkout — 23 of 24 currently red). A fuzz run reproducing one of
those is confirming a known issue, not discovering a new one — only a
*behavior change* from what's recorded there is noteworthy.

Each entry in `DEVIATIONS_c_impl.md` is marked **Executed** (independently
reproduced, either while authoring these documents or via
`bug_test.py`/`docs/BUG_REPORT.md` upstream) or **Source** (derived from
reading `cc02` source, not independently executed) — treat **Source**
entries as high-confidence but unconfirmed, and re-verify before relying on
one as a pass/fail oracle.

---

## Appendix A: Idiomatic Patterns (Confirmed Working)

These are not language features so much as the standard workarounds for
missing features (primarily: no arrays), collected from the upstream
`examples/*.c02` programs and the emulator test suite.

```c
// "array" access via pointer arithmetic — the canonical substitute
for (u8 i = 0; *(msg + i); ++i) {
  // use *(msg + i)
}

// string walk
for (u8 *p = msg; *p != null; ++p) {
  // use *p
}

// idiomatic top-level program shape for a hardware target
reg u8 PORTB @ 0x6000;
fn main() -> void {
  while (true) {
    // main loop body
  }
}
```

## Appendix B: Confirmed Runtime Semantics

Behaviors demonstrated by the upstream emulator test suite
(`cc02/tests/emu_*.c02` via `emu_test.py`) that aren't stated anywhere in
prose in the upstream README — useful as oracle values for a fuzzer
generating arithmetic/conversion test cases (assuming none of the §9
deviations are in play for the specific expression shape used):

- Implicit widen (`u8`→`u16`) zero-extends; a binary op between a `u8` and
  a `u16` widens the `u8` operand before computing.
- Signed division/modulo truncate toward zero with the mathematically
  correct sign (`-6 / 2 == -3`, `-7 % 2 == -1`).
- Signed right shift is arithmetic (sign-extending), not logical.
- Unsigned `u16` multiplication overflow wraps mod 65536 rather than
  erroring.
- `if (x)` / bare-value truthiness works correctly at both `u8` and `u16`
  width (nonzero test).
- A callee does not clobber caller locals it has no pointer/name access to
  (ordinary, non-pointer-aliased calls are correct — this is distinct from
  the P0-7 pointer-aliased case above).
