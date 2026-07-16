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

(* base_type -> wire TYPE_* kind. The inverse lives in the reader's read_type,
 * because kind 5 (STRUCT) must also pull a length-prefixed name off the cursor
 * to fill in `StructName`. *)
let type_kind_tag = function
  | U8 -> 0 | I8 -> 1 | U16 -> 2 | I16 -> 3 | Void -> 4
  | StructName _ -> 5

let tac_op_of_int = function
  | 0 -> TacAdd        | 1  -> TacSub    | 2  -> TacMul      | 3  -> TacDiv   | 4  -> TacMod
  | 5 -> TacLt         | 6  -> TacGt     | 7  -> TacLte      | 8  -> TacGte   | 9  -> TacEq   | 10 -> TacNeq
  | 11 -> TacAnd       | 12 -> TacOr
  | 13 -> TacShl       | 14 -> TacShr    | 15 -> TacBand     | 16 -> TacBxor  | 17 -> TacBor
  | 18 -> TacNeg       | 19 -> TacNot    | 20 -> TacBnot
  | 21 -> TacInc       | 22 -> TacDec
  | 23 -> TacAddrOf    | 24 -> TacLoad   | 25 -> TacStore
  | 26 -> TacCopy
  | 27 -> TacLabel     | 28 -> TacJump   | 29 -> TacCondJump
  | 30 -> TacCall      | 31 -> TacReturn | 32 -> TacContinue | 33 -> TacBreak
  | 34 -> TacFieldLoad | 35 -> TacFieldStore
  | 36 -> TacCast
  | n -> failwith (Printf.sprintf "bad tac_op %d" n)

let ir_init_kind_of_int = function
  | 0 -> IRInitNone | 1 -> IRInitInt | 2 -> IRInitStr
  | n -> failwith (Printf.sprintf "bad ir_init_kind %d" n)
