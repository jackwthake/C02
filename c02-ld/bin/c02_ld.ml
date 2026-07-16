(* c02-ld — entry point and IR object-file reader.
 * The data model and wire numbering live in tac.ml (module Tac). *)

open Serializer
open Tac

let read_whole_file filename =
  In_channel.with_open_bin filename In_channel.input_all

let parse_file filename =
  let content = read_whole_file filename in
  let cursor = { buf = Bytes.of_string content; pos = 0 } in
  read_module cursor

let () =
  (* argv.(0) is the program name; the rest are the files to read *)
  let files = Array.to_list Sys.argv |> List.tl in
  List.iter (fun f ->
    let m = parse_file f in
    Printf.printf "%s: structs=%d globals=%d regs=%d cfgs=%d externs=%d\n"
      f (List.length m.structs) (List.length m.globals)
      (List.length m.regs) (List.length m.cfgs) (List.length m.externs)
  ) files
