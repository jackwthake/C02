use crate::parser::{TopLevel, Expr, Stmt, Op, Type};
use crate::analyzer::{SymbolTable, Symbol};

use serde::Deserialize;

use std::fs::File;
use std::io::{self, Write};
use std::process;
use std::path::Path;
use std::collections::HashMap;

#[derive(Debug, Deserialize, Clone, Copy)]
pub struct Memory_Map {
  pub rom_start: u16,
  pub rom_top: u16
}

// ZP layout
static FP: u8         = 0x00;  // 2 bytes: FP lo, FP hi
static RET: u8        = 0x02;  // 2 bytes: return value
static REG_START: u8  = 0x04;  // scratch registers r0-r13, 2 bytes each -> $04..$1D
static REG_TOP: u8    = 0x1D;
static ARGS_START: u8 = 0x1E;  // function arg passing zone, 2 bytes each -> $1E..$2F
// static ARGS_TOP: u8   = 0x2F;
static GLOBALS_START: u8 = 0x30; // global variables, 2 bytes each -> $30..$FF
static GLOBALS_TOP: u8   = 0xFF;

// How a named variable is stored — used by resolve_identifier
#[derive(Clone, Debug)]
enum VarLocation {
  ZeroPage(u8),   // LDA zp / STA zp  (params, locals, globals all land here)
  Absolute(u16),  // LDA abs / STA abs (reg-mapped hardware registers)
}

struct Generator {
  output: Vec<u8>,
  labels: HashMap<String, usize>,
  patches: Vec<(usize, String)>,

  global_symbols: SymbolTable,
  global_slots: HashMap<String, u8>,   // global var name -> ZP address ($30+)
  next_global: u8,                     // next free ZP address in globals zone

  param_slots: HashMap<String, u8>,    // param name -> ZP address in args zone
  local_slots: HashMap<String, u8>,    // local var name -> ZP address in scratch zone
  local_reg_top: u8,                   // watermark: next free scratch reg index (0-based)
                                       // locals own [0, local_reg_top), expr scratch uses above

  reg_depth: u8,                       // current expression scratch depth above local_reg_top
}

impl Generator {
  fn new(symbol_table: SymbolTable) -> Self {
    Generator {
      output: Vec::new(),
      labels: HashMap::new(),
      patches: Vec::new(),
      global_symbols: symbol_table,
      global_slots: HashMap::new(),
      next_global: GLOBALS_START,
      param_slots: HashMap::new(),
      local_slots: HashMap::new(),
      local_reg_top: 0,
      reg_depth: 0,
    }
  }

  ////////////////////////////////////////////////////////
  /// Slot allocation
  ////////////////////////////////////////////////////////

  // Allocate a 16-bit scratch register slot above the local watermark.
  // Returns the ZP address of the lo byte.
  fn alloc_scratch_reg(&mut self) -> u8 {
    let idx = self.local_reg_top + self.reg_depth;
    self.reg_depth += 1;
    assert!((REG_START + idx * 2) <= REG_TOP, "Scratch register overflow!");

    REG_START + idx * 2
  }

  fn free_scratch_reg(&mut self) {
    assert!(self.reg_depth > 0, "free_scratch_reg: underflow — freed more scratch regs than were allocated");
    self.reg_depth -= 1;
  }

  // Allocate a permanent local variable slot (below the scratch watermark).
  // Returns ZP address of lo byte.
  fn alloc_local(&mut self, name: String) -> u8 {
    let addr = REG_START + self.local_reg_top * 2;

    assert!(
      !self.local_slots.contains_key(&name),
      "alloc_local: duplicate local variable '{}'", name
    );
    assert!(
      addr + 1 <= REG_TOP,
      "alloc_local: local variable '{}' would overflow scratch register zone (addr=${:02X}, REG_TOP=${:02X})",
      name, addr, REG_TOP
    );

    self.local_reg_top += 1;
    self.local_slots.insert(name, addr);
    addr
  }

