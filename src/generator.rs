use crate::parser::{TopLevel, Expr, Stmt, Op, Type};
use crate::analyzer::{SymbolTable, Symbol};

use std::collections::HashMap;
use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct Memory_Map {
  soft_stack_start: u16, // start of software stack in main RAM
  rom_start: u16,        // start of ROM where code is emitted
}

struct Generator {
  output: String,                              // accumulates emitted assembly code for the current function, flushed to final output at the end of gen_function
  reg_depth: usize,                            // tracks current virtual register depth for expression evaluation, used to assign r0, r1, etc. in emitted assembly
  label_count: usize,                          // tracks unique labels for control flow
  symbol_table: SymbolTable,                   // symbol table from analyzer, used for register and global variable lookups
  local_offsets: HashMap<String, (u8, Type)>,  // name -> FP offset, type
  global_offsets: HashMap<String, (u8, Type)>, // name -> FP offset, type
  init_stmts: Vec<(String, Type, Expr)>,       // global variable initializers to run at start of main
  param_offsets: HashMap<String, (u16, Type)>, // function parameters passed on stack at fixed offsets from 0x20
  global_top: u8,                              // top of globals in zero page, grows upwards as we allocate
  current_frame_size: u8,                      // grows as we encounter VarDecls
  included_helpers: HashMap<String, bool>,     // track which runtime helpers have been included to avoid duplicates
}

// Tuple format: (helper name, helper code, number of 16-bit args expected, return type)
pub static STD_LIB_FUNCTIONS: &[(&str, &str, usize, Type)] = &[
  ("__mul16", include_str!("../libc02/__mul16.s"), 2, Type::U16),
  ("__div16", include_str!("../libc02/__div16.s"), 2, Type::U16),
  ("memcpy", include_str!("../libc02/memcpy.s"), 2, Type::U16),
  ("strcpy", include_str!("../libc02/strcpy.s"), 3, Type::U16),
];

impl Generator {
  fn new(symbol_table: SymbolTable) -> Self {
    Self {
      output: String::new(),
      reg_depth: 0,
      label_count: 0,
      symbol_table,
      local_offsets: HashMap::new(),
      global_offsets: HashMap::new(),
      global_top: 0x40,
      init_stmts: Vec::new(),
      param_offsets: HashMap::new(),
      current_frame_size: 0,
      included_helpers: HashMap::new(),
    }
  }
  
  // Appends a single line to the output assembly string.
  fn emit(&mut self, line: &str) {
    self.output.push_str(line);
    self.output.push('\n');
  }
  
  // Stores the value in the current virtual register into a local variable's
  // stack slot via (SP),Y indirect indexed addressing. u8/i8 write one byte;
  // u16/i16/ptr write two bytes little-endian at consecutive offsets.
  fn emit_local_store(&mut self, name: &str, data_type: &Type) {
    let (offset, _t) = self.local_offsets.get(name)
    .map(|(offset, t)| (*offset, t.clone()))
    .expect("local_store called for undeclared local");
    let reg = format!("r{}", self.reg_depth);
    
    self.emit(&format!("; --- Store Local '{}' at offset ${:02X} ---", name, offset));
    
    match data_type {
      Type::U8 | Type::I8 => {
        self.emit(&format!("  LDY #${:02X}", offset));
        self.emit(&format!("  LDA {}", reg)); 
        self.emit("  STA (SP),Y");
      }
      Type::U16 | Type::I16 | Type::Ptr(_) => {
        self.emit(&format!("  LDY #${:02X}", offset));
        self.emit(&format!("  LDA {}", reg));
        self.emit("  STA (SP),Y");
        self.emit(&format!("  LDY #${:02X}", offset.wrapping_add(1)));
        self.emit(&format!("  LDA {}+1", reg));
        self.emit("  STA (SP),Y");
      }
      _ => unreachable!()
    }
  }
  
