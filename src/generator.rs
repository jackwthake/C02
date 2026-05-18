use crate::parser::{TopLevel, Expr, Stmt, Op, Type};
use crate::analyzer::{SymbolTable, Symbol};

struct Generator {
  output: String,
  reg_depth: usize,
  label_count: usize,  // for unique if/while jump labels
  symbol_table: SymbolTable,
}

impl Generator {
  fn new(symbol_table: SymbolTable) -> Self {
    Self {
      output: String::new(),
      reg_depth: 0,
      label_count: 0,
      symbol_table,
    }
  }
  
  fn emit(&mut self, line: &str) {
    self.output.push_str(line);
    self.output.push('\n');
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
        // evaluate expr into r0
        self.reg_depth = 0;
        self.gen_expr(expr);

        // look up what name is in symbol table
        match self.symbol_table.local_symbols.get(name) {
          Some(Symbol::Register { address, .. }) => {
            let address = *address;
            self.emit(&format!("  LDA r0"));
            self.emit(&format!("  STA ${:04X}", address));
          }
          _ => todo!("non-register assign")
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
        match self.symbol_table.local_symbols.get(name) {
          Some(Symbol::Register { address, .. }) => {
            let address = *address;
            self.emit(&format!("  LDA ${:04X}", address));
            self.emit(&format!("  STA {}", reg));
            self.emit(&format!("  LDA #$00"));
            self.emit(&format!("  STA {}+1", reg));
          }
          _ => todo!("identifier expr")
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