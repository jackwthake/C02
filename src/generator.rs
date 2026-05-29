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
  current_ret_wide: bool,              // true if the current function returns U16/I16 — controls
                                       // whether Stmt::Return stores to RET+1 as well as RET
}

fn max_expr_scratch_depth(expr: &Expr) -> u8 {
  match expr {
    // Leaves — no scratch needed
    Expr::Number(_) | Expr::Identifier(_) => 0,

    Expr::Cast(_, inner) => max_expr_scratch_depth(inner),

    Expr::BinOp(lhs, op, rhs) => {
      let lhs_depth = max_expr_scratch_depth(lhs);

      let (stash_slots, rhs_depth) = match op {
        Op::Minus => {
          let rhs_d = max_expr_scratch_depth(rhs);
          (2, rhs_d + 2)
        }
        _ => {
          let rhs_d = max_expr_scratch_depth(rhs);
          (1, rhs_d + 1)
        }
      };

      lhs_depth.max(rhs_depth).max(stash_slots)
    }

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
      let cond_d = max_expr_scratch_depth(cond);
      let then_d = max_body_scratch_depth(then_b);
      let else_d = else_b.as_ref().map_or(0, |b| max_body_scratch_depth(b));
      cond_d.max(then_d).max(else_d)
    }
    Stmt::While(cond, body) => {
      max_expr_scratch_depth(cond).max(max_body_scratch_depth(body))
    }
    _ => 0,
  }
}