  // Resolve a name to where it lives in memory.
  fn resolve_identifier(&self, name: &str) -> Option<VarLocation> {
    // 1. param (in args zone)
    if let Some(&addr) = self.param_slots.get(name) {
      return Some(VarLocation::ZeroPage(addr));
    }
    // 2. local (in scratch register zone)
    if let Some(&addr) = self.local_slots.get(name) {
      return Some(VarLocation::ZeroPage(addr));
    }
    // 3. global var (in globals zone)
    if let Some(&addr) = self.global_slots.get(name) {
      return Some(VarLocation::ZeroPage(addr));
    }
    // 4. reg-mapped hardware register (absolute address)
    if let Some(Symbol::Register { address, .. }) = self.global_symbols.local_symbols.get(name) {
      return Some(VarLocation::Absolute(*address));
    }
    None
  }

  ////////////////////////////////////////////////////////
  /// OP Code emitters
  ////////////////////////////////////////////////////////

  fn emit_lda_imm(&mut self, val: u8) { self.output.push(0xA9); self.output.push(val); }
  fn emit_lda_zpg(&mut self, addr: u8) { self.output.push(0xA5); self.output.push(addr); }
  fn emit_lda_abs(&mut self, addr: u16) {
    self.output.push(0xAD);
    self.output.push((addr & 0xFF) as u8);
    self.output.push((addr >> 8) as u8);
  }
  fn emit_lda_indirect_zpg(&mut self, addr: u8) { self.output.push(0xB1); self.output.push(addr); }

  fn emit_sta_zpg(&mut self, addr: u8) { self.output.push(0x85); self.output.push(addr); }
  fn emit_sta_abs(&mut self, addr: u16) {
    self.output.push(0x8D);
    self.output.push((addr & 0xFF) as u8);
    self.output.push((addr >> 8) as u8);
  }

  fn emit_ldx_imm(&mut self, val: u8) { self.output.push(0xA2); self.output.push(val); }
  fn emit_ldx_zpg(&mut self, addr: u8) { self.output.push(0xA6); self.output.push(addr); }
  fn emit_ldy_imm(&mut self, val: u8) { self.output.push(0xA0); self.output.push(val); }

  fn emit_adc_zpg(&mut self, addr: u8) { self.output.push(0x65); self.output.push(addr); }
  fn emit_sbc_zpg(&mut self, addr: u8) { self.output.push(0xE5); self.output.push(addr); }
  fn emit_clc(&mut self) { self.output.push(0x18); }
  fn emit_sec(&mut self) { self.output.push(0x38); }

  fn emit_jsr(&mut self, label: &str) {
    self.output.push(0x20);
    let patch_offset = self.output.len();
    self.output.push(0x00);
    self.output.push(0x00);
    self.patches.push((patch_offset, label.to_string()));
  }

  fn emit_jmp_abs(&mut self, label: &str) {
    self.output.push(0x4C);
    let patch_offset = self.output.len();
    self.output.push(0x00);
    self.output.push(0x00);
    self.patches.push((patch_offset, label.to_string()));
  }

  fn emit_rts(&mut self) { self.output.push(0x60); }
  fn emit_txs(&mut self) { self.output.push(0x9A); }

  fn emit_label(&mut self, name: &str) {
    assert!(
      !self.labels.contains_key(name),
      "emit_label: duplicate label '{}' (first defined at offset {}, now at {})",
      name,
      self.labels[name],
      self.output.len()
    );
    self.labels.insert(name.to_string(), self.output.len());
  }

  ////////////////////////////////////////////////////////
  /// Hardware Stack Emitters (Page 1)
  ////////////////////////////////////////////////////////

  fn emit_push_a(&mut self) { self.output.push(0x48); }
  fn emit_pop_a(&mut self) { self.output.push(0x68); }

  // fn emit_push_imm(&mut self, val: u8) { self.emit_lda_imm(val); self.emit_push_a(); }
  fn emit_push_zpg(&mut self, addr: u8) { self.emit_lda_zpg(addr); self.emit_push_a(); }
  fn emit_pop_zpg(&mut self, addr: u8) { self.emit_pop_a(); self.emit_sta_zpg(addr); }

  ////////////////////////////////////////////////////////
  /// Global pre-pass and ROM init table
  ////////////////////////////////////////////////////////

