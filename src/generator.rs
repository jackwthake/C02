use crate::parser::{TopLevel, Expr, Stmt, Op, Type};
use crate::analyzer::{SymbolTable, Symbol};

use std::collections::HashMap;

struct Generator {
  output: String,
  reg_depth: usize,
  label_count: usize,
  symbol_table: SymbolTable,
  local_offsets: HashMap<String, (u8, Type)>,  // name -> FP offset
  current_frame_size: u8,              // grows as we encounter VarDecls
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
    .map(|(offset, t)| (*offset, t.clone())) // Copy the integer, clone the Type
    .expect("local_store called for undeclared local");
    
    
    match data_type {
      Type::U8 | Type::I8 => {
        self.emit(&format!("  LDY #${:02X}", offset));
        self.emit("  LDA r0");
        self.emit("  STA (FP),Y");
      }
      Type::U16 | Type::I16 => {
        // low byte
        self.emit(&format!("  LDY #${:02X}", offset));
        self.emit("  LDA r0");
        self.emit("  STA (FP),Y");
        // high byte
        self.emit(&format!("  LDY #${:02X}", offset + 1));
        self.emit("  LDA r0+1");
        self.emit("  STA (FP),Y");
      }
      _ => unreachable!()
    }
  }
  
  fn emit_local_load(&mut self, name: &str, data_type: &Type) {
    let (offset, _t) = self.local_offsets.get(name)
    .map(|(offset, t)| (*offset, t.clone())) // Copy the integer, clone the Type
    .expect("local_load called for undeclared local");
    let reg = format!("r{}", self.reg_depth);
    
    match data_type {
      Type::U8 | Type::I8 => {
        self.emit(&format!("  LDY #${:02X}", offset));
        self.emit("  LDA (FP),Y");
        self.emit(&format!("  STA {}", reg));
        self.emit(&format!("  LDA #$00"));
        self.emit(&format!("  STA {}+1", reg));
      }
      Type::U16 | Type::I16 => {
        self.emit(&format!("  LDY #${:02X}", offset));
        self.emit("  LDA (FP),Y");
        self.emit(&format!("  STA {}", reg));
        self.emit(&format!("  LDY #${:02X}", offset + 1));
        self.emit("  LDA (FP),Y");
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
  
  fn gen_function(&mut self, name: &str, params: &[(Type, String)], ret: &Type, body: &[Stmt]) {
    self.emit(&format!("_{}:", name));
    
    // pre-scan to calculate frame size
    let frame_size = self.calc_frame_size(body);
    
    if frame_size > 0 {
      // adjust FP down so locals don't overflow into ROM
      self.emit(&format!("  LDA FP"));
      self.emit(&format!("  SEC"));
      self.emit(&format!("  SBC #${:02X}", frame_size));
      self.emit(&format!("  STA FP"));
      self.emit(&format!("  LDA FP+1"));
      self.emit(&format!("  SBC #$00"));
      self.emit(&format!("  STA FP+1"));
    }
    
    for stmt in body {
      self.gen_stmt(stmt);
    }
  }
  
  fn calc_frame_size(&self, body: &[Stmt]) -> u8 {
    let mut size = 0u8;
    for stmt in body {
      if let Stmt::VarDecl(data_type, _, _) = stmt {
        match data_type {
          Type::U8 | Type::I8 => size += 1,
          Type::U16 | Type::I16 => size += 2,
          _ => {}
        }
      }
    }
    size
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
        // allocate space on software stack
        let offset = self.current_frame_size;
        self.local_offsets.insert(name.clone(), (offset, data_type.clone()));
        
        // grow frame by type width
        match data_type {
          Type::U8 | Type::I8 => self.current_frame_size += 1,
          Type::U16 | Type::I16 => self.current_frame_size += 2,
          _ => unreachable!()
        }
        
        // if there's an initializer evaluate and store it
        if let Some(expr) = initializer {
          self.reg_depth = 0;
          self.gen_expr(expr);
          self.emit_local_store(name, data_type);
        }
      }
      _ => todo!("stmt")
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
            self.emit("  CLC");
            self.emit(&format!("  LDA {}", reg));
            self.emit(&format!("  ADC {}", right_reg));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  LDA {}+1", reg));
            self.emit(&format!("  ADC {}+1", right_reg));
            self.emit(&format!("  STA {}+1", reg));
          }
          Op::Minus => {
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

pub fn generate(ast: Vec<TopLevel>, symbol_table: SymbolTable) -> String {
  let mut generator = Generator::new(symbol_table);
  
  generator.emit(include_str!("../c02rt/reg.s"));
  generator.emit(include_str!("../c02rt/c02_vectors.s"));
  generator.emit(include_str!("../c02rt/c02rt0.s"));
  
  for item in &ast {
    generator.gen_toplevel(item);
  }
  
  generator.output
}