fn max_body_scratch_depth(body: &[Stmt]) -> u8 {
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

  fn alloc_scratch_reg(&mut self) -> u8 {
    let idx = self.local_reg_top + self.reg_depth;
    self.reg_depth += 1;
    assert!((REG_START + idx * 2) < REG_TOP, "Scratch register overflow!");
    REG_START + idx * 2
  }

  fn free_scratch_reg(&mut self) {
    assert!(self.reg_depth > 0, "free_scratch_reg: underflow");
    self.reg_depth -= 1;
  }

  fn alloc_local(&mut self, name: String, ty: Type) -> u8 {
    let addr = REG_START + self.local_reg_top * 2;
    assert!(
      !self.local_slots.contains_key(&name),
      "alloc_local: duplicate local variable '{}'", name
    );
    // Strict: addr is lo byte, addr+1 is hi byte — both must be below REG_TOP
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

  fn resolve_identifier(&self, name: &str) -> Option<VarLocation> {
    if let Some(&addr) = self.param_slots.get(name) {
      return Some(VarLocation::ZeroPage(addr));
    }
    if let Some(&addr) = self.local_slots.get(name) {
      return Some(VarLocation::ZeroPage(addr));
    }
    if let Some(&addr) = self.global_slots.get(name) {
      return Some(VarLocation::ZeroPage(addr));
    }
    if let Some(Symbol::Register { address, .. }) = self.global_symbols.local_symbols.get(name) {
      return Some(VarLocation::Absolute(*address));
    }
    None
  }

  fn resolve_type(&self, name: &str) -> Option<Type> {
    if let Some(ty) = self.param_types.get(name)  { return Some(ty.clone()); }
    if let Some(ty) = self.local_types.get(name)   { return Some(ty.clone()); }
    if let Some(ty) = self.global_types.get(name)  { return Some(ty.clone()); }
    if let Some(Symbol::Register { .. }) = self.global_symbols.local_symbols.get(name) {
      return Some(Type::U8);
    }
    None
  }

  // Infer whether evaluating this expression produces a 16-bit value.
  // Used by gen_call to decide whether to push 1 or 2 bytes per arg.
  fn expr_is_wide(&self, expr: &Expr) -> bool {
    match expr {
      // Bare numeric literals are narrow unless explicitly cast — a literal
      // that happens to be > 255 will be caught by the analyser before we get here.
      Expr::Number(_) => false,

      Expr::Identifier(name) => self.resolve_type(name)
        .map(|t| type_is_wide(&t))
        .unwrap_or(false),

      // A BinOp is wide if its operands are wide — check lhs as the canonical side.
      Expr::BinOp(lhs, _, _) => self.expr_is_wide(lhs),

      // Cast determines wideness by the target type.
      Expr::Cast(ty, _) => type_is_wide(ty),

      // For calls we'd need the callee's return type from the symbol table.
      // Treating as narrow here is safe: if the caller assigns the result to
      // a wide destination, gen_expr_wide handles it via the Call arm there.
      // Pushing a call result as an arg is always narrow for now.
      Expr::Call(_, _) => false,

      _ => false,
    }
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
  // fn emit_sta_indirect_zpg(&mut self, addr: u8) {
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
      "emit_label: duplicate label '{}' (first at offset {}, now at {})",
      name, self.labels[name], self.output.len()
    );
    self.labels.insert(name.to_string(), self.output.len());
  }

  ////////////////////////////////////////////////////////
  /// Hardware Stack Emitters (Page 1)
  ////////////////////////////////////////////////////////

  fn emit_push_a(&mut self) { self.output.push(0x48); }
  fn emit_pop_a(&mut self)  { self.output.push(0x68); }

  fn emit_push_zpg(&mut self, addr: u8) { self.emit_lda_zpg(addr);  self.emit_push_a(); }
  fn emit_pop_zpg(&mut self, addr: u8)  { self.emit_pop_a(); self.emit_sta_zpg(addr); }

  ////////////////////////////////////////////////////////
  /// Global pre-pass and ROM init table
  ////////////////////////////////////////////////////////

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
        self.next_global += 2;
      }
    }
  }

  fn emit_global_init(&mut self, items: &[TopLevel]) {
    for item in items {
      if let TopLevel::GlobalVar(ty, name, init_expr) = item {
        let addr = *self.global_slots.get(name).expect("global not allocated");

        // Zero both bytes first (.bss semantics)
        self.emit_lda_imm(0x00);
        self.emit_sta_zpg(addr);
        self.emit_sta_zpg(addr + 1);

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
            _ => panic!("global initialiser must be a constant literal"),
          }
        }
      }
    }
  }

  ////////////////////////////////////////////////////////
  /// Expression codegen
  ///
  /// gen_expr_into_a  — evaluates expr, leaves lo byte in A.
  ///                    Used for all narrow (u8/i8) contexts.
  ///
  /// gen_expr_wide    — evaluates expr, leaves lo byte in A and writes
  ///                    hi byte to `hi_addr` in ZP.
  ///                    Used only when the destination is known to be 16-bit.
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
        self.gen_expr_into_a(*lhs);
        let scratch = self.alloc_scratch_reg();
        self.emit_sta_zpg(scratch);

        self.gen_expr_into_a(*rhs);

        match op {
          Op::Plus => {
            self.emit_clc();
            self.emit_adc_zpg(scratch);
          }
          Op::Minus => {
            let rhs_scratch = self.alloc_scratch_reg();
            self.emit_sta_zpg(rhs_scratch);
            self.emit_lda_zpg(scratch);
            self.emit_sec();
            self.emit_sbc_zpg(rhs_scratch);
            self.free_scratch_reg();
          }
          Op::Multiply | Op::Divide => todo!("multiply/divide not yet implemented"),
          _ => todo!("comparison op in expression context"),
        }

        self.free_scratch_reg();
      }

      Expr::Call(name, args) => {
        self.gen_call(name, args);
        self.emit_lda_zpg(RET);
      }

      Expr::Cast(_, inner) => {
        // Narrowing: lo byte naturally truncates, hi is discarded.
        self.gen_expr_into_a(*inner);
      }

      _ => todo!("unimplemented expression variant in gen_expr_into_a"),
    }
  }

  // Evaluate expr and write lo byte into A, hi byte into hi_addr.
  // Only called when the destination slot is known to be 16-bit wide.
  fn gen_expr_wide(&mut self, expr: Expr, hi_addr: u8) {
    match expr {
      Expr::Number(n) => {
        // Write hi first (doesn't use A), then load lo into A.
        self.emit_lda_imm(((n >> 8) & 0xFF) as u8);
        self.emit_sta_zpg(hi_addr);
        self.emit_lda_imm((n & 0xFF) as u8);
      }

      Expr::Identifier(name) => {
        match self.resolve_identifier(&name) {
          Some(VarLocation::ZeroPage(src)) => {
            // Copy hi first (doesn't disturb A), then load lo into A.
            self.emit_lda_zpg(src + 1);
            self.emit_sta_zpg(hi_addr);
            self.emit_lda_zpg(src);
          }
          Some(VarLocation::Absolute(_)) => {
            panic!("gen_expr_wide: cannot load a 16-bit value from an absolute hardware register");
          }
          None => panic!("codegen: undeclared identifier '{}'", name),
        }
      }

      Expr::BinOp(lhs, op, rhs) => {
        // 16-bit arithmetic: evaluate both sides into scratch slots, combine with carry/borrow.
        let lhs_scratch = self.alloc_scratch_reg(); // lo at lhs_scratch, hi at lhs_scratch+1
        self.gen_expr_wide(*lhs, lhs_scratch + 1);
        self.emit_sta_zpg(lhs_scratch);

        let rhs_scratch = self.alloc_scratch_reg(); // lo at rhs_scratch, hi at rhs_scratch+1
        self.gen_expr_wide(*rhs, rhs_scratch + 1);
        self.emit_sta_zpg(rhs_scratch);

        match op {
          Op::Plus => {
            self.emit_clc();
            self.emit_lda_zpg(lhs_scratch);
            self.emit_adc_zpg(rhs_scratch);       // lo result in A, carry set on overflow
            self.emit_sta_zpg(lhs_scratch);        // stash lo result (lhs_scratch now free)
            self.emit_lda_zpg(lhs_scratch + 1);
            self.emit_adc_zpg(rhs_scratch + 1);   // hi result in A, carry propagated
            self.emit_sta_zpg(hi_addr);
            self.emit_lda_zpg(lhs_scratch);        // reload lo result into A
          }
          Op::Minus => {
            self.emit_sec();
            self.emit_lda_zpg(lhs_scratch);
            self.emit_sbc_zpg(rhs_scratch);
            self.emit_sta_zpg(lhs_scratch);        // stash lo result
            self.emit_lda_zpg(lhs_scratch + 1);
            self.emit_sbc_zpg(rhs_scratch + 1);   // borrow propagates via C
            self.emit_sta_zpg(hi_addr);
            self.emit_lda_zpg(lhs_scratch);        // reload lo result into A
          }
          Op::Multiply | Op::Divide => todo!("multiply/divide not yet implemented"),
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
        self.emit_lda_zpg(RET);
      }

      Expr::Cast(target_ty, inner) => {
        if type_is_wide(&target_ty) {
          // Widening cast: evaluate narrow into A, zero-extend hi.
          self.emit_lda_imm(0x00);
          self.emit_sta_zpg(hi_addr);
          self.gen_expr_into_a(*inner);
        } else {
          // Narrowing cast: lo byte into A, hi is zero.
          self.emit_lda_imm(0x00);
          self.emit_sta_zpg(hi_addr);
          self.gen_expr_into_a(*inner);
        }
      }

      _ => todo!("unimplemented expression variant in gen_expr_wide"),
    }
  }

  ////////////////////////////////////////////////////////
  /// Function call codegen
  ////////////////////////////////////////////////////////

  fn gen_call(&mut self, name: String, args: Vec<Expr>) {
    // Save FP before touching the stack
    self.emit_push_zpg(FP);
    self.emit_push_zpg(FP + 1);

    // Push args right-to-left so arg 0 is closest to the return address.
    // Narrow (u8/i8) args push 1 byte; wide (u16/i16) args push 2 bytes (hi then lo).
    // Track the exact byte count so cleanup is always symmetric.
    let mut pushed_bytes: usize = 0;

    for arg in args.into_iter().rev() {
      if self.expr_is_wide(&arg) {
        // Wide arg: evaluate into a scratch slot, push hi then lo
        let scratch = self.alloc_scratch_reg();
        self.gen_expr_wide(arg, scratch + 1);
        self.emit_sta_zpg(scratch);
        self.emit_push_zpg(scratch + 1); // hi
        self.emit_push_zpg(scratch);     // lo
        self.free_scratch_reg();
        pushed_bytes += 2;
      } else {
        // Narrow arg: evaluate directly into A, push 1 byte
        self.gen_expr_into_a(arg);
        self.emit_push_a();
        pushed_bytes += 1;
      }
    }

    self.emit_jsr(&format!("_{}", name));

    // Pop exactly as many bytes as were pushed for args
    for _ in 0..pushed_bytes {
      self.emit_pop_a();
    }

    // Restore FP (LIFO: hi was pushed last, comes off first)
    self.emit_pop_zpg(FP + 1);
    self.emit_pop_zpg(FP);
  }

  ////////////////////////////////////////////////////////
  /// Statement codegen
  ////////////////////////////////////////////////////////

  fn gen_statement(&mut self, stmt: Stmt) {
    match stmt {

      Stmt::VarDecl(ty, name, init_expr) => {
        let addr = self.alloc_local(name, ty.clone());
        if let Some(expr) = init_expr {
          if type_is_wide(&ty) {
            self.gen_expr_wide(expr, addr + 1);
            self.emit_sta_zpg(addr);
          } else {
            // Narrow: zero hi byte so the slot is clean for any future wide read
            self.emit_lda_imm(0x00);
            self.emit_sta_zpg(addr + 1);
            self.gen_expr_into_a(expr);
            self.emit_sta_zpg(addr);
          }
        } else {
          self.emit_lda_imm(0x00);
          self.emit_sta_zpg(addr);
          self.emit_sta_zpg(addr + 1);
        }
      }

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
          other => { self.gen_expr_into_a(other); }
        }
      }

      Stmt::Return(maybe_expr) => {
        if let Some(expr) = maybe_expr {
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
      self.local_slots.clear();
      self.local_types.clear();
      self.param_slots.clear();
      self.param_types.clear();
      self.local_reg_top = 0;
      self.reg_depth = 0;
      self.current_epilogue = format!("_{}_epilogue", name);
      self.current_ret_wide = type_is_wide(&ret_type);

      let is_main = name == "main";

      let local_count   = count_locals(&body);
      let scratch_depth = max_body_scratch_depth(&body);
      let regs_used     = local_count + scratch_depth;

      self.emit_label(&format!("_{}", name));

      if !is_main {
        // Snapshot SP into FP before saving regs so param offsets are fixed.
        self.output.push(0xBA);                    // TSX
        self.output.push(0x86); self.output.push(FP); // STX FP
        self.emit_lda_imm(0x01);
        self.emit_sta_zpg(FP + 1);

        for i in 0..regs_used {
          let addr = REG_START + i * 2;
          if addr + 1 < REG_TOP {
            self.emit_push_zpg(addr);
            self.emit_push_zpg(addr + 1);
          }
        }
      }

      // Copy params from the hardware stack into the args zone.
      //
      // Stack layout at this point (FP snapshotted before reg saves):
      //   [FP+1] = PCL  (pushed by JSR)
      //   [FP+2] = PCH
      //   [FP+3] = first arg byte  (lo for narrow, lo for wide)
      //   [FP+4] = second arg byte (next narrow arg, or hi of first wide arg)
      //   ...
      //
      // The caller pushes narrow args as 1 byte and wide args as 2 bytes (hi then lo,
      // so lo is at the lower offset). Offset advances by 1 for narrow, 2 for wide —
      // matching exactly what the caller pushed.
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

        if type_is_wide(param_type) {
          // Wide param: copy hi byte from the next stack slot
          self.emit_ldy_imm(offset + 1);
          self.emit_lda_indirect_zpg(FP);
          self.emit_sta_zpg(arg_idx + 1);
          offset += 2;
        } else {
          // Narrow param: zero hi slot, advance by 1
          self.emit_lda_imm(0x00);
          self.emit_sta_zpg(arg_idx + 1);
          offset += 1;
        }
      }

      for stmt in body {
        self.gen_statement(stmt);
      }

      assert!(
        self.reg_depth == 0,
        "gen_function '{}': scratch register leak — reg_depth is {} after body (expected 0)",
        name, self.reg_depth
      );

      // All Stmt::Return jumps land here so the epilogue runs exactly once.
      self.emit_label(&self.current_epilogue.clone());

      if !is_main {
        for i in (0..regs_used).rev() {
          let addr = REG_START + i * 2;
          if addr + 1 < REG_TOP {
            self.emit_pop_zpg(addr + 1);
            self.emit_pop_zpg(addr);
          }
        }

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

    // Initialise FP to $01FF before anything else so gen_call in main has a
    // valid saved-FP sentinel to push on the very first call.
    self.emit_lda_imm(0xFF);
    self.emit_sta_zpg(FP);
    self.emit_lda_imm(0x01);
    self.emit_sta_zpg(FP + 1);

    self.emit_global_init(items);
    self.emit_jsr("_main");

    self.emit_label("_halt");
    self.emit_jmp_abs("_halt");
  }

  fn pad_binary_and_emit_vectors(&mut self, mem_map: Memory_Map) {
    let vector_offset = (0xFFFA - mem_map.rom_start) as usize;
    assert!(
      self.output.len() <= vector_offset,
      "pad_binary_and_emit_vectors: program overruns vector table \
       (output={} bytes, vector_offset={} from rom_start=${:04X})",
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
        "resolve_patches: patch site for '{}' at offset {} out of bounds (output len={})",
        label, patch_offset, self.output.len()
      );
      self.output[*patch_offset]     = (absolute_addr & 0xFF) as u8;
      self.output[*patch_offset + 1] = ((absolute_addr >> 8) & 0xFF) as u8;
    }
  }
}

////////////////////////////////////////////////////////
/// Body pre-pass: count VarDecl nodes
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

  generator.alloc_globals(&ast);
  generator.emit_runtime(&ast);

  for item in ast {
    match &item {
      TopLevel::Function(_, _, _, _)                          => generator.gen_function(item),
      TopLevel::RegDecl(_, _, _) | TopLevel::GlobalVar(_, _, _) => {}
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