  // Walk top-level items and assign a ZP address to every GlobalVar.
  // Must run before any codegen so function bodies can resolve globals.
  fn alloc_globals(&mut self, items: &[TopLevel]) {
    for item in items {
      if let TopLevel::GlobalVar(_, name, _) = item {
        assert!(
          !self.global_slots.contains_key(name),
          "alloc_globals: duplicate global variable '{}'", name
        );
        let addr = self.next_global;
        assert!(addr <= GLOBALS_TOP - 1, "global ZP overflow");
        self.global_slots.insert(name.clone(), addr);
        self.next_global += 2; // always 16-bit slots
      }
    }
  }

  // Emit the global initialisation sequence inline in the reset vector.
  // For each global: zero both bytes, then if there's an initialiser, store lo (and hi for u16).
  // This runs before JSR _main so globals are ready on entry.
  fn emit_global_init(&mut self, items: &[TopLevel]) {
    for item in items {
      if let TopLevel::GlobalVar(ty, name, init_expr) = item {
        let addr = *self.global_slots.get(name).expect("global not allocated");

        // Always zero both bytes first (.bss semantics)
        self.emit_lda_imm(0x00);
        self.emit_sta_zpg(addr);
        self.emit_sta_zpg(addr + 1);

        // If there's an initialiser, evaluate it and store
        if let Some(expr) = init_expr {
          match expr {
            Expr::Number(n) => {
              let lo = (*n & 0xFF) as u8;
              let hi = ((*n >> 8) & 0xFF) as u8;
              self.emit_lda_imm(lo);
              self.emit_sta_zpg(addr);
              match ty {
                Type::U16 | Type::I16 => {
                  self.emit_lda_imm(hi);
                  self.emit_sta_zpg(addr + 1);
                }
                _ => {}
              }
            }
            // Globals can only be initialised with constants at this stage —
            // non-constant initialisers would require running code before main
            _ => panic!("global initialiser must be a constant literal"),
          }
        }
      }
    }
  }

  ////////////////////////////////////////////////////////
  /// Expression codegen — leaves result in A (lo byte)
  /// For 16-bit results, hi byte is also written to scratch reg hi byte
  ////////////////////////////////////////////////////////

  fn gen_expr_into_a(&mut self, expr: Expr) {
    match expr {
      Expr::Number(n) => {
        self.emit_lda_imm((n & 0xFF) as u8);
      }

      Expr::Identifier(name) => {
        match self.resolve_identifier(&name) {
          Some(VarLocation::ZeroPage(addr)) => self.emit_lda_zpg(addr),
          Some(VarLocation::Absolute(addr)) => self.emit_lda_abs(addr),
          None => panic!("codegen: undeclared identifier '{}'", name),
        }
      }

      Expr::BinOp(lhs, op, rhs) => {
        // Evaluate lhs into A, stash it in a scratch reg, eval rhs into A, then combine
        self.gen_expr_into_a(*lhs);
        let scratch = self.alloc_scratch_reg();
        self.emit_sta_zpg(scratch);   // save lhs

        self.gen_expr_into_a(*rhs);   // rhs now in A

        match op {
          Op::Plus => {
            self.emit_clc();
            self.emit_adc_zpg(scratch);
          }
          Op::Minus => {
            // We want lhs - rhs. rhs is in A, lhs is in scratch.
            // Stash rhs, load lhs, subtract rhs.
            let rhs_scratch = self.alloc_scratch_reg();
            self.emit_sta_zpg(rhs_scratch);
            self.emit_lda_zpg(scratch);
            self.emit_sec();
            self.emit_sbc_zpg(rhs_scratch);
            self.free_scratch_reg(); // free rhs_scratch
          }
          Op::Multiply | Op::Divide => {
            // Not yet implemented — 65C02 has no MUL/DIV
            todo!("multiply/divide not yet implemented")
          }
          // Comparison ops leave result in A as 0x01 (true) or 0x00 (false).
          // These are handled in gen_condition for branches; reaching here means
          // someone assigned the result of a comparison to a variable.
          _ => todo!("comparison op in expression context"),
        }

        self.free_scratch_reg(); // free lhs scratch
      }

      Expr::Call(name, args) => {
        self.gen_call(name, args);
        // Return value is in RET; load it into A
        self.emit_lda_zpg(RET);
      }

      Expr::Cast(_, inner) => {
        // For now just evaluate the inner expression — truncation happens naturally
        // since we only move the lo byte. Full 16-bit cast support later.
        self.gen_expr_into_a(*inner);
      }

      _ => todo!("unimplemented expression variant in gen_expr_into_a"),
    }
  }

