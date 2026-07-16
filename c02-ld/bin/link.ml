open Serializer
open Tac

type symbol = Reg of reg | Global of global | Func of cfg | Extern of extern
type symbol_table = (string, symbol) Hashtbl.t 

let make_symtab x : symbol_table = Hashtbl.create x

let merge_two_module module_a module_b =
  let externs = List.append module_a.externs module_b.externs in
  let structs = List.append module_a.structs module_b.structs in
  let globals = List.append module_a.globals module_b.globals in
  let regs    = List.append module_a.regs module_b.regs in
  let cfgs    = List.append module_a.cfgs module_b.cfgs in

  { externs; structs; globals; regs; cfgs }

let rec merge_modules modules : ir_module =
  match modules with
  | []             -> { externs=[]; structs=[]; globals=[]; regs=[]; cfgs=[] }
  | [x]            -> x
  | x :: y :: tail -> merge_modules (List.append [(merge_two_module x y)] tail)

let populate_symbol_table m : symbol_table = 
  let len = (List.length m.externs) + (List.length m.structs) + (List.length m.globals)
          + (List.length m.regs) + (List.length m.cfgs) in
  let symtab = make_symtab len in

  List.iter (fun e -> Hashtbl.add symtab e.extern_name (Extern e)) m.externs;
  List.iter (fun g -> Hashtbl.add symtab g.glob_name (Global g)) m.globals;
  List.iter (fun r -> Hashtbl.add symtab r.reg_name (Reg r)) m.regs;
  List.iter (fun f -> Hashtbl.add symtab f.fn_name (Func f)) m.cfgs;
  symtab

let combine a b =
  match a, b with
  | Extern e1, Extern e2          -> if e1 = e2 then a else failwith "Conflicting externs"
  (* resolution: sig-check e against def -> keep def *)
  | Extern e, def | def, Extern e -> if e.extern_is_func then
                                       (match def with
                                       | Func f ->
                                            let param_types ps = List.map (fun (bt, d, _) -> (bt, d)) ps in
                                            if e.extern_type = f.ret_type
                                              && param_types e.extern_params = param_types f.params
                                            then def
                                            else failwith ("signature mismatch for " ^ e.extern_name)
                                       | _      -> failwith "declared as function, defined as variable")
                                     else
                                       (match def with
                                       | Global g -> if e.extern_type = g.glob_type then def
                                                     else failwith ("type mismatch for " ^ e.extern_name)
                                       | _        -> failwith "declared as variable but defined as a register")
  (* same type AND addr? -> keep one; else conflict  *)
  | Reg r1, Reg r2            -> if r1 = r2 then a else failwith ("Conflicting registers: " ^ r1.reg_name ^ " and " ^ r2.reg_name)
  (* same type, <=1 initializer? -> merge; else err  *)
  | Global g1, Global g2      -> if g1.glob_type = g2.glob_type then 
                                   begin match g1.glob_init_kind, g2.glob_init_kind with
                                     | IRInitNone, IRInitNone -> a
                                     | IRInitNone, _ -> b
                                     | _, IRInitNone -> a
                                     | _, _ -> failwith ("global " ^ g1.glob_name ^ " has multiple initializers")
                                   end
                                 else failwith "Conflicting global types"
  (* duplicate function definition -> conflict       *)
  | Func a, Func _            -> failwith ("Redefined function: " ^ a.fn_name)
  (* cross-kind same name -> namespace collision     *)
  | _, _                      -> failwith "Redefined symbol"

module Names = Set.Make (String)

(* Reconcile the populated table: for each DISTINCT name, fold that name's
   bindings through `combine` down to the single surviving symbol (or fail on a
   real conflict). Resolved externs disappear here — `combine` keeps the
   definition — so only genuinely-unresolved externs survive, for codegen. *)
let reconcile (symtab : symbol_table) : symbol list =
  let names = Hashtbl.fold (fun name _ acc -> Names.add name acc) symtab Names.empty in
  Names.fold (fun name acc ->
    match Hashtbl.find_all symtab name with
    | []            -> acc                    (* unreachable: name came from the table *)
    | first :: rest -> List.fold_left combine first rest :: acc
  ) names []

(* Partition the reconciled symbols back into an ir_module. Structs aren't part
   of the symbol namespace, so they ride through separately. *)
let symbols_to_module (symbols : symbol list) (structs : ir_struct list) : ir_module =
  let regs    = List.filter_map (function Reg r    -> Some r | _ -> None) symbols in
  let globals = List.filter_map (function Global g -> Some g | _ -> None) symbols in
  let cfgs    = List.filter_map (function Func f   -> Some f | _ -> None) symbols in
  let externs = List.filter_map (function Extern e -> Some e | _ -> None) symbols in
  { regs; globals; cfgs; externs; structs }

(* Whole-program link: gather every module, resolve the symbol namespace, and
   rebuild one merged module. *)
let link (modules : ir_module list) : ir_module =
  let merged  = merge_modules modules in
  let symtab  = populate_symbol_table merged in
  let symbols = reconcile symtab in
  symbols_to_module symbols merged.structs