use crate::parser::{TopLevel, Expr, Stmt, Op, Type};
use crate::analyzer::{SymbolTable, Symbol};

use std::collections::HashMap;

struct Generator {
  output: String,
  reg_depth: usize,
  label_count: usize,
  symbol_table: SymbolTable,
  local_offsets: HashMap<String, u8>,  // name -> FP offset
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
    let offset = *self.local_offsets.get(name)
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
    let offset = *self.local_offsets.get(name)
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
    // emit the label
    self.emit(&format!("_{}:", name));
    
    for stmt in body {
      self.gen_stmt(stmt);
    }
  }
  
  fn gen_stmt(&mut self, stmt: &Stmt) {
    match stmt {
      Stmt::Assign(name, expr) => {
        self.reg_depth = 0;
        self.gen_expr(expr);
        
        let address = match self.symbol_table.local_symbols.get(name) {
          Some(Symbol::Register { address, .. }) => *address,
          _ => todo!("non-register assign"),
        };
        
        self.emit("  LDA r0");
        self.emit(&format!("  STA ${:04X}", address));
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
        self.local_offsets.insert(name.clone(), offset);
        
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
        self.emit(&format!("  LDA #<{}", n));
        self.emit(&format!("  STA {}", reg));
        self.emit(&format!("  LDA #>{}", n));
        self.emit(&format!("  STA {}+1", reg));
      }
      Expr::Identifier(name) => {
        let reg = format!("r{}", self.reg_depth);
        let symbol = self.symbol_table.local_symbols.get(name);

        match symbol {
          Some(Symbol::Register { address, data_type }) => {
            let address = *address;
            let data_type = data_type.clone();
            let _ = symbol;
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
            let data_type = data_type.clone();
            let is_local = self.local_offsets.contains_key(name);
            let _ = symbol;
            if is_local {
              self.emit_local_load(name, &data_type);
            } else {
              todo!("global variable load")
            }
          }
          Some(Symbol::Function { .. }) => unreachable!("function used as expression"),
          None => unreachable!("undefined identifier"),
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