  ////////////////////////////////////////////////////////
  /// Function call codegen (shared between Stmt::Expr and Expr::Call)
  ////////////////////////////////////////////////////////

  fn gen_call(&mut self, name: String, args: Vec<Expr>) {
    let arg_count = args.len();

    assert!(
      ARGS_START.saturating_add((arg_count as u8).saturating_mul(2)) <= GLOBALS_START,
      "gen_call: too many arguments to '{}' — would overflow args zone ({} args, ARGS_START=${:02X}, GLOBALS_START=${:02X})",
      name, arg_count, ARGS_START, GLOBALS_START
    );

    // Save FP
    self.emit_push_zpg(FP);
    self.emit_push_zpg(FP + 1);

    // Push args right-to-left so arg 0 lands closest to the return address.
    // Stack at JSR (top = last pushed): PCL, PCH, arg0, arg1, ..., FP_hi, FP_lo
    // Callee reads arg0 at offset 3, arg1 at offset 4, etc.
    for arg in args.into_iter().rev() {
      self.gen_expr_into_a(arg);
      self.emit_push_a();
    }

    self.emit_jsr(&format!("_{}", name));

    // Clean up args
    for _ in 0..arg_count {
      self.emit_pop_a(); // discard to a reg
    }

    // Restore FP (LIFO: hi first)
    self.emit_pop_zpg(FP + 1);
    self.emit_pop_zpg(FP);
  }

  ////////////////////////////////////////////////////////
  /// Statement codegen
  ////////////////////////////////////////////////////////

  fn gen_statement(&mut self, stmt: Stmt) {
    match stmt {

      // Variable declaration: allocate a local slot, evaluate init expr, store it
      Stmt::VarDecl(_ty, name, init_expr) => {
        let addr = self.alloc_local(name);
        if let Some(expr) = init_expr {
          self.gen_expr_into_a(expr);
          self.emit_sta_zpg(addr);
        } else {
          // Zero-init locals with no initialiser
          self.emit_lda_imm(0x00);
          self.emit_sta_zpg(addr);
        }
      }

      // Assignment: evaluate RHS, store into wherever LHS lives
      Stmt::Assign(name, expr) => {
        self.gen_expr_into_a(expr);
        match self.resolve_identifier(&name) {
          Some(VarLocation::ZeroPage(addr)) => self.emit_sta_zpg(addr),
          Some(VarLocation::Absolute(addr)) => self.emit_sta_abs(addr),
          None => panic!("codegen: assignment to undeclared identifier '{}'", name),
        }
      }

      Stmt::Expr(expr) => {
        match expr {
          Expr::Call(name, args) => self.gen_call(name, args),
          other => { self.gen_expr_into_a(other); } // evaluate for side effects
        }
      }

      Stmt::Return(maybe_expr) => {
        if let Some(expr) = maybe_expr {
          self.gen_expr_into_a(expr);
          self.emit_sta_zpg(RET);
        }
        // Actual RTS is emitted at end of gen_function epilogue
      }

      Stmt::If(_, _, _) | Stmt::While(_, _) => {
        todo!("if/while codegen")
      }

      Stmt::DerefAssign(_, _) => {
        todo!("deref assign codegen")
      }
    }
  }

  ////////////////////////////////////////////////////////
  /// Function codegen
  ////////////////////////////////////////////////////////

