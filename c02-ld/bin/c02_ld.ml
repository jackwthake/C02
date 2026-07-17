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
  (* argv.(0) is the program name; the rest are the args *)
  let args = Array.to_list Sys.argv |> List.tl in

  (* parse args: expect -o <outfile> plus one or more input files *)
  let rec walk acc_files out = function
    | [] -> (List.rev acc_files, out)
    | "-o" :: file :: rest -> walk acc_files (Some file) rest
    | flag :: _ when flag = "-o" -> failwith "-o requires a filename"
    | f :: rest -> walk (f :: acc_files) out rest
  in
  let (files, outfile) = walk [] None args in
  let outfile = match outfile with Some f -> f | None -> failwith "missing -o output file" in

  let modules = List.map (fun f -> parse_file f) files in

  (* The linker always emits a relocatable object; it never requires a main.
     Entry-point existence (§7.4) is enforced by the generator, the only stage
     that turns an object into an executable — a linked object may legitimately
     be a library. Conflict rejection, struct dedup, and extern pass-through
     still happen in `link`. *)
  let linked = link modules in
  write_module linked outfile
