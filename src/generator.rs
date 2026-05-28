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
static RET: u8        = 0x02;  // 2 bytes: return value lo, hi
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

fn type_is_wide(ty: &Type) -> bool {
  matches!(ty, Type::U16 | Type::I16)
}

struct Generator {
  output: Vec<u8>,
  labels: HashMap<String, usize>,
  patches: Vec<(usize, String)>,

  global_symbols: SymbolTable,
  global_slots: HashMap<String, u8>,    // global var name -> ZP address ($30+)
  global_types: HashMap<String, Type>,  // global var name -> type (for wide stores)
  next_global: u8,                      // next free ZP address in globals zone

  param_slots: HashMap<String, u8>,    // param name -> ZP address in args zone
  param_types: HashMap<String, Type>,  // param name -> type (for wide loads/stores)
  local_slots: HashMap<String, u8>,    // local var name -> ZP address in scratch zone
  local_types: HashMap<String, Type>,  // local var name -> type (for wide stores)
  local_reg_top: u8,                   // watermark: next free scratch reg index (0-based)
                                       // locals own [0, local_reg_top), expr scratch uses above

  reg_depth: u8,                       // current expression scratch depth above local_reg_top
  current_epilogue: String,            // label for the current function's epilogue — set by
                                       // gen_function, read by Stmt::Return to emit an early-exit jump
  current_ret_wide: bool,              // true if the current function returns U16/I16
}

fn max_expr_scratch_depth(expr: &Expr) -> u8 {
  match expr {
    // Leaves — no scratch needed
    Expr::Number(_) | Expr::Identifier(_) => 0,

    Expr::Cast(_, inner) => max_expr_scratch_depth(inner),

    Expr::BinOp(lhs, op, rhs) => {
      // Evaluating lhs first, then stashing it in a scratch reg while rhs runs.
      // That scratch reg is live the whole time rhs evaluates, so rhs depth
      // is offset by 1 (or 2 for Minus which needs a second stash slot).
      let lhs_depth = max_expr_scratch_depth(lhs);

      let (stash_slots, rhs_depth) = match op {
        Op::Minus => {
          // lhs scratch + rhs scratch both live simultaneously
          let rhs_d = max_expr_scratch_depth(rhs);
          (2, rhs_d + 2)
        }
        _ => {
          let rhs_d = max_expr_scratch_depth(rhs);
          (1, rhs_d + 1)
        }
      };

      // Peak is either: during lhs eval (lhs_depth),
      // or during rhs eval with stash slots already occupied (rhs_depth)
      lhs_depth.max(rhs_depth).max(stash_slots)
    }

    // A call itself uses no scratch — args are evaluated left-to-right
    // and each is pushed immediately, so they don't accumulate in scratch regs.
    // The depth is just the max across all arg expressions evaluated individually.
    Expr::Call(_, args) => {
      args.iter().map(|a| max_expr_scratch_depth(a)).max().unwrap_or(0)
    }

    _ => 0,
  }
}

fn max_stmt_scratch_depth(stmt: &Stmt) -> u8 {
  match stmt {
    Stmt::VarDecl(_, _, Some(e)) => max_expr_scratch_depth(e),
    Stmt::Assign(_, e)           => max_expr_scratch_depth(e),
    Stmt::Expr(e)                => max_expr_scratch_depth(e),
    Stmt::Return(Some(e))        => max_expr_scratch_depth(e),
    Stmt::If(cond, then_b, else_b) => {
      let cond_d  = max_expr_scratch_depth(cond);
      let then_d  = max_body_scratch_depth(then_b);
      let else_d  = else_b.as_ref().map_or(0, |b| max_body_scratch_depth(b));
      cond_d.max(then_d).max(else_d)
    }
    Stmt::While(cond, body) => {
      max_expr_scratch_depth(cond).max(max_body_scratch_depth(body))
    }
    _ => 0,
  }
}