  fn gen_function(&mut self, function: TopLevel) {
    if let TopLevel::Function(name, params, _ret_type, body) = function {
      // Reset per-function state
      self.local_slots.clear();
      self.local_reg_top = 0;
      self.reg_depth = 0;

      let is_main = name == "main";

      // Pre-pass: count locals so we know how many scratch regs to save
      let local_count = count_locals(&body);
      let regs_used = local_count;
      // If this function makes any calls, FP will be clobbered by callees.
      // Ensure at least 1 scratch slot is saved so the epilogue can restore
      // SP correctly regardless of how many locals/params we have.
      self.emit_label(&format!("_{}", name));

      if !is_main {
        // Capture hardware SP into FP *before* pushing saved regs.
        // This means FP always points just below the return address, so
        // args are always at a fixed offset of 3+ regardless of how many
        // regs we save below.
        self.output.push(0xBA); // TSX
        self.output.push(0x86); self.output.push(FP); // STX FP
        self.emit_lda_imm(0x01);
        self.emit_sta_zpg(FP + 1);

        // Now save scratch regs that locals will occupy (pushed below FP snapshot)
        for i in 0..regs_used {
          let addr = REG_START + i * 2;
          if addr + 1 <= REG_TOP {
            self.emit_push_zpg(addr);
            self.emit_push_zpg(addr + 1);
          }
        }
      }

      // Copy params from hardware stack into args zone ZP slots.
      // Stack at this point (empty-descending, FP captured before reg saves):
      //   [FP+3]: arg 0 lo   <- first arg pushed by caller
      //   [FP+2]: PCH        <- pushed by JSR
      //   [FP+1]: PCL        <- pushed by JSR
      //   [FP+0]: (empty, SP points here)
      // Caller also pushed FP hi/lo above the args but we don't need to
      // account for those since FP was snapshotted before the reg saves.
      let mut offset: u8 = 3;
      for (i, (param_type, param_name)) in params.iter().enumerate() {
        let arg_idx = ARGS_START + ((i as u8) * 2);

        assert!(
          arg_idx + 1 < GLOBALS_START,
          "gen_function: param '{}' of '{}' overflows args zone (arg_idx=${:02X})",
          param_name, name, arg_idx
        );

        self.param_slots.insert(param_name.clone(), arg_idx);
        self.emit_ldy_imm(offset);
        self.emit_lda_indirect_zpg(FP);
        self.emit_sta_zpg(arg_idx);
        offset += match param_type {
          Type::U16 | Type::I16 => 2,
          _ => 1,
        };
      }

      // Generate body
      for stmt in body {
        self.gen_statement(stmt);
      }

      assert!(
        self.reg_depth == 0,
        "gen_function '{}': scratch register leak — reg_depth is {} after body (expected 0)",
        name, self.reg_depth
      );

      if !is_main {
        // Epilogue: pop saved scratch regs back into ZP (restoring caller's values),
        // then restore hardware SP from FP to cleanly unwind the frame.
        // Must pop in reverse push order (LIFO): we pushed lo then hi, so pop hi then lo.
        for i in (0..regs_used).rev() {
          let addr = REG_START + i * 2;
          if addr + 1 <= REG_TOP {
            self.emit_pop_zpg(addr + 1);
            self.emit_pop_zpg(addr);
          }
        }

        // Restore hardware SP to the snapshot point (just below return addr).
        // This cleanly unwinds past anything else on the stack.
        self.emit_ldx_zpg(FP);
        self.emit_txs();
      }

      self.param_slots.clear();
      self.emit_rts();
    } else {
      panic!("gen_function: called with non-Function TopLevel variant");
    }
  }

  ////////////////////////////////////////////////////////
  /// Top-level binary emission
  ////////////////////////////////////////////////////////

  fn emit_runtime(&mut self, items: &[TopLevel]) {
    self.emit_label("_reset_vector");
    self.output.push(0x78); // SEI
    self.output.push(0xD8); // CLD
    self.emit_ldx_imm(0xFF);
    self.emit_txs();

    // Initialise FP ($00/$01) to $01FF — a safe sentinel pointing into page 1.
    // Without this, the first gen_call in main pushes garbage as "saved FP",
    // and the entire call chain uses a corrupt stack anchor on every restore.
    self.emit_lda_imm(0xFF);
    self.emit_sta_zpg(FP);
    self.emit_lda_imm(0x01);
    self.emit_sta_zpg(FP + 1);

    // Initialise globals before calling main
    self.emit_global_init(items);

    self.emit_jsr("_main");

    self.emit_label("_halt");
    self.emit_jmp_abs("_halt");
  }