  // Loads a local variable from its stack slot into the current virtual register
  // via (SP),Y indirect indexed addressing. u8/i8 zero-extend into the high byte
  // of the register; u16/i16/ptr load both bytes little-endian.
  fn emit_local_load(&mut self, name: &str, data_type: &Type) {
    let (offset, _t) = self.local_offsets.get(name)
    .map(|(offset, t)| (*offset, t.clone()))
    .expect("local_load called for undeclared local");
    let reg = format!("r{}", self.reg_depth);
    
    self.emit(&format!("; --- Load Local '{}' from offset ${:02X} ---", name, offset));
    
    match data_type {
      Type::U8 | Type::I8 => {
        self.emit(&format!("  LDY #${:02X}", offset));
        self.emit("  LDA (SP),Y");  // make sure exactly two spaces here
        self.emit(&format!("  STA {}", reg));
        self.emit("  LDA #$00");
        self.emit(&format!("  STA {}+1", reg));
      }
      Type::U16 | Type::I16 | Type::Ptr(_) => {
        self.emit(&format!("  LDY #${:02X}", offset));
        self.emit("  LDA (SP),Y");
        self.emit(&format!("  STA {}", reg));
        self.emit(&format!("  LDY #${:02X}", offset.wrapping_add(1)));
        self.emit("  LDA (SP),Y");
        self.emit(&format!("  STA {}+1", reg));
      }
      _ => unreachable!()
    }
  }
  
  
  // Stores the value in the current virtual register into a global variable's
  // absolute zero-page address. Globals live in usr_space ($40–$FF) and are
  // accessed with direct addressing rather than the software stack pointer.
  fn emit_global_store(&mut self, name: &str, data_type: &Type) {
    let (offset, _t) = self.global_offsets.get(name)
    .map(|(offset, t)| (*offset, t.clone()))
    .expect("global_store called for undeclared global");
    
    self.emit(&format!("; --- Store Global '{}' at offset ${:02X} ---", name, offset));
    match data_type {
      Type::U8 | Type::I8 => {
        self.emit(&format!("  LDA r{}", self.reg_depth));
        self.emit(&format!("  STA ${:04X}", offset));
      }
      Type::U16 | Type::I16 | Type::Ptr(_) => {
        self.emit(&format!("  LDA r{}", self.reg_depth));
        self.emit(&format!("  STA ${:04X}", offset));
        self.emit(&format!("  LDA r{}+1", self.reg_depth));
        self.emit(&format!("  STA ${:04X}", offset + 1));
      }
      _ => unreachable!()
    }
  }
  
  // Loads a global variable from its absolute zero-page address into the current
  // virtual register. u8/i8 zero-extend into the high byte; u16/i16/ptr load
  // both bytes. Globals are never accessed via the software stack.
  fn emit_global_load(&mut self, name: &str, data_type: &Type) {
    let (offset, _t) = self.global_offsets.get(name)
    .map(|(offset, t)| (*offset, t.clone()))
    .expect("global_load called for undeclared global");
    
    self.emit(&format!("; --- Load Global '{}' from offset ${:02X} ---", name, offset));
    match data_type {
      Type::U8 | Type::I8 => {
        self.emit(&format!("  LDA ${:04X}", offset));
        self.emit(&format!("  STA r{}", self.reg_depth));
        self.emit("  LDA #$00");
        self.emit(&format!("  STA r{}+1", self.reg_depth));
      }
      Type::U16 | Type::I16 | Type::Ptr(_) => {
        self.emit(&format!("  LDA ${:04X}", offset));
        self.emit(&format!("  STA r{}", self.reg_depth));
        self.emit(&format!("  LDA ${:04X}", offset + 1));
        self.emit(&format!("  STA r{}+1", self.reg_depth));
      }
      _ => unreachable!()
    }
  }
  
  // Stores the value in the current virtual register into a function parameter's
  // ABI slot in the args zone ($20–$2E). Parameters are passed in fixed zero-page
  // locations rather than on the software stack, keeping call overhead minimal.
  fn emit_param_store(&mut self, name: &str, data_type: &Type) {
    let (offset, _t) = self.param_offsets.get(name)
    .map(|(offset, t)| (*offset, t.clone()))
    .expect("param_store called for undeclared parameter");
    
    self.emit(&format!("; --- Store Parameter '{}' at offset ${:04X} ---", name, offset));
    
    match data_type {
      Type::U8 | Type::I8 => {
        self.emit(&format!("  LDA r{}", self.reg_depth));
        self.emit(&format!("  STA ${:04X}", offset));
      }
      Type::U16 | Type::I16 | Type::Ptr(_) => {
        self.emit(&format!("  LDA r{}", self.reg_depth));
        self.emit(&format!("  STA ${:04X}", offset));
        self.emit(&format!("  LDA r{}+1", self.reg_depth));
        self.emit(&format!("  STA ${:04X}", offset + 1));
      }
      _ => unreachable!()
    }
  }
  
  // Loads a function parameter from its ABI slot in the args zone ($20–$2E)
  // into the current virtual register. u8/i8 zero-extend; u16/i16/ptr load
  // both bytes. Parameters are read-only by convention at their fixed addresses.
  fn emit_param_load(&mut self, name: &str, data_type: &Type) {
    let (offset, _t) = self.param_offsets.get(name)
    .map(|(offset, t)| (*offset, t.clone()))
    .expect("param_load called for undeclared parameter");
    
    self.emit(&format!("; --- Load Parameter '{}' from offset ${:04X} ---", name, offset));
    
    match data_type {
      Type::U8 | Type::I8 => {
        self.emit(&format!("  LDA ${:04X}", offset));
        self.emit(&format!("  STA r{}", self.reg_depth));
        self.emit("  LDA #$00");
        self.emit(&format!("  STA r{}+1", self.reg_depth));
      }
      Type::U16 | Type::I16 | Type::Ptr(_) => {
        self.emit(&format!("  LDA ${:04X}", offset));
        self.emit(&format!("  STA r{}", self.reg_depth));
        self.emit(&format!("  LDA ${:04X}", offset + 1));
        self.emit(&format!("  STA r{}+1", self.reg_depth));
      }
      _ => unreachable!()
    }
  }
  