fn max_body_scratch_depth(body: &[Stmt]) -> u8 {
  // Statements are sequential — scratch is fully freed between them,
  // so we take the max, not the sum.
  body.iter().map(max_stmt_scratch_depth).max().unwrap_or(0)
}

impl Generator {
  fn new(symbol_table: SymbolTable) -> Self {
    Generator {
      output: Vec::new(),
      labels: HashMap::new(),
      patches: Vec::new(),
      global_symbols: symbol_table,
      global_slots: HashMap::new(),
      global_types: HashMap::new(),
      next_global: GLOBALS_START,
      param_slots: HashMap::new(),
      param_types: HashMap::new(),
      local_slots: HashMap::new(),
      local_types: HashMap::new(),
      local_reg_top: 0,
      reg_depth: 0,
      current_epilogue: String::new(),
      current_ret_wide: false,
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
  // Returns ZP address of lo byte. Every slot is 2 bytes wide regardless of
  // type — hi byte is zeroed on init and written for U16/I16 values.
  fn alloc_local(&mut self, name: String, ty: Type) -> u8 {
    let addr = REG_START + self.local_reg_top * 2;

    assert!(
      !self.local_slots.contains_key(&name),
      "alloc_local: duplicate local variable '{}'", name
    );
    // Strict less-than: addr is the lo byte, addr+1 is the hi byte.
    // Both must fit inside the scratch zone, so addr+1 < REG_TOP
    // (REG_TOP is the last valid address, meaning addr+1 must be below it,
    // not equal to it, so the hi byte doesn't alias the boundary byte).
    assert!(
      addr + 1 < REG_TOP,
      "alloc_local: local variable '{}' would overflow scratch register zone (addr=${:02X}, REG_TOP=${:02X})",
      name, addr, REG_TOP
    );

    self.local_reg_top += 1;
    self.local_types.insert(name.clone(), ty);
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

  // Look up the type of a named variable across all scopes.
  fn resolve_type(&self, name: &str) -> Option<Type> {
    if let Some(ty) = self.param_types.get(name) { return Some(ty.clone()); }
    if let Some(ty) = self.local_types.get(name)  { return Some(ty.clone()); }
    if let Some(ty) = self.global_types.get(name) { return Some(ty.clone()); }
    // Hardware registers are always u8-wide (single byte mapped I/O)
    if let Some(Symbol::Register { .. }) = self.global_symbols.local_symbols.get(name) {
      return Some(Type::U8);
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
  // fn emit_sta_indirect_zpg(&mut self, addr: u8) { // Ill need this eventually
  //   self.output.push(0x91);
  //   self.output.push(addr);
  // }

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
      if let TopLevel::GlobalVar(ty, name, _) = item {
        assert!(
          !self.global_slots.contains_key(name),
          "alloc_globals: duplicate global variable '{}'", name
        );
        let addr = self.next_global;
        assert!(addr <= GLOBALS_TOP - 1, "global ZP overflow");
        self.global_slots.insert(name.clone(), addr);
        self.global_types.insert(name.clone(), ty.clone());
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
              if type_is_wide(ty) {
                self.emit_lda_imm(hi);
                self.emit_sta_zpg(addr + 1);
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
  /// Expression codegen
  ///
  /// gen_expr_into_a:  evaluates expr, leaves lo byte in A.
  ///                   For narrow (u8) types this is sufficient.
  ///
  /// gen_expr_wide:    evaluates expr, leaves lo byte in A and writes
  ///                   hi byte to `hi_addr` in ZP. Used when the
  ///                   destination slot is known to be 16-bit wide.
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
        self.emit_sta_zpg(scratch);   // save lhs lo

        self.gen_expr_into_a(*rhs);   // rhs lo now in A

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
        // Return value lo byte is in RET
        self.emit_lda_zpg(RET);
      }

      Expr::Cast(_, inner) => {
        // Truncation to lo byte happens naturally — we only read A.
        self.gen_expr_into_a(*inner);
      }

      _ => todo!("unimplemented expression variant in gen_expr_into_a"),
    }
  }

  // Evaluate expr and write lo byte into A, hi byte into hi_addr.
  // Used for 16-bit destinations (U16/I16 locals, params, globals, RET).
  fn gen_expr_wide(&mut self, expr: Expr, hi_addr: u8) {
    match expr {
      Expr::Number(n) => {
        self.emit_lda_imm((n & 0xFF) as u8);
        // Write hi immediately — A still holds lo after this
        self.emit_lda_imm(((n >> 8) & 0xFF) as u8);
        self.emit_sta_zpg(hi_addr);
        // Restore lo into A so the caller can STA to the lo slot
        self.emit_lda_imm((n & 0xFF) as u8);
      }

      Expr::Identifier(name) => {
        match self.resolve_identifier(&name) {
          Some(VarLocation::ZeroPage(src)) => {
            // Copy hi byte first (doesn't touch A yet), then lo into A
            self.emit_lda_zpg(src + 1);
            self.emit_sta_zpg(hi_addr);
            self.emit_lda_zpg(src);
          }
          Some(VarLocation::Absolute(_)) => {
            // Hardware registers are always 8-bit; treat hi as zero
            panic!("gen_expr_wide: cannot load a 16-bit value from an absolute hardware register");
          }
          None => panic!("codegen: undeclared identifier '{}'", name),
        }
      }

      Expr::BinOp(lhs, op, rhs) => {
        // 16-bit arithmetic: evaluate both sides wide, combine with carry.
        // lhs lo -> A, lhs hi -> scratch+1; rhs lo -> A, rhs hi -> scratch+1.
        let lhs_scratch = self.alloc_scratch_reg(); // lo at lhs_scratch, hi at lhs_scratch+1

        self.gen_expr_wide(*lhs, lhs_scratch + 1);
        self.emit_sta_zpg(lhs_scratch); // store lhs lo

        let rhs_scratch = self.alloc_scratch_reg(); // lo at rhs_scratch, hi at rhs_scratch+1

        self.gen_expr_wide(*rhs, rhs_scratch + 1);
        self.emit_sta_zpg(rhs_scratch); // store rhs lo

        match op {
          Op::Plus => {
            // Add lo bytes first; carry propagates into hi add.
            // lhs_scratch = lhs lo, lhs_scratch+1 = lhs hi
            // rhs_scratch = rhs lo, rhs_scratch+1 = rhs hi
            self.emit_clc();
            self.emit_lda_zpg(lhs_scratch);
            self.emit_adc_zpg(rhs_scratch);       // lo result in A, carry set on overflow
            self.emit_sta_zpg(lhs_scratch);        // stash lo result back in lhs_scratch (now free)
            self.emit_lda_zpg(lhs_scratch + 1);
            self.emit_adc_zpg(rhs_scratch + 1);   // hi result in A, carry propagated
            self.emit_sta_zpg(hi_addr);            // store hi result
            self.emit_lda_zpg(lhs_scratch);        // reload lo result into A
            // A = lo result, hi_addr = hi result
          }
          Op::Minus => {
            self.emit_sec();
            self.emit_lda_zpg(lhs_scratch);
            self.emit_sbc_zpg(rhs_scratch);
            self.emit_sta_zpg(lhs_scratch); // stash lo result back in lhs_scratch
            self.emit_lda_zpg(lhs_scratch + 1);
            self.emit_sbc_zpg(rhs_scratch + 1); // borrow propagates via C
            self.emit_sta_zpg(hi_addr);
            self.emit_lda_zpg(lhs_scratch); // reload lo result into A
          }
          Op::Multiply | Op::Divide => {
            todo!("multiply/divide not yet implemented")
          }
          _ => todo!("comparison op in expression context"),
        }

        self.free_scratch_reg(); // free rhs_scratch
        self.free_scratch_reg(); // free lhs_scratch
      }

      Expr::Call(name, args) => {
        self.gen_call(name, args);
        // Return value: lo in RET, hi in RET+1
        self.emit_lda_zpg(RET + 1);
        self.emit_sta_zpg(hi_addr);
        self.emit_lda_zpg(RET); // lo into A
      }

      Expr::Cast(target_ty, inner) => {
        if type_is_wide(&target_ty) {
          // Widening cast: evaluate inner narrow into A, zero-extend hi.
          // Zero hi_addr first so carry from any prior operation doesn't linger,
          // then evaluate — A holds lo, hi_addr holds 0x00.
          self.emit_lda_imm(0x00);
          self.emit_sta_zpg(hi_addr);
          self.gen_expr_into_a(*inner); // lo into A
        } else {
          // Narrowing cast: evaluate lo into A, hi_addr gets zero.
          self.emit_lda_imm(0x00);
          self.emit_sta_zpg(hi_addr);
          self.gen_expr_into_a(*inner);
        }
      }

      _ => todo!("unimplemented expression variant in gen_expr_wide"),
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
    // Each arg is pushed hi then lo (hi first so lo is at the lower stack offset,
    // matching the param-copy logic: arg0 lo at FP+3, arg0 hi at FP+4).
    // For u8 args the hi push is always zero.
    //
    // Stack at JSR (top = last pushed, lowest address):
    //   PCL, PCH, arg0_lo, arg0_hi, arg1_lo, arg1_hi, ..., FP_hi, FP_lo
    for arg in args.into_iter().rev() {
      // Evaluate into a scratch slot so we have both bytes before pushing
      let scratch = self.alloc_scratch_reg();
      self.gen_expr_wide(arg, scratch + 1);
      self.emit_sta_zpg(scratch); // store lo

      // Push hi then lo so lo is closer to return address (lower stack offset)
      self.emit_push_zpg(scratch + 1); // hi
      self.emit_push_zpg(scratch);     // lo

      self.free_scratch_reg();
    }

    self.emit_jsr(&format!("_{}", name));

    // Clean up args — 2 bytes per arg regardless of type
    for _ in 0..arg_count * 2 {
      self.emit_pop_a();
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

      // Variable declaration: allocate a local slot, evaluate init expr, store it.
      // Always stores both bytes — hi is zeroed for u8 types.
      Stmt::VarDecl(ty, name, init_expr) => {
        let addr = self.alloc_local(name, ty.clone());
        if let Some(expr) = init_expr {
          if type_is_wide(&ty) {
            self.gen_expr_wide(expr, addr + 1);
            self.emit_sta_zpg(addr);
          } else {
            // Narrow: zero hi byte, store lo
            self.emit_lda_imm(0x00);
            self.emit_sta_zpg(addr + 1);
            self.gen_expr_into_a(expr);
            self.emit_sta_zpg(addr);
          }
        } else {
          // Zero-init both bytes
          self.emit_lda_imm(0x00);
          self.emit_sta_zpg(addr);
          self.emit_sta_zpg(addr + 1);
        }
      }

      // Assignment: evaluate RHS, store into wherever LHS lives.
      // Wide assignments write both bytes; narrow assignments write lo only
      // (hi byte of the slot retains whatever it held, which for well-typed
      // programs is always zero for u8 slots).
      Stmt::Assign(name, expr) => {
        let ty = self.resolve_type(&name).unwrap_or(Type::U8);
        match self.resolve_identifier(&name) {
          Some(VarLocation::ZeroPage(addr)) => {
            if type_is_wide(&ty) {
              self.gen_expr_wide(expr, addr + 1);
              self.emit_sta_zpg(addr);
            } else {
              self.gen_expr_into_a(expr);
              self.emit_sta_zpg(addr);
            }
          }
          Some(VarLocation::Absolute(abs_addr)) => {
            // Hardware registers are always 8-bit
            self.gen_expr_into_a(expr);
            self.emit_sta_abs(abs_addr);
          }
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
          // ret_type is stored in current_ret_type; use it to decide wide vs narrow
          if self.current_ret_wide {
            self.gen_expr_wide(expr, RET + 1);
            self.emit_sta_zpg(RET);
          } else {
            // Zero hi byte of RET so callers reading RET+1 see a clean value
            self.emit_lda_imm(0x00);
            self.emit_sta_zpg(RET + 1);
            self.gen_expr_into_a(expr);
            self.emit_sta_zpg(RET);
          }
        }
        // Jump to the epilogue so the reg-restore + TXS always runs,
        // even for early returns inside if/while bodies.
        // Note: when return is the last statement this emits a JMP that lands
        // on the immediately following epilogue label — a harmless 3-byte no-op.
        let epilogue = self.current_epilogue.clone();
        self.emit_jmp_abs(&epilogue);
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
    if let TopLevel::Function(name, params, ret_type, body) = function {
      // Reset per-function state — both slot maps cleared here so a panic
      // mid-body can't leak state from the previous function.
      self.local_slots.clear();
      self.local_types.clear();
      self.param_slots.clear();
      self.param_types.clear();
      self.local_reg_top = 0;
      self.reg_depth = 0;
      self.current_epilogue = format!("_{}_epilogue", name);
      self.current_ret_wide = type_is_wide(&ret_type);

      let is_main = name == "main";

      // Pre-pass: count locals + max expression scratch depth so we know
      // exactly how many scratch regs to save/restore around this frame.
      let local_count = count_locals(&body);
      let scratch_depth = max_body_scratch_depth(&body);
      let regs_used = local_count + scratch_depth;

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
          if addr + 1 < REG_TOP {
            self.emit_push_zpg(addr);
            self.emit_push_zpg(addr + 1);
          }
        }
      }

      // Copy params from hardware stack into args zone ZP slots.
      // Stack at this point (empty-descending, FP captured before reg saves):
      //   [FP+3]: arg 0 lo
      //   [FP+4]: arg 0 hi
      //   [FP+5]: arg 1 lo   <- next arg regardless of type (always 2 bytes per arg)
      //   ...
      //   [FP+2]: PCH        <- pushed by JSR
      //   [FP+1]: PCL        <- pushed by JSR
      //   [FP+0]: (empty, SP points here)
      let mut offset: u8 = 3;
      for (i, (param_type, param_name)) in params.iter().enumerate() {
        let arg_idx = ARGS_START + ((i as u8) * 2);

        assert!(
          arg_idx + 1 < GLOBALS_START,
          "gen_function: param '{}' of '{}' overflows args zone (arg_idx=${:02X})",
          param_name, name, arg_idx
        );

        self.param_slots.insert(param_name.clone(), arg_idx);
        self.param_types.insert(param_name.clone(), param_type.clone());

        // Always copy lo byte
        self.emit_ldy_imm(offset);
        self.emit_lda_indirect_zpg(FP);
        self.emit_sta_zpg(arg_idx);

        // Copy hi byte — for narrow params the caller pushed zero, so this
        // is always safe and keeps the slot clean for any future wide read
        self.emit_ldy_imm(offset + 1);
        self.emit_lda_indirect_zpg(FP);
        self.emit_sta_zpg(arg_idx + 1);

        // Each arg always occupies exactly 2 stack bytes regardless of type
        offset += 2;
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

      // Epilogue label — all Stmt::Return jumps converge here so the
      // reg-restore + TXS runs exactly once regardless of exit point.
      self.emit_label(&self.current_epilogue.clone());

      if !is_main {
        // Epilogue: pop saved scratch regs back into ZP (restoring caller's values),
        // then restore hardware SP from FP to cleanly unwind the frame.
        // Must pop in reverse push order (LIFO): we pushed lo then hi, so pop hi then lo.
        for i in (0..regs_used).rev() {
          let addr = REG_START + i * 2;
          if addr + 1 < REG_TOP {
            self.emit_pop_zpg(addr + 1);
            self.emit_pop_zpg(addr);
          }
        }

        // Restore hardware SP to the snapshot point (just below return addr).
        // This cleanly unwinds past anything else on the stack.
        self.emit_ldx_zpg(FP);
        self.emit_txs();
      }

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
        .unwrap_or_else(|| panic!("undefined label: {}", label));
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