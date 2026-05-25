use crate::parser::{TopLevel, Expr, Stmt, Op, Type};
use crate::analyzer::{SymbolTable, Symbol};

use serde::Deserialize;

use std::fs::File;
use std::io::{self, Write};
use std::path::Path;
use std::collections::HashMap;

#[derive(Debug, Deserialize)]
pub struct Memory_Map {
  pub rom_start: u16,        // start of ROM where code is emitted
}

// ZP layout
static FP: u8 = 0x00;
static RET: u8 = 0x02;
static REG_START: u8 = 0x04;
static REG_TOP: u8 = 0x1D;
static ARGS_START: u8 = 0x1E;
static ARGS_TOP: u8 = 0x2F;
static GLOBALS_START: u8 = 0x30;
static GLOBALS_TOP: u8 = 0xFF;

struct Generator {
  output: Vec<u8>,
  labels: HashMap<String, usize>,        // label name → offset in output
  patches: Vec<(usize, String)>,         // (offset to patch, label name)
  global_symbols: SymbolTable,
  param_slots: HashMap<String, u8>,      // param name → args ZP address
  reg_depth: u8,                         // current scratch register depth, starts at 0 = r0
}

impl Generator {
  fn new(symbol_table: SymbolTable) -> Self {
    Generator {
      output: Vec::new(),
      labels: HashMap::new(),
      patches: Vec::new(),
      global_symbols: symbol_table,
      param_slots: HashMap::new(),
      reg_depth: 0,
    }
  }
  
  ////////////////////////////////////////////////////////
  /// OP Code emitters
  ////////////////////////////////////////////////////////
  fn emit_lda_imm(&mut self, val: u8) {
    self.output.push(0xa9);
    self.output.push(val);
  }
  
  fn emit_ldx_imm(&mut self, val: u8) {
    self.output.push(0xa2);
    self.output.push(val);
  }
  
  fn emit_ldy_imm(&mut self, val: u8) {
    self.output.push(0xa0);
    self.output.push(val);
  }
  
  fn emit_sta_zpg(&mut self, addr: u8) {
    self.output.push(0x85);
    self.output.push(addr);
  }
  
  fn emit_sta_abs(&mut self, addr: u16) {
    self.output.push(0x8D);
    self.output.push((addr & 0xFF) as u8);
    self.output.push((addr >> 8) as u8);
  }
  
  fn emit_lda_zpg(&mut self, addr: u8) {
    self.output.push(0xa5);
    self.output.push(addr);
  }
  
  fn emit_lda_indirect_zpg(&mut self, addr: u8) {
    self.output.push(0xB1); // LDA (addr), Y
    self.output.push(addr);
  }
  
  fn emit_jsr(&mut self, label: &str) {
    self.output.push(0x20);
    let patch_offset = self.output.len();
    self.output.push(0x00); // lo placeholder
    self.output.push(0x00); // hi placeholder
    self.patches.push((patch_offset, label.to_string()));
  }
  
  fn emit_jmp_abs(&mut self, label: &str) {
    self.output.push(0x4c);
    let patch_offset = self.output.len();
    self.output.push(0x00); // lo placeholder
    self.output.push(0x00); // hi placeholder
    self.patches.push((patch_offset, label.to_string()));
  }
  
  fn emit_rts(&mut self) {
    self.output.push(0x60);
  }
  
  fn emit_label(&mut self, name: &str) {
    self.labels.insert(name.to_string(), self.output.len());
  }
  
  ////////////////////////////////////////////////////////
  /// Hardware Stack Emitters (Page 1)
  ////////////////////////////////////////////////////////  
  fn emit_push_a(&mut self) {
    self.output.push(0x48); // PHA
  }
  
  fn emit_push_imm(&mut self, val: u8) {
    self.emit_lda_imm(val);
    self.emit_push_a();
  }
  
  fn emit_pop_a(&mut self) {
    self.output.push(0x68); // PLA
  }
  