  // Returns a unique local label string (.L0, .L1, ...) and advances the counter.
  // Used by all control-flow codegen to avoid label collisions across the output.
  fn fresh_label(&mut self) -> String {
    let l = format!(".L{}", self.label_count);
    self.label_count += 1;
    l
  }
  
  // Dispatches a top-level AST node to the appropriate code generation path.
  // RegDecl emits nothing — the symbol table already has the address.
  // GlobalVar allocates a zero-page slot and queues its initializer for main.
  // Function delegates to gen_function for full prologue/body/epilogue codegen.
  fn gen_toplevel(&mut self, item: &TopLevel) {
    match item {
      TopLevel::RegDecl(_, _, _) => {
        // symbol table has everything we need, no code to emit
      }
      TopLevel::GlobalVar(t, name, initializer) => {
        self.global_offsets.insert(name.clone(), (self.global_top, t.clone()));
        
        self.global_top += match t {
          Type::U8 | Type::I8 => 1,
          Type::U16 | Type::I16 => 2,
          _ => 0,
        };
        
        if let Some(expr) = initializer {
          self.init_stmts.push((name.clone(), t.clone(), expr.clone()));
        }
      }
      TopLevel::Function(name, params, ret, body) => {
        self.gen_function(name, params, ret, body);
      }
    }
  }
  
  // Pure size calculation — no side effects on local_offsets.
  // If/else branches are mutually exclusive so we take their max, not their sum.
  // While/if bodies start from the current watermark so inner locals stack
  // above outer ones (they are live simultaneously), but siblings share slots.
  fn calc_frame_size(stmts: &[Stmt], curr_size: u8) -> u8 {
    let mut size = curr_size;
    for stmt in stmts {
      match stmt {
        Stmt::Assign(_, _) | Stmt::Return(_) | Stmt::Expr(_) | Stmt::DerefAssign(_, _) => {}

        Stmt::While(_, body) => {
          let inner = Self::calc_frame_size(body, size);
          if inner > size { size = inner; }
        }

        Stmt::If(_, body, else_body) => {
          let then_size = Self::calc_frame_size(body, size);
          let else_size = else_body.as_ref()
            .map(|e| Self::calc_frame_size(e, size))
            .unwrap_or(size);
          size = then_size.max(else_size);
        }

        Stmt::VarDecl(data_type, _, _) => {
          size += match data_type {
            Type::U8 | Type::I8 => 1,
            Type::U16 | Type::I16 => 2,
            _ => 0,
          };
        }
      }
    }
    size
  }

  // Scope-aware offset assignment. Walks stmts at the current nesting level
  // and assigns slots only to VarDecls seen here, starting from `base`.
  // Recurses into nested blocks with the updated watermark so inner locals
  // sit above outer ones — but inner names never overwrite outer entries
  // because we check local_offsets before inserting.
  fn assign_offsets(&mut self, stmts: &[Stmt], base: u8) -> u8 {
    let mut watermark = base;
    for stmt in stmts {
      match stmt {
        Stmt::Assign(_, _) | Stmt::Return(_) | Stmt::Expr(_) | Stmt::DerefAssign(_, _) => {}

        Stmt::While(_, body) => {
          // Inner locals live above the current watermark.
          // The inner watermark may grow, but siblings start fresh from `watermark`.
          self.assign_offsets(body, watermark);
        }

        Stmt::If(_, body, else_body) => {
          // Both branches are mutually exclusive — both start from watermark.
          self.assign_offsets(body, watermark);
          if let Some(else_stmts) = else_body {
            self.assign_offsets(else_stmts, watermark);
          }
        }

        Stmt::VarDecl(data_type, var_name, _) => {
          let slot_size = match data_type {
            Type::U8 | Type::I8 => 1,
            Type::U16 | Type::I16 => 2,
            _ => 0,
          };
          // Only insert if not already present — outer binding wins.
          // This prevents an inner redeclaration from clobbering the outer offset.
          if !self.local_offsets.contains_key(var_name) {
            self.local_offsets.insert(var_name.clone(), (watermark, data_type.clone()));
          }
          // Always advance watermark so subsequent locals get non-overlapping slots,
          // regardless of whether this name was shadowing an outer binding.
          watermark += slot_size;
        }
      }
    }
    watermark
  }
  
