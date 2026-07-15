(* c02-ld — entry point and IR object-file reader.
 * The data model and wire numbering live in tac.ml (module Tac). *)

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

let () = print_endline "c02-ld"
