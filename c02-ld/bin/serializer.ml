open Tac

(* A mutable cursor over a slurped file: advance `pos` on every read.
 * RULE: every cursor read gets its own `let ... in`. Never put two cursor
 * reads in the same tuple/record/argument list — OCaml leaves evaluation
 * order unspecified there, which silently reads the fields out of order. *)
type cursor = { buf : bytes; mutable pos : int }

let u32 c =                                   (* mirrors read_u32 *)
  let v = Int32.to_int (Bytes.get_int32_le c.buf c.pos) in
  c.pos <- c.pos + 4;
  v

let i64 c =                                   (* mirrors read_i64; stays int64 *)
  let v = Bytes.get_int64_le c.buf c.pos in
  c.pos <- c.pos + 8;
  v

let str c =                                   (* mirrors read_str; len 0 -> None *)
  let len = u32 c in
  if len = 0 then None
  else
    let s = Bytes.sub_string c.buf c.pos len in
    c.pos <- c.pos + len;
    Some s

(* Wire type_t is (kind, is_ptr, ptr_depth, [name if STRUCT]); we collapse it to
 * Tac.ty = (base_type, depth). is_ptr is redundant (it's just depth > 0), so we
 * read past it. Kind 5 (STRUCT) additionally pulls the length-prefixed name. *)
let read_type c : ty =
  let kind = u32 c in
  let _is_ptr = u32 c in
  let depth = u32 c in
  let base =
    match kind with
    | 0 -> U8 | 1 -> I8 | 2 -> U16 | 3 -> I16 | 4 -> Void
    | 5 ->
      (match str c with
       | Some name -> StructName name
       | None -> failwith "struct type with empty name")
    | n -> failwith (Printf.sprintf "bad type kind %d" n)
  in
  (base, depth)

let read_named_type c : named_type =
  let (base, depth) = read_type c in
  let name = Option.value (str c) ~default:"" in
  (base, depth, name)

let read_operand c : operand = 
  let kind = u32 c in
  let op_type = read_type c in
  match kind with
  | 0 -> OperandNone (op_type)
  | 1 -> OperandTemp (op_type, u32 c)
  | 2 -> OperandVar (op_type, match str c with 
                              | Some s -> s
                              | None -> failwith "Missing VAR name")
  | 3 -> OperandConstInt (op_type, Int64.to_int(i64 c))
  | 4 -> OperandConstStr (op_type, Option.value (str c) ~default:"")
  | n -> failwith (Printf.sprintf "bad operand kind %d" n)

let read_instr c : instr = 
  let op              = tac_op_of_int (u32 c) in 
  let dst             = read_operand c in 
  let src1            = read_operand c in 
  let src2            = read_operand c in
  let label           = u32 c in
  let call_name       = Option.value (str c) ~default:"" in
  let call_args_count = u32 c in

  let call_args = List.init call_args_count (fun _ -> read_operand c) in

  let field_name = Option.value (str c) ~default:"" in
  let cast_type = read_type c in

  { op; dst; src1; src2; label; field_name; cast_type; call_name; call_args }

let read_block c : block = 
  let block_id = u32 c in
  let instruction_count = u32 c in
  let instructions = List.init instruction_count (fun _ -> read_instr c) in

  { block_id; instructions; successors=[] } (* successors is not serialized *)

let read_cfg c : cfg =
  let fn_name = match str c with 
                | Some s -> s
                | None -> failwith "Missing FUNCTION name" in
  let ret_type = read_type c in
  
  let param_count = u32 c in
  let params = List.init param_count (fun _ -> read_named_type c) in

  let block_count = u32 c in
  let blocks = List.init block_count (fun _ -> read_block c) in

  let next_temp = u32 c in
  let next_label = u32 c in
  let is_interrupt = match u32 c with 0 -> false | n -> true in 

  { fn_name; ret_type; params; blocks; next_temp; next_label; is_interrupt }

(* Struct field: name, type, then byte offset within the struct. *)
let read_ir_field c : ir_field =
  let ir_field_name   = Option.value (str c) ~default:"" in
  let ir_field_type   = read_type c in
  let ir_field_offset = u32 c in
  { ir_field_name; ir_field_type; ir_field_offset }

(* Struct: name, field_count, total_size, THEN the fields (size precedes them
 * on the wire). *)
let read_ir_struct c : ir_struct =
  let ir_struct_name   = Option.value (str c) ~default:"" in
  let field_count      = u32 c in
  let ir_struct_size   = u32 c in
  let ir_struct_fields = List.init field_count (fun _ -> read_ir_field c) in
  { ir_struct_name; ir_struct_fields; ir_struct_size }

(* Global: name, type, init_kind, then a kind-dependent payload (an i64, a
 * string, or nothing). We store the raw value; the unused half defaults. *)
let read_global c : global =
  let glob_name      = Option.value (str c) ~default:"" in
  let glob_type      = read_type c in
  let glob_init_kind = ir_init_kind_of_int (u32 c) in
  let (glob_int_val, glob_str_val) =
    match glob_init_kind with
    | IRInitInt  -> (Int64.to_int (i64 c), "")
    | IRInitStr  -> (0, Option.value (str c) ~default:"")
    | IRInitNone -> (0, "")
  in
  { glob_name; glob_type; glob_init_kind; glob_int_val; glob_str_val }

(* Register: name, type, then a u64 hardware address. The wire address is 8
 * bytes; a 6502 address fits an int, so we narrow. *)
let read_reg c : reg =
  let reg_name = Option.value (str c) ~default:"" in
  let reg_type = read_type c in
  let reg_addr = Int64.to_int (i64 c) in
  { reg_name; reg_type; reg_addr }

(* Extern (forward decl): is_function flag, name, type, then param_count params.
 * `params` is only meaningful when is_function, but always present on the wire. *)
let read_extern c : extern =
  let extern_is_func = u32 c <> 0 in
  let extern_name    = Option.value (str c) ~default:"" in
  let extern_type    = read_type c in
  let param_count    = u32 c in
  let extern_params  = List.init param_count (fun _ -> read_named_type c) in
  { extern_is_func; extern_name; extern_type; extern_params }

let read_module c : ir_module =
  let magic = u32 c in
  if magic <> c02_magic then failwith "Invalid object file";
  
  let version = u32 c in
  if version <> ir_version then 
    failwith (Printf.sprintf "Bad IR format expected version v%d got v%d" ir_version version);

  let struct_count = u32 c in
  let structs = List.init struct_count (fun _ -> read_ir_struct c) in

  let global_count = u32 c in
  let globals = List.init global_count (fun _ -> read_global c) in

  let reg_count = u32 c in
  let regs = List.init reg_count (fun _ -> read_reg c) in

  let cfg_count = u32 c in
  let cfgs = List.init cfg_count (fun _ -> read_cfg c) in

  let extern_count = u32 c in
  let externs = List.init extern_count (fun _ -> read_extern c) in
  
  { externs; structs; globals; regs; cfgs }