  // Emits a complete function: parameter ABI setup, two-pass frame layout,
  // prologue SP bump, optional global initializers (main only), body statements,
  // and epilogue SP restore + RTS. Non-void functions rely on the return
  // statement's codegen to emit the epilogue inline before each RTS.
  fn gen_function(&mut self, name: &str, params: &[(Type, String)], ret: &Type, body: &[Stmt]) {
    for (i, (param_type, param_name)) in params.iter().enumerate() {
      let addr = 0x20 + (i * 2) as u16;
      self.param_offsets.insert(param_name.clone(), (addr, param_type.clone()));
    }
    
    assert!(self.param_offsets.len() <= 8, "Cannot have more than 8 parameters due to stack offset limits");
    
    self.emit(&format!("_{}:", name));
    
    self.local_offsets.clear();
    self.current_frame_size = 0;

    // Pass 1: calculate how much stack space this frame needs (pure, no side effects).
    self.current_frame_size = Self::calc_frame_size(body, 0);
    assert!(self.current_frame_size < 250, "Stack frame exceeded 250 bytes!");

    // Pass 2: assign concrete offsets into local_offsets, scope-aware so that
    // inner redeclarations of the same name never clobber the outer binding.
    self.assign_offsets(body, 0);
    
    // generate function prologue for stack pointer if local variables are found
    if self.current_frame_size > 0 {
      self.emit("; --- Function Prologue ---");
      self.emit("  LDA SP");
      self.emit("  SEC");
      self.emit(&format!("  SBC #${:02X}", self.current_frame_size));
      self.emit("  STA SP");
      self.emit("  LDA SP+1");
      self.emit("  SBC #$00");
      self.emit("  STA SP+1");
    }
    
    // are there globals to initialize?
    if name == "main" && !self.init_stmts.is_empty() {
      self.emit("; --- Global Variable Initialization ---");
      let inits = std::mem::take(&mut self.init_stmts);
      for (var_name, data_type, expr) in &inits {
        self.reg_depth = 0;
        self.gen_expr(expr);
        self.emit_global_store(var_name, data_type);
      }
    }
    
    // generate function body
    for stmt in body {
      self.gen_stmt(stmt);
    }
    
    if ret == &Type::Void {
      // Stack Frame Clean-up
      // main never needs to clean up the stack because if main returns, execution stops
      if self.current_frame_size > 0 && !matches!(name, "main") {
        self.emit("; --- Function Epilogue ---");
        self.emit("  LDA SP");
        self.emit("  CLC");
        self.emit(&format!("  ADC #${:02X}", self.current_frame_size));
        self.emit("  STA SP");
        self.emit("  LDA SP+1");
        self.emit("  ADC #$00");
        self.emit("  STA SP+1");
        self.emit("; -------------------------");
      }
      
      self.emit("  RTS\n");
    }
    
    self.param_offsets.clear();
  }
  
  // Emits assembly for a single statement. Assignment resolves the target through
  // the param → local → global → register priority chain. Control flow (if/while)
  // emits branch/jump scaffolding and recurses into sub-blocks. VarDecl only emits
  // the initializer store — slot offsets were already fixed by assign_offsets.
  fn gen_stmt(&mut self, stmt: &Stmt) {
    match stmt {
      Stmt::Assign(name, expr) => {
        self.reg_depth = 0;
        self.gen_expr(expr);
        
        // check args then locals then globals
        if let Some((_, data_type)) = self.param_offsets.get(name).cloned() {
          self.emit_param_store(name, &data_type);
        } else if let Some((_, data_type)) = self.local_offsets.get(name).cloned() {
          self.emit_local_store(name, &data_type);
        } else if let Some((_, data_type)) = self.global_offsets.get(name).cloned() {
          self.emit_global_store(name, &data_type);
        } else {
          let symbol = self.symbol_table.local_symbols.get(name).cloned();
          match symbol {
            Some(Symbol::Register { address, .. }) => {
              self.emit(&format!("; --- register write at addr: {:04X}", address));
              self.emit("  LDA r0");
              self.emit(&format!("  STA ${:04X}", address));
            }
            Some(Symbol::Variable { data_type }) => {
              self.emit_global_store(name, &data_type);
            }
            _ => unreachable!("assign to undefined or function — analyzer should catch")
          }
        }
      }
      
      Stmt::DerefAssign(ptr_expr, value_expr) => {
        // evaluate value into r0
        self.reg_depth = 0;
        self.gen_expr(value_expr);
        
        // evaluate pointer into r1
        self.reg_depth = 1;
        self.gen_expr(ptr_expr);
        self.reg_depth = 0;
        
        // store value through pointer
        self.emit("  ; --- DerefAssign");
        self.emit("  LDY #$00");
        self.emit("  LDA r0");
        self.emit("  STA (r1),Y");
      }

      Stmt::Expr(expr) => {
        self.reg_depth = 0;
        self.gen_expr(expr);
        // result is in r0 but we can ignore it since this is an expression statement
      }
      
      Stmt::Return(maybe_expr) => {
        if let Some(expr) = maybe_expr {
          self.reg_depth = 0;
          self.gen_expr(expr);  // result in r0
        }
        
        // epilogue
        if self.current_frame_size > 0 {
          self.emit("; --- Function Epilogue ---");
          self.emit("  LDA SP");
          self.emit("  CLC");
          self.emit(&format!("  ADC #${:02X}", self.current_frame_size));
          self.emit("  STA SP");
          self.emit("  LDA SP+1");
          self.emit("  ADC #$00");
          self.emit("  STA SP+1");
        }
        self.emit("  RTS\n");
      }
      
      Stmt::VarDecl(data_type, name, initializer) => {
        // Offsets are already calculated in pre-scan pass!
        // Just evaluate the optional initializer expression and write to memory
        if let Some(expr) = initializer {
          self.reg_depth = 0;
          self.gen_expr(expr);
          self.emit_local_store(name, data_type);
        }
      }
      
      Stmt::If(cond_expr, then_stmts, else_stmts) => {
        let saved_depth = self.reg_depth;
        self.reg_depth = 0;
        
        self.emit("\n  ; --- If");
        self.gen_expr(cond_expr);
        
        match else_stmts {
          Some(else_body) => {
            let else_block = self.fresh_label();
            let end_if = self.fresh_label();
            
            self.emit(&format!("  LDA r0"));
            self.emit(&format!("  BEQ {}", else_block));
            
            // then block (fall-through when condition true)
            self.emit("  ; then block");
            for stmt in then_stmts {
              self.gen_stmt(stmt);
            }
            self.emit(&format!("  JMP {}", end_if));
            
            // else block
            self.emit(&format!("\n{}:   ; else block", else_block));
            for stmt in else_body {
              self.gen_stmt(stmt);
            }
            
            self.emit(&format!("{}:", end_if));
          }
          None => {
            let end_if = self.fresh_label();
            
            self.emit(&format!("  LDA r0"));
            self.emit(&format!("  BEQ {}", end_if));
            
            for stmt in then_stmts {
              self.gen_stmt(stmt);
            }
            
            self.emit(&format!("{}:", end_if));
          }
        }
        
        self.reg_depth = saved_depth;
      }
      
      // TODO: this doesnt work with nested loops
      Stmt::While(cond_expr, body_stmts) => {
        let saved_depth = self.reg_depth;
        self.reg_depth = 0;
        
        let start_label = self.fresh_label();
        let body_label = self.fresh_label();
        let end_label = self.fresh_label();
        
        self.emit("; --- While Loop");
        
        self.emit(&format!("{}:", start_label));
        self.gen_expr(cond_expr);
        
        self.emit(&format!("  LDA r0"));
        self.emit(&format!("  BNE {}", body_label));
        self.emit(&format!("  JMP {}", end_label));
        
        self.emit(&format!("{}:", body_label));
        for stmt in body_stmts {
          self.gen_stmt(stmt);
        }
        
        self.emit(&format!("  JMP {}", start_label));
        self.emit(&format!("{}:", end_label));
        
        self.reg_depth = saved_depth;
      }
      
    }
  }
  
