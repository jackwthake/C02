
use crate::parser::{TopLevel, Expr, Stmt, Op, Type};
use crate::analyzer::{SymbolTable, Symbol};

use std::collections::HashMap;
use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct Memory_Map {
  soft_stack_start: u16, // start of software stack in main RAM
  rom_start: u16, // start of ROM where code is emitted
}

struct Generator {
  output: String,
  reg_depth: usize,
  label_count: usize,
  symbol_table: SymbolTable,
  local_offsets: HashMap<String, (u8, Type)>,  // name -> FP offset, type
  current_frame_size: u8,                      // grows as we encounter VarDecls
}

impl Generator {
  fn new(symbol_table: SymbolTable) -> Self {
    Self {
      output: String::new(),
      reg_depth: 0,
      label_count: 0,
      symbol_table,
      local_offsets: HashMap::new(),
      current_frame_size: 0,
    }
  }
  
  fn emit(&mut self, line: &str) {
    self.output.push_str(line);
    self.output.push('\n');
  }
  
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
      Type::U16 | Type::I16 => {
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
  
  fn emit_local_load(&mut self, name: &str, data_type: &Type) {
    let (offset, _t) = self.local_offsets.get(name)
    .map(|(offset, t)| (*offset, t.clone()))
    .expect("local_load called for undeclared local");
    let reg = format!("r{}", self.reg_depth);

    self.emit(&format!("; --- Load Local '{}' from offset ${:02X} ---", name, offset));
    
    match data_type {
      Type::U8 | Type::I8 => {
        self.emit(&format!("  LDY #${:02X}", offset));
        self.emit("  LDA (SP),Y");
        self.emit(&format!("  STA {}", reg));
      }
      Type::U16 | Type::I16 => {
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
  
  fn fresh_label(&mut self) -> String {
    let l = format!(".L{}", self.label_count);
    self.label_count += 1;
    l
  }
  
  fn gen_toplevel(&mut self, item: &TopLevel) {
    match item {
      TopLevel::RegDecl(_, _, _) => {
        // symbol table has everything we need, no code to emit
      }
      TopLevel::GlobalVar(t, name, initializer) => {
        // zero page allocation in usr_space — later
      }
      TopLevel::Function(name, params, ret, body) => {
        self.gen_function(name, params, ret, body);
      }
    }
  }

  fn calc_frame_size(&mut self, stmts: &[Stmt], curr_size: u8) -> u8 {
    let mut size = curr_size;
    for stmt in stmts {
      match stmt {
        Stmt::Assign(_, _) | Stmt::Return(_) => {} // no size impact
        Stmt::While(_, body) => {
          size = self.calc_frame_size(body, size)
        }

        Stmt::If(_, body, else_body) => {
          size = self.calc_frame_size(body, size);
          if let Some(else_stmts) = else_body {
            size = self.calc_frame_size(else_stmts, size);
          }
        }
        Stmt::VarDecl(data_type, var_name, _) => {
          self.local_offsets.insert(var_name.clone(), (size, data_type.clone()));
          size += match data_type {
            Type::U8 | Type::I8 => 1,
            Type::U16 | Type::I16 => 2,
            _ => 0,
          }
        }
      }
    }
    size
  }
  
  fn gen_function(&mut self, name: &str, params: &[(Type, String)], ret: &Type, body: &[Stmt]) {
    self.emit(&format!("_{}:", name));
    self.local_offsets.clear();
    self.current_frame_size = 0;
    
    // build variable offsets upwards starting exactly at 0
    let total_frame_size = self.calc_frame_size(body, 0);
    assert!(total_frame_size < 250, "Stack frame exceeded 250 bytes!");
    
    // generate function prologue for stack pointer if local variables are found
    if total_frame_size > 0 {
      self.emit("; --- Function Prologue ---");
      self.emit("  LDA SP");
      self.emit("  SEC");
      self.emit(&format!("  SBC #${:02X}", total_frame_size));
      self.emit("  STA SP");
      self.emit("  LDA SP+1");
      self.emit("  SBC #$00");
      self.emit("  STA SP+1");
      self.emit("; -------------------------\n");
    }
    
    // generate function body
    for stmt in body {
      self.gen_stmt(stmt);
    }
    
    // Stack Frame Clean-up
    // main never needs to clean up the stack because if main returns, execution stops
    if total_frame_size > 0 && !matches!(name, "main") {
      self.emit("\n; --- Function Epilogue ---");
      self.emit("  LDA SP");
      self.emit("  CLC");
      self.emit(&format!("  ADC #${:02X}", total_frame_size));
      self.emit("  STA SP");
      self.emit("  LDA SP+1");
      self.emit("  ADC #$00");
      self.emit("  STA SP+1");
      self.emit("; -------------------------");
    }
    
    self.emit("  RTS");
  }
  
  fn gen_stmt(&mut self, stmt: &Stmt) {
    match stmt {
      Stmt::Assign(name, expr) => {
        self.reg_depth = 0;
        self.gen_expr(expr);
        
        // check locals first — they aren't in the symbol table
        if let Some((_, data_type)) = self.local_offsets.get(name).cloned() {
          self.emit_local_store(name, &data_type);
        } else {
          let symbol = self.symbol_table.local_symbols.get(name).cloned();
          match symbol {
            Some(Symbol::Register { address, .. }) => {
              self.emit(&format!("; --- register write at addr: {:04X}", address));
              self.emit("  LDA r0");
              self.emit(&format!("  STA ${:04X}", address));
            }
            Some(Symbol::Variable { data_type }) => {
              todo!("global variable store")
            }
            _ => unreachable!("assign to undefined or function — analyzer should catch")
          }
        }
      }

      Stmt::Return(None) => {
        self.emit("  RTS");
      }

      Stmt::Return(Some(expr)) => {
        self.reg_depth = 0;
        self.gen_expr(expr);
        // return value convention — leave in r0
        self.emit("  RTS");
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
        todo!("if statement codegen")
      }

      Stmt::While(cond_expr, body_stmts) => {
        let start_label = self.fresh_label();
        let body_label = self.fresh_label();
        let end_label = self.fresh_label();

        self.emit("\n  ; --- While Loop");
        
        // loop start point
        self.emit(&format!("{}:", start_label));

        // evaluate condition (leaves 0x01 or 0x00 in r0)
        self.reg_depth = 0;
        self.gen_expr(cond_expr);

        // If r0 is NOT zero (true), jump straight into the body
        self.emit("  LDA r0");
        self.emit(&format!("  BNE {}", body_label));
        // Otherwise, execute a full 16-bit jump to escape the loop
        self.emit(&format!("  JMP {}", end_label));

        // loop Body
        self.emit(&format!("{}:", body_label));
        for stmt in body_stmts {
          self.gen_stmt(stmt);
        }

        // unconditional jump back to start
        self.emit(&format!("  JMP {}", start_label));

        // loop exit point
        self.emit(&format!("{}:", end_label));
      }
    }
  }
  
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
        
        // check locals first — they aren't in the symbol table
        if let Some((_, data_type)) = self.local_offsets.get(name).cloned() {
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
              todo!("global variable load")
            }
            Some(Symbol::Function { .. }) => unreachable!("function used as expression"),
            None => unreachable!("undefined identifier: {}", name),
          }
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
          _ => todo!("op")
        }
      }
      
      _ => todo!("expr")
    }
  }
}

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
  
  generator.output
}