  fn emit_pop_zpg(&mut self, addr: u8) {
    self.emit_pop_a();
    self.emit_sta_zpg(addr);
  }
  
  ////////////////////////////////////////////////////////
  /// Binary Emitters
  ////////////////////////////////////////////////////////
  fn emit_runtime(&mut self) {
    self.emit_label("_reset_vector");
    self.output.push(0x78);       // SEI, disable interrupts
    self.output.push(0xD8);       // CLD, clear decimal mode
    self.emit_ldx_imm(0xFF);      // get ready to setup hardware SP
    self.output.push(0x9A);       // TXS, move val in x reg to SP (hardware)
    
    // everything is initialized, start program
    self.emit_jsr("_main");
    
    // catch all if main returns
    self.emit_label("_halt");
    self.emit_jmp_abs("_halt");
  }
  
  fn pad_binary_and_emit_vectors(&mut self, mem_map: Memory_Map) {
    // pad to vectors
    let vector_offset = (0xFFFA - mem_map.rom_start) as usize;
    while self.output.len() < vector_offset {
      self.output.push(0xEA); // NOP as padding
    }
    
    // NMI - unused
    self.output.push(0x00);
    self.output.push(0x00);
    
    // RESET vector - points to _reset_vector
    let reset_addr = mem_map.rom_start + *self.labels.get("_reset_vector").unwrap() as u16;
    self.output.push((reset_addr & 0xFF) as u8);
    self.output.push((reset_addr >> 8) as u8);
    
    // IRQ - unused
    self.output.push(0x00);
    self.output.push(0x00);
  }
  
  /// Runs after the binary is generated, resolves label addresses
  fn resolve_patches(&mut self, rom_start: u16) {
    for (patch_offset, label) in &self.patches {
      let label_offset = self.labels.get(label)
      .expect(&format!("undefined label: {}", label));
      
      let absolute_addr = rom_start as usize + label_offset;
      dbg!(label, patch_offset, label_offset, absolute_addr);
      
      let lo = (absolute_addr & 0xFF) as u8;
      let hi = ((absolute_addr >> 8) & 0xFF) as u8;
      
      self.output[*patch_offset] = lo;
      self.output[*patch_offset + 1] = hi;
    }
  }
  
  // Function ABI (65c02 Hardware Stack - Page 1):
  // ---------------------------------------------------------------------------
  // CALLER:
  // 1. Pushes used scratch registers (if applicable)
  // 2. Pushes its current Frame Pointer (FP Low, then FP High)
  // 3. Pushes arguments left-to-right
  // 4. Calls function (JSR natively pushes the 2-byte return address to the stack)
  //
  // CALLEE (Prologue):
  // 1. Captures the hardware Stack Pointer (S) into X (TSX)
  // 2. Stores X into the Zero-Page Frame Pointer (STX FP)
  // 3. Sets FP+1 to $01 to enable Zero-Page Indirect Indexed addressing (FP),Y into Page 1
  //
  // STACK STATE at this point (Empty Descending):
  // SP+5: FP Low   <-- Pushed by caller
  // SP+4: FP High  <-- Pushed by caller
  // SP+3: Arg 1    <-- Pushed by caller (Arguments start here, Y=3)
  // SP+2: PCH      <-- Pushed natively by JSR
  // SP+1: PCL      <-- Pushed natively by JSR
  // SP  : [Empty]  <-- Hardware S points here. FP holds this value.
  //
  // CALLEE (Body):
  // 4. Uses (FP),Y with Y starting at 3 to copy args to fast Zero-Page registers
  // 5. Decrements hardware stack pointer (S) if space is needed for locals
  // 6. Executes function body, reading from ZP argument registers
  //
  // CALLEE (Epilogue):
  // 7. Cleanly restores hardware Stack Pointer directly from FP (LDX FP, TXS)
  // 8. Returns to caller (RTS)
  //
  // CALLER (Cleanup):
  // 9. Pops and discards the arguments from the stack
  // 10. Pops and restores its Frame Pointer (FP High, then FP Low)
  // 11. Pops and restores scratch registers
  // ---------------------------------------------------------------------------
  fn gen_function(&mut self, function: TopLevel) {
    if let TopLevel::Function(name, params, _ret_type, body) = function {
      self.emit_label(&format!("_{}", name));
      
      let is_main = name == "main";
      
      // main is the entry point and has no args so we don't need a prologue for it
      if !is_main {
        // prologue: FP = hardware S, FP+1 = 0x01 (Page 1)
        self.output.push(0xBA); // TSX
        self.output.push(0x86); self.output.push(FP); // STX FP
        self.emit_lda_imm(0x01);
        self.emit_sta_zpg(FP + 1);
      }
      
      // Hardware Stack offset starts at 3 because S+1 is RetLo and S+2 is RetHi
      let mut offset: u8 = 3;
      for (i, (param_type, param_name)) in params.iter().enumerate() {
        self.param_slots.insert(param_name.clone(), ARGS_START + (i * 2) as u8);
        
        self.emit_ldy_imm(offset);
        self.emit_lda_indirect_zpg(FP);
        self.emit_sta_zpg(ARGS_START + ((i * 2) as u8));
        
        offset += match param_type {
          Type::U16 | Type::I16 => 2,
          _ => 1,
        };
      }
      
      for stmt in body {
        self.gen_statement(stmt);
      }      
      
      if !is_main {
        // epilogue: cleanly restore hardware S from FP
        self.output.push(0xA6); self.output.push(FP); // LDX FP
        self.output.push(0x9A); // TXS
      }
      
      self.param_slots.clear();
      self.emit_rts();
    }
  }
  