  // Emit a 16-bit unsigned `left < right`, producing 0 or 1 in `lhs_reg`.
  // Template for < and > (caller swaps lhs/rhs for >).
  //
  //   CMP hi bytes: BCC true  (lhs_hi < rhs_hi → definitely true)
  //                 BNE false (lhs_hi > rhs_hi → definitely false)
  //   CMP lo bytes: BCC true  (high equal, lhs_lo < rhs_lo → true)
  //   fall-through: false
  fn emit_lt_op(&mut self, lhs: &str, rhs: &str, comment: &str) {
    let true_label  = self.fresh_label();
    let false_label = self.fresh_label();
    let end_label   = self.fresh_label();
    self.emit(&format!("; --- {}", comment));
    self.emit(&format!("  LDA {}+1", lhs));
    self.emit(&format!("  CMP {}+1", rhs));
    self.emit(&format!("  BCC {}", true_label));   // lhs_hi < rhs_hi → true
    self.emit(&format!("  BNE {}", false_label));  // lhs_hi > rhs_hi → false
    self.emit(&format!("  LDA {}", lhs));          // high bytes equal, check low
    self.emit(&format!("  CMP {}", rhs));
    self.emit(&format!("  BCC {}", true_label));   // lhs_lo < rhs_lo → true
    self.emit(&format!("{}:", false_label));
    self.emit("  LDA #$00");
    self.emit(&format!("  STA {}", lhs));
    self.emit(&format!("  STA {}+1", lhs));
    self.emit(&format!("  JMP {}", end_label));
    self.emit(&format!("{}:", true_label));
    self.emit("  LDA #$01");
    self.emit(&format!("  STA {}", lhs));
    self.emit("  LDA #$00");
    self.emit(&format!("  STA {}+1", lhs));
    self.emit(&format!("{}:", end_label));
  }

  // Emit a 16-bit unsigned `left >= right`, producing 0 or 1 in `lhs_reg`.
  // Template for >= and <= (caller swaps lhs/rhs for <=).
  //
  //   CMP hi bytes: BCC false (lhs_hi < rhs_hi → definitely false)
  //                 BNE true  (lhs_hi > rhs_hi → definitely true)
  //   CMP lo bytes: BCS true  (high equal, lhs_lo >= rhs_lo → true)
  //   fall-through: false
  fn emit_gte_op(&mut self, lhs: &str, rhs: &str, comment: &str) {
    let true_label  = self.fresh_label();
    let false_label = self.fresh_label();
    let end_label   = self.fresh_label();
    self.emit(&format!("; --- {}", comment));
    self.emit(&format!("  LDA {}+1", lhs));
    self.emit(&format!("  CMP {}+1", rhs));
    self.emit(&format!("  BCC {}", false_label));  // lhs_hi < rhs_hi → false
    self.emit(&format!("  BNE {}", true_label));   // lhs_hi > rhs_hi → true
    self.emit(&format!("  LDA {}", lhs));          // high bytes equal, check low
    self.emit(&format!("  CMP {}", rhs));
    self.emit(&format!("  BCS {}", true_label));   // lhs_lo >= rhs_lo → true
    self.emit(&format!("{}:", false_label));
    self.emit("  LDA #$00");
    self.emit(&format!("  STA {}", lhs));
    self.emit(&format!("  STA {}+1", lhs));
    self.emit(&format!("  JMP {}", end_label));
    self.emit(&format!("{}:", true_label));
    self.emit("  LDA #$01");
    self.emit(&format!("  STA {}", lhs));
    self.emit("  LDA #$00");
    self.emit(&format!("  STA {}+1", lhs));
    self.emit(&format!("{}:", end_label));
  }

