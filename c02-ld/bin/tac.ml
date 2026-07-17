(* tac.ml — TAC / IR data model and wire-format numbering.
 *
 * Port of the frontend's C02.Lowering.TAC (Haskell), which is the *write* side
 * of the same object-file contract this linker reads. The enum numbers below
 * are the contract: keep them in lockstep with c02-as/src/ir.h and the Haskell
 * tag functions. Serialization (the cursor + read_* logic) lives elsewhere, not
 * here — this module is just the shapes and the numbering.
 *)

let c02_magic  = 0x43303249   (* "C02I" *)
let ir_version = 3

(* --------------------------------------------------------------------------
 * Types, in dependency order — OCaml requires each type defined before use
 * (unlike Haskell). Only `block` is self-recursive, which is legal on its own;
 * nothing here needs `and`-chaining.
 * ------------------------------------------------------------------------ *)

(* Haskell: data BaseType = U8|I8|U16|I16|Void | StructName String.
 * The struct name rides inside the constructor, so `ty` needs no separate
 * struct_name field. (Wire TYPE_INVALID never appears in a well-formed .o.) *)
type base_type =
  | U8 | I8 | U16 | I16 | Void
  | StructName of string

(* Haskell: type Ty = (BaseType, Int)  -- (base type, pointer depth) *)
type ty = base_type * int

(* Haskell: type NamedType = (BaseType, Int, String)  -- (base, depth, name) *)
type named_type = base_type * int * string

type tac_op =
  | TacAdd       | TacSub    | TacMul      | TacDiv  | TacMod
  | TacLt        | TacGt     | TacLte      | TacGte  | TacEq  | TacNeq
  | TacAnd       | TacOr
  | TacShl       | TacShr    | TacBand     | TacBxor | TacBor
  | TacNeg       | TacNot    | TacBnot
  | TacInc       | TacDec
  | TacAddrOf    | TacLoad   | TacStore
  | TacCopy
  | TacLabel     | TacJump   | TacCondJump
  | TacCall      | TacReturn | TacContinue | TacBreak
  | TacFieldLoad | TacFieldStore
  | TacCast

type ir_init_kind = IRInitNone | IRInitInt | IRInitStr

(* Every operand carries a `ty` first, then its kind-specific payload. *)
type operand =
  | OperandNone     of ty
  | OperandTemp     of ty * int
  | OperandVar      of ty * string
  | OperandConstInt of ty * int
  | OperandConstStr of ty * string

(* Fat, fixed-shape instruction: every field is present on the wire for every
 * opcode, so the reader consumes them all regardless of `op`. *)
type instr = {
  op         : tac_op;
  dst        : operand;
  src1       : operand;
  src2       : operand;
  label      : int;
  call_name  : string;
  call_args  : operand list;
  field_name : string;
  cast_type  : ty;
}

(* `successors` is NOT on the wire — rebuild it from terminators after load.
 * Compare blocks by `block_id`; structural `=` loops on cyclic back-edges. *)
type block = {
  block_id     : int;
  instructions : instr list;
  successors   : block list;
}

type cfg = {
  fn_name      : string;
  ret_type     : ty;
  params       : named_type list;
  blocks       : block list;
  next_temp    : int;
  next_label   : int;
  is_interrupt : bool;
}

type global = {
  glob_name      : string;
  glob_type      : ty;
  glob_init_kind : ir_init_kind;
  glob_int_val   : int;
  glob_str_val   : string;
}

type ir_field = {
  ir_field_name   : string;
  ir_field_type   : ty;
  ir_field_offset : int;
}

type ir_struct = {
  ir_struct_name   : string;
  ir_struct_fields : ir_field list;
  ir_struct_size   : int;
}

type reg = {
  reg_name : string;
  reg_type : ty;
  reg_addr : int;
}

type extern = {
  extern_is_func : bool;
  extern_name    : string;
  extern_type    : ty;
  extern_params  : named_type list;
}

(* `module` is a reserved keyword in OCaml, so the top-level type is `ir_module`. *)
type ir_module = {
  externs : extern list;
  structs : ir_struct list;
  globals : global list;
  regs    : reg list;
  cfgs    : cfg list;
}

(* --------------------------------------------------------------------------
 * Wire numbering — mirrors c02-as/src/ir.h and TAC.hs. These ints are the
 * contract with the frontend's serializer.
 * ------------------------------------------------------------------------ *)

(* Every operand variant carries a ty first; pull it out regardless of kind. *)
let operand_type = function
  | OperandNone t          | OperandTemp (t, _)          | OperandVar (t, _)
  | OperandConstInt (t, _) | OperandConstStr (t, _) -> t

(* operand -> wire OPERAND_* kind. Inverse of read_operand's `match kind`. *)
let operand_tag = function
  | OperandNone _     -> 0
  | OperandTemp _     -> 1
  | OperandVar _      -> 2
  | OperandConstInt _ -> 3
  | OperandConstStr _ -> 4

(* base_type -> wire TYPE_* kind. The inverse lives in the reader's read_type,
 * because kind 5 (STRUCT) must also pull a length-prefixed name off the cursor
 * to fill in `StructName`. *)
let type_kind_tag = function
  | U8 -> 0 | I8 -> 1 | U16 -> 2 | I16 -> 3 | Void -> 4
  | StructName _ -> 5

(* tac_op wire numbering: the index into this array IS the tag. Both directions
 * are derived from it, so they cannot drift apart — a hand-written inverse would
 * be a second list to keep in lockstep, and a mismatch there is a silent binary
 * desync. Line grouping mirrors the type declaration above; keep it that way so
 * the two stay diffable against ir.h's enum. *)
let tac_ops = [|
  TacAdd;       TacSub;    TacMul;      TacDiv;   TacMod;
  TacLt;        TacGt;     TacLte;      TacGte;   TacEq;   TacNeq;
  TacAnd;       TacOr;
  TacShl;       TacShr;    TacBand;     TacBxor;  TacBor;
  TacNeg;       TacNot;    TacBnot;
  TacInc;       TacDec;
  TacAddrOf;    TacLoad;   TacStore;
  TacCopy;
  TacLabel;     TacJump;   TacCondJump;
  TacCall;      TacReturn; TacContinue; TacBreak;
  TacFieldLoad; TacFieldStore;
  TacCast;
|]

let tac_op_of_int n =
  if n < 0 || n >= Array.length tac_ops then failwith (Printf.sprintf "bad tac_op %d" n)
  else tac_ops.(n)

(* Inverse by construction. A constructor added to tac_op but missing from
 * tac_ops fails loudly here instead of emitting a wrong tag. *)
let tac_op_tag op =
  let rec go i =
    if i >= Array.length tac_ops then failwith "tac_op missing from tac_ops"
    else if tac_ops.(i) = op then i
    else go (i + 1)
  in
  go 0

let ir_init_kind_of_int = function
  | 0 -> IRInitNone | 1 -> IRInitInt | 2 -> IRInitStr
  | n -> failwith (Printf.sprintf "bad ir_init_kind %d" n)

let ir_init_kind_tag = function
  | IRInitNone -> 0 | IRInitInt -> 1 | IRInitStr -> 2