  fn gen_statement(&mut self, stmt: Stmt) {
    match stmt {
      Stmt::Assign(name, expr) => {
        let address = if let Some(Symbol::Register { address, .. }) = self.global_symbols.local_symbols.get(&name) {
          Some(*address)
        } else {
          None
        };
        
        if let Some(addr) = address {
          match expr {
            Expr::Number(n) => self.emit_lda_imm(n as u8),
            Expr::Identifier(name) => {
              if let Some(addr) = self.param_slots.get(&name) {
                self.emit_lda_zpg(*addr);
              }
            }
            _ => todo!()
          }
          self.emit_sta_abs(addr);
        }
      }
      Stmt::Expr(expr) => self.gen_expression(expr),
      _ => {}
    }
  }
  
  fn gen_expression(&mut self, expr: Expr) {
    match expr {
      Expr::Call(name, args) => {
        let arg_count = args.len();
        
        // push FP (low byte then high byte)
        self.emit_lda_zpg(FP);
        self.emit_push_a();
        self.emit_lda_zpg(FP + 1);
        self.emit_push_a();
        
        // push each argument
        for arg in args {
          match arg {
            Expr::Number(n) => self.emit_push_imm(n as u8),
            _ => todo!()
          }
        }
        
        // JSR (Hardware stack tracks return addresses natively)
        self.emit_jsr(&format!("_{}", name));
        
        // clean up: pop args then pop FP
        for _ in 0..arg_count {
          self.emit_pop_zpg(REG_START); // pop and discard
        }
        
        // Stack is LIFO, pop high byte then low byte
        self.emit_pop_zpg(FP + 1);
        self.emit_pop_zpg(FP);
      }
      _ => {}
    }
  }
}

pub fn generate(ast: Vec<TopLevel>, symbol_table: SymbolTable, mem_map: Memory_Map) -> Vec<u8> {
  let mut generator = Generator::new(symbol_table);
  
  generator.emit_runtime();
  
  for item in ast {
    generator.gen_function(item);
  }
  
  dbg!(mem_map.rom_start);
  generator.resolve_patches(mem_map.rom_start);
  generator.pad_binary_and_emit_vectors(mem_map);
  
  generator.output
}

pub fn emit_binary<P: AsRef<Path>>(data: &[u8], path: P) -> io::Result<()> {
  let mut file = File::create(path)?;
  file.write_all(data)?;
  
  Ok(())
}