  fn pad_binary_and_emit_vectors(&mut self, mem_map: Memory_Map) {
    let vector_offset = (0xFFFA - mem_map.rom_start) as usize;
    assert!(
      self.output.len() <= vector_offset,
      "pad_binary_and_emit_vectors: program already overruns 6502 vector table \
       (output={} bytes, vector_offset={} bytes from rom_start=${:04X})",
      self.output.len(), vector_offset, mem_map.rom_start
    );
    while self.output.len() < vector_offset {
      self.output.push(0xEA); // NOP padding
    }
    self.output.push(0x00); self.output.push(0x00); // NMI unused
    let reset_addr = mem_map.rom_start + *self.labels.get("_reset_vector").unwrap() as u16;
    self.output.push((reset_addr & 0xFF) as u8);
    self.output.push((reset_addr >> 8) as u8);
    self.output.push(0x00); self.output.push(0x00); // IRQ unused
  }

  fn resolve_patches(&mut self, rom_start: u16) {
    for (patch_offset, label) in &self.patches {
      let label_offset = self.labels.get(label)
        .expect(&format!("undefined label: {}", label));
      let absolute_addr = rom_start as usize + label_offset;
      assert!(
        *patch_offset + 1 < self.output.len(),
        "resolve_patches: patch site for '{}' at offset {} is out of bounds (output len={})",
        label, patch_offset, self.output.len()
      );
      self.output[*patch_offset] = (absolute_addr & 0xFF) as u8;
      self.output[*patch_offset + 1] = ((absolute_addr >> 8) & 0xFF) as u8;
    }
  }
}

////////////////////////////////////////////////////////
/// Body pre-pass: count VarDecl statements (non-recursive for now)
////////////////////////////////////////////////////////

fn count_locals(body: &[Stmt]) -> u8 {
  let mut count = 0u8;
  for stmt in body {
    match stmt {
      Stmt::VarDecl(_, _, _) => count += 1,
      Stmt::If(_, then_body, else_body) => {
        count += count_locals(then_body);
        if let Some(eb) = else_body { count += count_locals(eb); }
      }
      Stmt::While(_, loop_body) => count += count_locals(loop_body),
      _ => {}
    }
  }
  count
}

////////////////////////////////////////////////////////
/// Public API
////////////////////////////////////////////////////////

pub fn generate(ast: Vec<TopLevel>, symbol_table: SymbolTable, mem_map: Memory_Map) -> (usize, Vec<u8>) {
  let mut generator = Generator::new(symbol_table);

  assert!(
    ast.iter().any(|item| matches!(item, TopLevel::Function(name, _, _, _) if name == "main")),
    "generate: no 'main' function found in AST"
  );

  assert!(
    mem_map.rom_top > mem_map.rom_start,
    "generate: invalid memory map — rom_top (${:04X}) must be greater than rom_start (${:04X})",
    mem_map.rom_top, mem_map.rom_start
  );

  assert!(
    mem_map.rom_start <= 0xFFFA,
    "generate: rom_start (${:04X}) must be below the 6502 vector table at $FFFA",
    mem_map.rom_start
  );

  // Pre-pass: assign ZP addresses to all globals before any codegen
  generator.alloc_globals(&ast);

  // Emit reset vector + global init + halt
  generator.emit_runtime(&ast);

  // Emit each function
  for item in ast {
    match &item {
      TopLevel::Function(_, _, _, _) => generator.gen_function(item),
      TopLevel::RegDecl(_, _, _) | TopLevel::GlobalVar(_, _, _) => {} // handled elsewhere
    }
  }

  let prog_len = generator.output.len();
  let rom_size = (mem_map.rom_top - mem_map.rom_start) as usize;

  if prog_len > rom_size {
    eprintln!("Error: Program is larger than ROM defined in c02_config.ron. Aborting.");
    eprintln!("\tProgram size: {} bytes. ROM size: {} bytes", prog_len, rom_size);
    process::exit(1);
  }

  generator.resolve_patches(mem_map.rom_start);
  generator.pad_binary_and_emit_vectors(mem_map);

  (prog_len, generator.output)
}

pub fn emit_binary<P: AsRef<Path>>(data: &[u8], path: P) -> io::Result<()> {
  let mut file = File::create(path)?;
  file.write_all(data)?;
  Ok(())
}