  // Emits assembly for an expression, leaving the result in the virtual register
  // at the current reg_depth. Nested BinOps increment reg_depth for the right
  // operand so both sides can be live simultaneously without clobbering each other.
  // Callers are responsible for saving/restoring reg_depth around calls.
  fn gen_expr(&mut self, expr: &Expr) {
    match expr {
      Expr::Number(n) => {
        let reg = format!("r{}", self.reg_depth);
        let lo = (n & 0xFF) as u8;
        let hi = ((n >> 8) & 0xFF) as u8;
        self.emit(&format!("  LDA #${:02X}", lo));
        self.emit(&format!("  STA {}", reg));
        self.emit(&format!("  LDA #${:02X}", hi));
        self.emit(&format!("  STA {}+1", reg));
      }
      Expr::Identifier(name) => {
        let reg = format!("r{}", self.reg_depth);
        
        // check params first, then locals, then globals
        if let Some((_, data_type)) = self.param_offsets.get(name).cloned() {
          self.emit_param_load(name, &data_type);
        } else if let Some((_, data_type)) = self.local_offsets.get(name).cloned() {
          self.emit_local_load(name, &data_type);
        } else {
          let symbol = self.symbol_table.local_symbols.get(name).cloned();
          match symbol {
            Some(Symbol::Register { address, data_type }) => {
              match data_type {
                Type::U8 | Type::I8 => {
                  self.emit(&format!("  LDA ${:04X}", address));
                  self.emit(&format!("  STA {}", reg));
                  self.emit("  LDA #$00");
                  self.emit(&format!("  STA {}+1", reg));
                }
                Type::U16 | Type::I16 => {
                  self.emit(&format!("  LDA ${:04X}", address));
                  self.emit(&format!("  STA {}", reg));
                  self.emit(&format!("  LDA ${:04X}", address + 1));
                  self.emit(&format!("  STA {}+1", reg));
                }
                _ => unreachable!("registers cannot be void"),
              }
            }
            Some(Symbol::Variable { data_type }) => {
              self.emit_global_load(name, &data_type);
            }
            Some(Symbol::Function { .. }) => unreachable!("function used as expression"),
            None => unreachable!("undefined identifier: {}", name),
          }
        }
      }
      
      Expr::Deref(inner) => {
        self.gen_expr(inner);
        let reg = format!("r{}", self.reg_depth);
        self.emit(&format!("  ; --- Dereference at address in {}", reg));
        self.emit("  LDY #$00");
        self.emit(&format!("  LDA ({reg}),Y"));
        self.emit(&format!("  STA {reg}"));
        self.emit("  LDA #$00");
        self.emit(&format!("  STA {reg}+1"));
      }
      
      Expr::Call(func_name, args) => {
        // mark std lib helpers as needed if called so they get included in final output
        let mut is_std_helper = false; // default to false for non-helpers
        if let Some((_, _, _, _)) = STD_LIB_FUNCTIONS.iter().find(|(name, _, _, _)| *name == func_name) {
          self.included_helpers.insert(func_name.clone(), true);
          is_std_helper = true;
        }
        
        for (i, arg_expr) in args.iter().enumerate() {
          self.reg_depth = 0;
          self.gen_expr(arg_expr);
          
          self.emit("  LDA r0");
          self.emit(&format!("  STA args{}", i));
          self.emit("  LDA r0+1");
          self.emit(&format!("  STA args{}+1", i));  // note +1
        }
        
        if !is_std_helper {
          // regular function call
          self.emit(&format!("  JSR _{}", func_name));
        } else {
          // std helper call
          self.emit(&format!("  JSR {}", func_name));
        }
        // return value is now in r0
      }
      
      Expr::Unary(op, operand) => {
        let reg = format!("r{}", self.reg_depth);
        match op {
          Op::Negate => {
            self.gen_expr(operand);
            self.emit(&format!("  ; --- Negate {}", reg));
            self.emit("  SEC");
            self.emit("  LDA #$00");
            self.emit(&format!("  SBC {}", reg));
            self.emit(&format!("  STA {}", reg));
            self.emit("  LDA #$00");
            self.emit(&format!("  SBC {}+1", reg));
            self.emit(&format!("  STA {}+1", reg));
          }
          Op::Bang => {
            self.gen_expr(operand);
            let true_label = self.fresh_label();
            let end_label = self.fresh_label();
            self.emit(&format!("  ; --- Bang {}", reg));
            self.emit(&format!("  LDA {}", reg));
            self.emit(&format!("  BEQ {}", true_label));
            self.emit("  LDA #$00");
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  STA {}+1", reg));
            self.emit(&format!("  JMP {}", end_label));
            self.emit(&format!("{}:", true_label));
            self.emit("  LDA #$01");
            self.emit(&format!("  STA {}", reg));
            self.emit("  LDA #$00");
            self.emit(&format!("  STA {}+1", reg));
            self.emit(&format!("{}:", end_label));
          }
          Op::AddressOf => {
            match operand.as_ref() {
              Expr::Identifier(name) => {
                if let Some((offset, _)) = self.global_offsets.get(name).cloned() {
                  self.emit(&format!("  LDA #${:02X}", offset));
                  self.emit(&format!("  STA {}", reg));
                  self.emit("  LDA #$00");
                  self.emit(&format!("  STA {}+1", reg));
                } else if let Some((offset, _)) = self.local_offsets.get(name).cloned() {
                  self.emit("  CLC");
                  self.emit("  LDA SP");
                  self.emit(&format!("  ADC #${:02X}", offset));
                  self.emit(&format!("  STA {}", reg));
                  self.emit("  LDA SP+1");
                  self.emit("  ADC #$00");
                  self.emit(&format!("  STA {}+1", reg));
                } else {
                  unreachable!("AddressOf on undeclared identifier");
                }
              }
              _ => unreachable!("AddressOf on non-lvalue"),
            }
          }
          _ => todo!("Unimplemented unary operator: {:?}", op),
        }
      }
      
      Expr::BinOp(left, op, right) => {
        let reg = format!("r{}", self.reg_depth);
        let right_reg = format!("r{}", self.reg_depth + 1);
        
        // evaluate left into current reg
        self.gen_expr(left);
        
        // evaluate right into next reg
        self.reg_depth += 1;
        self.gen_expr(right);
        self.reg_depth -= 1;
        
        match op {
          Op::Plus => {
            self.emit(&format!("; --- {} + {}", reg, right_reg));
            self.emit("  CLC");
            self.emit(&format!("  LDA {}", reg));
            self.emit(&format!("  ADC {}", right_reg));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  LDA {}+1", reg));
            self.emit(&format!("  ADC {}+1", right_reg));
            self.emit(&format!("  STA {}+1", reg));
          }
          Op::Minus => {
            self.emit(&format!("; --- {} - {}", reg, right_reg));
            self.emit("  SEC");
            self.emit(&format!("  LDA {}", reg));
            self.emit(&format!("  SBC {}", right_reg));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  LDA {}+1", reg));
            self.emit(&format!("  SBC {}+1", right_reg));
            self.emit(&format!("  STA {}+1", reg));
          }
          Op::Multiply => {
            // 65C02 has no MUL instruction — software multiply routine
            // load args into ABI registers and JSR to a runtime helper
            // leave result in reg
            self.included_helpers.insert("__mul16".to_string(), true); // mark helper as needed so it gets included in final output
            self.emit(&format!("; --- {} * {}", reg, right_reg));
            self.emit(&format!("  LDA {}", reg));
            self.emit(&format!("  STA args0"));
            self.emit(&format!("  LDA {}+1", reg));
            self.emit(&format!("  STA args0+1"));
            self.emit(&format!("  LDA {}", right_reg));
            self.emit(&format!("  STA args1"));
            self.emit(&format!("  LDA {}+1", right_reg));
            self.emit(&format!("  STA args1+1"));
            self.emit("  JSR __mul16");
            // convention: result comes back in args0
            self.emit(&format!("  LDA args0"));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  LDA args0+1"));
            self.emit(&format!("  STA {}+1", reg));
          }
          Op::Divide => {
            self.included_helpers.insert("__div16".to_string(), true); // mark helper as needed so it gets included in final output
            self.emit(&format!("; --- {} / {}", reg, right_reg));
            // same pattern as multiply but JSR __div16
            self.emit(&format!("  LDA {}", reg));
            self.emit(&format!("  STA args0"));
            self.emit(&format!("  LDA {}+1", reg));
            self.emit(&format!("  STA args0+1"));
            self.emit(&format!("  LDA {}", right_reg));
            self.emit(&format!("  STA args1"));
            self.emit(&format!("  LDA {}+1", right_reg));
            self.emit(&format!("  STA args1+1"));
            self.emit("  JSR __div16");
            self.emit(&format!("  LDA args0"));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  LDA args0+1"));
            self.emit(&format!("  STA {}+1", reg));
          }
          // comparison ops — return 0 or 1 in reg
          Op::EqualsEquals => {
            self.emit(&format!("; --- {} == {}", reg, right_reg));
            let false_label = self.fresh_label();
            let end_label = self.fresh_label();
            // compare low bytes
            self.emit(&format!("  LDA {}", reg));
            self.emit(&format!("  CMP {}", right_reg));
            self.emit(&format!("  BNE {}", false_label));
            // compare high bytes
            self.emit(&format!("  LDA {}+1", reg));
            self.emit(&format!("  CMP {}+1", right_reg));
            self.emit(&format!("  BNE {}", false_label));
            // true case
            self.emit(&format!("  LDA #$01"));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  LDA #$00"));
            self.emit(&format!("  STA {}+1", reg));
            self.emit(&format!("  JMP {}", end_label));
            // false case
            self.emit(&format!("{}:", false_label));
            self.emit(&format!("  LDA #$00"));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  STA {}+1", reg));
            self.emit(&format!("{}:", end_label));
          }
          Op::BangEquals => {
            self.emit(&format!("; --- {} != {}", reg, right_reg));
            let true_label = self.fresh_label();
            let end_label = self.fresh_label();
            // compare low bytes
            self.emit(&format!("  LDA {}", reg));
            self.emit(&format!("  CMP {}", right_reg));
            self.emit(&format!("  BNE {}", true_label));
            // compare high bytes
            self.emit(&format!("  LDA {}+1", reg));
            self.emit(&format!("  CMP {}+1", right_reg));
            self.emit(&format!("  BNE {}", true_label));
            // false case
            self.emit(&format!("  LDA #$00"));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  STA {}+1", reg));
            self.emit(&format!("  JMP {}", end_label));
            // true case
            self.emit(&format!("{}:", true_label));
            self.emit(&format!("  LDA #$01"));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  LDA #$00"));
            self.emit(&format!("  STA {}+1", reg));
            self.emit(&format!("{}:", end_label));
          }
          // Unsigned magnitude comparisons — high byte first, then low byte as tiebreaker.
          // CMP sets C=1 if A >= M (no borrow), C=0 if A < M (borrow occurred).
          // > and <= swap operands to reuse the < and >= templates respectively.
          Op::Lt => {
            self.emit_lt_op(&reg, &right_reg, &format!("{} < {}", reg, right_reg));
          }
          Op::Gt => {
            // left > right  ↔  right < left
            self.emit_lt_op(&right_reg, &reg, &format!("{} > {}", reg, right_reg));
          }
          Op::Gte => {
            self.emit_gte_op(&reg, &right_reg, &format!("{} >= {}", reg, right_reg));
          }
          Op::Lte => {
            // left <= right  ↔  right >= left
            self.emit_gte_op(&right_reg, &reg, &format!("{} <= {}", reg, right_reg));
          }
          _ => todo!("Unimplemented binary operator: {:?}", op)
        }
      }
      
      _ => todo!("Unimplemented expression: {:?}", expr)
    }
  }
}

