(* c02-ld — entry point and IR object-file reader.
 * The data model and wire numbering live in tac.ml (module Tac). *)

open Serializer
open Tac
open Link

let read_whole_file filename =
  In_channel.with_open_bin filename In_channel.input_all

let parse_file filename =
  let content = read_whole_file filename in
  let cursor = { buf = Bytes.of_string content; pos = 0 } in
  read_module cursor

let () =
  (* argv.(0) is the program name; the rest are the files to read *)
  let files = Array.to_list Sys.argv |> List.tl in
  let modules = List.map (fun f -> parse_file f) files in

  let linked = link modules in
  if not (List.exists (fun f -> f.fn_name = "main") linked.cfgs) then failwith "no main function in linked program";

  Printf.printf "structs=%d globals=%d regs=%d cfgs=%d externs=%d\n"
    (List.length linked.structs) (List.length linked.globals)
    (List.length linked.regs) (List.length linked.cfgs) (List.length linked.externs)