// Entry point for code generation. Emits the full assembly output as a String:
// zero-page register definitions, reset vectors, runtime startup, all top-level
// items in AST order, and any runtime helper functions that were referenced
// during codegen (mul16, div16, memcpy, strcpy). The memory map configures the
// soft stack base address and the ROM origin for the .org directive.
pub fn generate(ast: Vec<TopLevel>, symbol_table: SymbolTable, mem_map: Memory_Map) -> String {
  let mut generator = Generator::new(symbol_table);
  
  generator.emit(include_str!("../c02rt/reg.s"));
  generator.emit(include_str!("../c02rt/c02_vectors.s"));
  
  generator.emit(&format!(
    "; =============================================================================
; RUNTIME STARTUP CODE
; =============================================================================
    
  .org ${:04X}",
    mem_map.rom_start
  ));
  
  generator.emit(include_str!("../c02rt/c02rt0.s"));
  
  let soft_stack = mem_map.soft_stack_start;
  let low_byte = soft_stack & 0xFF;          
  let high_byte = (soft_stack >> 8) & 0xFF;  
  
  generator.emit(&format!(
    "    ; --- Initialize Software Stack Pointer (SP) ---
    ; software stack grows down from ${:04X} in main RAM
    LDA #${:02X}
    STA SP          ; Low byte of ${:04X}
    LDA #${:02X}
    STA SP+1        ; High byte of ${:04X}",
    soft_stack, low_byte, soft_stack, high_byte, soft_stack
  ));
  
  generator.emit(include_str!("../c02rt/c02rt0_end.s"));
  
  for item in &ast {
    generator.reg_depth = 0; // reset register depth for each top-level item
    generator.gen_toplevel(item);
  }
  
  // loop through included_helpers and append any that were marked as needed during codegen
  // This consumes the reference to generator.included_helpers, 
  // then the borrow ends immediately.
  let helpers_to_include: Vec<String> = generator
  .included_helpers
  .iter()
  .filter(|(_, needed)| **needed) // Dereference needed twice
  .map(|(helper, _)| helper.clone())
  .collect();
  
  for helper in helpers_to_include {
    generator.emit(&format!("\n; --- Include helper: {} ---", helper));
    
    if let Some((_, code, ..)) = STD_LIB_FUNCTIONS.iter().find(|(name, _, _, _)| *name == helper) {
      generator.emit(code);
    } else {
      panic!("Helper function '{}' not found in HELPER_FUNCTIONS", helper);
    }
  }
  generator.output
}