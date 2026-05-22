use std::collections::HashMap;

use crate::parser::{Type, Expr, Stmt, TopLevel, Op};
use crate::generator::HELPER_FUNCTIONS;

#[derive(Clone, Debug)]
pub enum Symbol {
  Variable { data_type: Type },
  Register { data_type: Type, address: u16 },
  Function { params: Vec<(Type, String)>, return_type: Type },
}

#[derive(Clone, Debug)]
pub struct SymbolTable {
  // Current block's variables
  pub local_symbols: HashMap<String, Symbol>,
  // Optional link to the outer enclosing scope
  parent: Option<Box<SymbolTable>>,
}

impl SymbolTable {
  // Create a brand new global symbol table
  pub fn new() -> Self {
    Self {
      local_symbols: HashMap::new(),
      parent: None,
    }
  }
  
  // Create a new child scope (e.g., entering an IF statement or Function)
  // It takes ownership of the current table to become its parent
  pub fn enter_scope(self) -> Self {
    Self {
      local_symbols: HashMap::new(),
      parent: Some(Box::new(self)),
    }
  }
  
  // Exit the current scope and return the parent scope back
  pub fn exit_scope(self) -> Result<Self, String> {
    match self.parent {
      Some(parent_table) => Ok(*parent_table),
      None => Err("Compiler error: Attempted to exit global scope!".to_string()),
    }
  }
  
  // Insert a variable into the CURRENT block only
  pub fn insert(&mut self, name: String, symbol: Symbol) -> Result<(), String> {
    if self.local_symbols.contains_key(&name) {
      return Err(format!("Redeclaration error: '{}' already defined in this block.", name));
    }
    self.local_symbols.insert(name, symbol);
    Ok(())
  }
  
  // Recursive lookup: Check current block, then ask parent
  pub fn lookup(&self, name: &str) -> Option<&Symbol> {
    if let Some(symbol) = self.local_symbols.get(name) {
      Some(symbol)
    } else if let Some(ref parent_table) = self.parent {
      parent_table.lookup(name) // Delegate to parent scope
    } else {
      None // Variable does not exist anywhere
    }
  }
  
  // Helper: Get the type of a symbol by name
  pub fn get_type(&self, name: &str) -> Option<Type> {
    match self.lookup(name) {
      Some(Symbol::Variable { data_type }) => Some(data_type.clone()),
      Some(Symbol::Register { data_type, .. }) => Some(data_type.clone()),
      Some(Symbol::Function { .. }) | None => None,
    }
  }
}

/// Check if two types are compatible (for assignment, return, etc.)
fn types_compatible(expected: &Type, actual: &Type) -> bool {
  match (expected, actual) {
    // Same types are always compatible
    (Type::U8, Type::U8) => true,
    (Type::I8, Type::I8) => true,
    (Type::U16, Type::U16) => true,
    (Type::I16, Type::I16) => true,
    (Type::Void, Type::Void) => true,
    (Type::Ptr(exp_inner), Type::Ptr(act_inner)) => types_compatible(exp_inner, act_inner),
    _ => false, // No implicit conversions between different types
  }
}

/// Check if an expression type can be assigned to a target type
/// This is more lenient than types_compatible: numeric literals can be assigned to any integer type
fn expr_assignable_to(expected: &Type, expr: &Expr, table: &SymbolTable) -> Result<bool, String> {
  match expr {
    Expr::Number(_) => {
      match expected {
        Type::U8 | Type::I8 | Type::U16 | Type::I16 => Ok(true),
        _ => Ok(false),
      }
    }
    // If the expression is a binary operation on two numbers, allow it to assign to a u8
    Expr::BinOp(left, _, right) if matches!(left.as_ref(), Expr::Number(_)) && matches!(right.as_ref(), Expr::Number(_)) => {
      match expected {
        Type::U8 | Type::I8 | Type::U16 | Type::I16 => Ok(true),
        _ => Ok(false),
      }
    }
    _ => {
      let expr_type = infer_expr_type(expr, table)?;
      Ok(types_compatible(expected, &expr_type))
    }
  }
}

/// Infer the type of an expression
fn infer_expr_type(expr: &Expr, table: &SymbolTable) -> Result<Type, String> {
  match expr {
    Expr::Number(num) => {
      if (*num >= 0 && *num <= 255) || (*num < 0 && *num >= -128) {
        Ok(Type::U8) // We can treat all small literals as u8 for simplicity
      } else if (*num > 255 && *num <= 65535) || (*num < -128 && *num >= -32768) {
        Ok(Type::U16) // Larger literals can be treated as u16
      } else {
        Err(format!("Numeric literal out of range: {}", num))
      }
    }
    Expr::Identifier(name) => {
      table.get_type(name).ok_or_else(|| format!("Undeclared identifier: '{}'.", name))
    }
    Expr::BinOp(left, op, right) => {
      let left_type = infer_expr_type(left, table)?;
      let right_type = infer_expr_type(right, table)?;
      
      // For binary operations, both operands must be compatible
      // Allow numeric literals to work with any integer type
      let left_ok = match left.as_ref() {
        Expr::Number(_) => matches!(left_type, Type::U8 | Type::I8 | Type::U16 | Type::I16),
        _ => true,
      };
      let right_ok = match right.as_ref() {
        Expr::Number(_) => matches!(right_type, Type::U8 | Type::I8 | Type::U16 | Type::I16),
        _ => true,
      };
      
      if !left_ok || !right_ok || !types_compatible(&left_type, &right_type) {
        // Try to find the "dominant" type for mixed numeric literal operations
        let is_left_literal = matches!(left.as_ref(), Expr::Number(_));
        let is_right_literal = matches!(right.as_ref(), Expr::Number(_));
        
        if is_left_literal && is_right_literal {
          // Both numeric literals - fine, result is whatever type they evaluate to
        } else if is_left_literal {
          // Left is literal, right is not - this is ok, result type is right_type
        } else if is_right_literal {
          // Right is literal, left is not - this is ok, result type is left_type  
        } else {
          // Neither is a literal and types don't match - error
          return Err(format!(
            "Type mismatch in binary operation: left operand is {:?}, right operand is {:?}",
            left_type, right_type
          ));
        }
      }
      
      // Comparison operators return bool-like (we represent as u8 for 0/1)
      match op {
        Op::Lt | Op::Gt | Op::Lte | Op::Gte | Op::EqualsEquals | Op::BangEquals => Ok(Type::U8),
        // Arithmetic operators preserve the dominant operand type
        Op::Plus | Op::Minus | Op::Multiply | Op::Divide => {
          // If one is a literal, use the other's type; otherwise use left
          if matches!(left.as_ref(), Expr::Number(_)) && !matches!(right.as_ref(), Expr::Number(_)) {
            Ok(right_type)
          } else {
            Ok(left_type)
          }
        }
        _ => Err(format!("Invalid binary operator in expression")),
      }
    }
    Expr::Unary(op, operand) => {
      let operand_type = infer_expr_type(operand, table)?;
      match op {
        Op::Bang => Ok(Type::U8),
        Op::Negate => Ok(operand_type),
        Op::AddressOf => Ok(Type::Ptr(Box::new(operand_type))),  // add this
        _ => Err(format!("Invalid unary operator in expression")),
      }
    }
    Expr::Call(fn_name, _args) => {
      // Look up the function to get its return type
      match table.lookup(fn_name) {
        Some(Symbol::Function { return_type, .. }) => Ok(return_type.clone()),
        Some(_) => Err(format!("'{}' is not a function.", fn_name)),
        None => {
          // check if its a std library function (e.g., __mul16, __div16, memcpy, strcpy)
          if let Some((_, _, _, return_type)) = HELPER_FUNCTIONS.iter().find(|(name, _, _, _)| *name == fn_name) {
            Ok(return_type.clone())
          } else {
            Err(format!("Undeclared function: '{}'.", fn_name))
          }
        },
      }
    }
    Expr::Deref(expr) => {
      let expr_type = infer_expr_type(expr, table)?;
      match expr_type {
        Type::Ptr(inner) => Ok(*inner),
        _ => Err(format!("Cannot dereference non-pointer type {:?}", expr_type)),
      }
    }
    Expr::Cast(target_type, _expr) => {
      // Cast always results in the target type
      Ok(target_type.clone())
    }
  }
}

/// Entry point for semantic analysis
pub fn analyze(ast: &[TopLevel]) -> Result<SymbolTable, String> {
  let mut global_table = SymbolTable::new();
  
  // First pass: register all global declarations (functions and global variables)
  for item in ast {
    analyze_toplevel_decl(item, &mut global_table)?;
  }
  
  // Second pass: validate function bodies
  for item in ast {
    analyze_toplevel_body(item, &mut global_table)?;
  }
  
  Ok(global_table)
}

/// First pass: register declarations at global scope
fn analyze_toplevel_decl(toplevel: &TopLevel, table: &mut SymbolTable) -> Result<(), String> {
  match toplevel {
    TopLevel::Function(name, params, return_type, _body) => {
      let symbol = Symbol::Function {
        params: params.clone(),
        return_type: return_type.clone(),
      };
      table.insert(name.clone(), symbol)?;
      Ok(())
    }
    TopLevel::RegDecl(data_type, name, address) => {
      let symbol = Symbol::Register {
        data_type: data_type.clone(),
        address: *address,
      };
      table.insert(name.clone(), symbol)?;
      Ok(())
    }
    TopLevel::GlobalVar(data_type, name, _init_expr) => {
      let symbol = Symbol::Variable {
        data_type: data_type.clone(),
      };
      table.insert(name.clone(), symbol)?;
      Ok(())
    }
  }
}

/// Second pass: validate function bodies and variable initializers
fn analyze_toplevel_body(toplevel: &TopLevel, table: &mut SymbolTable) -> Result<(), String> {
  match toplevel {
    TopLevel::Function(_name, params, return_type, body) => {
      // Enter function scope
      let mut func_table = table.clone().enter_scope();
      
      // Register parameters in function scope
      for (param_type, param_name) in params {
        let symbol = Symbol::Variable {
          data_type: param_type.clone(),
        };
        func_table.insert(param_name.clone(), symbol)?;
      }
      
      // Analyze function body with return type tracking
      analyze_statements(body, &mut func_table, Some(return_type.clone()))?;
      
      // Exit function scope
      let _exited_table = func_table.exit_scope()?;
      Ok(())
    }
    TopLevel::RegDecl(_data_type, _name, _address) => {
      // Register declarations have no body to analyze
      Ok(())
    }
    TopLevel::GlobalVar(data_type, _name, Some(init_expr)) => {
      // Verify that initializer expression type matches the variable type
      let is_assignable = expr_assignable_to(data_type, init_expr, table)?;
      if !is_assignable {
        let init_type = infer_expr_type(init_expr, table)?;
        return Err(format!(
          "Type mismatch in global variable initializer: expected {:?}, found {:?}",
          data_type, init_type
        ));
      }
      Ok(())
    }
    TopLevel::GlobalVar(_data_type, _name, None) => {
      // No initializer, nothing to analyze
      Ok(())
    }
  }
}

/// Analyze a block of statements
fn analyze_statements(stmts: &[Stmt], table: &mut SymbolTable, return_type: Option<Type>) -> Result<(), String> {
  for stmt in stmts {
    analyze_stmt(stmt, table, return_type.clone())?;
  }
  Ok(())
}

/// Analyze a single statement
fn analyze_stmt(stmt: &Stmt, table: &mut SymbolTable, return_type: Option<Type>) -> Result<(), String> {
  match stmt {
    Stmt::VarDecl(data_type, name, init_expr) => {
      analyze_var_decl(data_type, name, init_expr, table)?;
      Ok(())
    }
    Stmt::Assign(name, expr) => {
      analyze_assign(name, expr, table)?;
      Ok(())
    }
    Stmt::DerefAssign(ptr_expr, value_expr) => {
      analyze_expr(ptr_expr, table)?;
      let ptr_type = infer_expr_type(ptr_expr, table)?;
      let inner_type = match ptr_type {
        Type::Ptr(inner) => *inner,
        _ => return Err(format!("Cannot assign through non-pointer type {:?}", ptr_type)),
      };
      analyze_expr(value_expr, table)?;
      let is_assignable = expr_assignable_to(&inner_type, value_expr, table)?;
      if !is_assignable {
        let value_type = infer_expr_type(value_expr, table)?;
        return Err(format!(
          "Type mismatch in pointer assignment: pointer points to {:?}, but expression is {:?}",
          inner_type, value_type
        ));
      }
      Ok(())
    }
    Stmt::Expr(expr) => {
      analyze_expr(expr, table)?;
      Ok(())
    }
    Stmt::Return(expr) => {
      analyze_return(expr, return_type, table)?;
      Ok(())
    }
    Stmt::If(cond_expr, then_stmts, else_stmts) => {
      analyze_if(cond_expr, then_stmts, else_stmts, table, return_type)?;
      Ok(())
    }
    Stmt::While(cond_expr, body_stmts) => {
      analyze_while(cond_expr, body_stmts, table, return_type)?;
      Ok(())
    }
  }
}

/// Analyze a variable declaration
fn analyze_var_decl(
  data_type: &Type,
  name: &str,
  init_expr: &Option<Expr>,
  table: &mut SymbolTable,
) -> Result<(), String> {
  // If there's an initializer, check type compatibility
  if let Some(expr) = init_expr {
    // Validate the expression (check for undeclared identifiers, wrong function calls, etc.)
    analyze_expr(expr, table)?;
    // Check type compatibility
    let is_assignable = expr_assignable_to(data_type, expr, table)?;
    if !is_assignable {
      let expr_type = infer_expr_type(expr, table)?;
      return Err(format!(
        "Type mismatch in variable '{}': declared as {:?}, but initializer is {:?}",
        name, data_type, expr_type
      ));
    }
  }
  
  // Register the variable in current scope
  let symbol = Symbol::Variable {
    data_type: data_type.clone(),
  };
  table.insert(name.to_string(), symbol)?;
  Ok(())
}

/// Analyze an assignment statement
fn analyze_assign(name: &str, expr: &Expr, table: &SymbolTable) -> Result<(), String> {
  // Verify target variable exists
  let target_type = table.get_type(name).ok_or_else(|| {
    format!("Undeclared identifier: '{}' in assignment.", name)
  })?;
  
  // Validate the expression (check for undeclared identifiers, wrong function calls, etc.)
  analyze_expr(expr, table)?;
  
  // Check if expression is assignable to the target type
  let is_assignable = expr_assignable_to(&target_type, expr, table)?;
  if !is_assignable {
    let expr_type = infer_expr_type(expr, table)?;
    return Err(format!(
      "Type mismatch in assignment to '{}': target is {:?}, but expression is {:?}",
      name, target_type, expr_type
    ));
  }
  
  Ok(())
}

/// Analyze a return statement
fn analyze_return(expr: &Option<Expr>, return_type: Option<Type>, table: &SymbolTable) -> Result<(), String> {
  match (expr, &return_type) {
    (Some(_), Some(Type::Void)) => {
      Err("Type mismatch in return statement: void function cannot return a value".to_string())
    }
    (None, Some(Type::Void)) => {
      // Returning from a void function without a value is correct
      Ok(())
    }
    (None, Some(expected)) => {
      Err(format!(
        "Type mismatch in return statement: function returns {:?}, but no value was returned",
        expected
      ))
    }
    (Some(e), Some(expected)) => {
      let expr_type = infer_expr_type(e, table)?;
      if !types_compatible(expected, &expr_type) {
        return Err(format!(
          "Type mismatch in return statement: function returns {:?}, but expression is {:?}",
          expected, expr_type
        ));
      }
      Ok(())
    }
    // If not in a function (return_type is None), return statements shouldn't occur but we'll allow them
    (_, None) => Ok(()),
  }
}

/// Analyze an if statement
fn analyze_if(
  cond_expr: &Expr,
  then_stmts: &[Stmt],
  else_stmts: &Option<Vec<Stmt>>,
  table: &mut SymbolTable,
  return_type: Option<Type>,
) -> Result<(), String> {
  // Analyze condition expression - should be able to be used in a boolean context
  let _cond_type = infer_expr_type(cond_expr, table)?;
  // Any type can be used in a condition (the code generator will handle it)
  
  // Analyze then-block in a new scope
  let mut then_table = table.clone().enter_scope();
  analyze_statements(then_stmts, &mut then_table, return_type.clone())?;
  let _exited_then = then_table.exit_scope()?;
  
  // Analyze else-block (if present) in a new scope
  if let Some(else_block) = else_stmts {
    let mut else_table = table.clone().enter_scope();
    analyze_statements(else_block, &mut else_table, return_type)?;
    let _exited_else = else_table.exit_scope()?;
  }
  
  Ok(())
}

/// Analyze a while loop
fn analyze_while(cond_expr: &Expr, body_stmts: &[Stmt], table: &mut SymbolTable, return_type: Option<Type>) -> Result<(), String> {
  // Analyze condition expression
  let _cond_type = infer_expr_type(cond_expr, table)?;
  
  // Analyze body in a new scope
  let mut body_table = table.clone().enter_scope();
  analyze_statements(body_stmts, &mut body_table, return_type)?;
  let _exited_body = body_table.exit_scope()?;
  
  Ok(())
}

/// Analyze an expression (validates it and checks for undeclared identifiers)
fn analyze_expr(expr: &Expr, table: &SymbolTable) -> Result<(), String> {
  match expr {
    Expr::Number(_) => {
      // Numeric literals are always valid
      Ok(())
    }
    Expr::Identifier(name) => {
      // Verify identifier exists
      if table.lookup(name).is_none() {
        return Err(format!("Undeclared identifier: '{}'.", name));
      }
      Ok(())
    }
    Expr::BinOp(left, _op, right) => {
      // Recursively check both operands
      analyze_expr(left, table)?;
      analyze_expr(right, table)?;
      Ok(())
    }
    Expr::Unary(op, expr) => {
      if matches!(op, Op::AddressOf) {
        if !matches!(expr.as_ref(), Expr::Identifier(_) | Expr::Deref(_)) {
          return Err("Cannot take address of a non-lvalue expression".to_string());
        }
      }
      analyze_expr(expr, table)?;
      Ok(())
    }
    Expr::Call(fn_name, args) => {
      // Verify function exists and check argument types
      match table.lookup(fn_name) {
        Some(Symbol::Function { params, .. }) => {
          // Check argument count matches parameter count
          if args.len() != params.len() {
            return Err(format!(
              "Function '{}' expects {} argument(s), but {} were provided",
              fn_name,
              params.len(),
              args.len()
            ));
          }
          
          // Check each argument type matches the corresponding parameter type
          for (i, (arg, (param_type, _param_name))) in args.iter().zip(params.iter()).enumerate() {
            let arg_type = infer_expr_type(arg, table)?;
            if !types_compatible(param_type, &arg_type) {
              return Err(format!(
                "Type mismatch in argument {} of function '{}': expected {:?}, found {:?}",
                i + 1, fn_name, param_type, arg_type
              ));
            }
          }
          Ok(())
        }
        Some(_) => Err(format!("'{}' is not a function.", fn_name)),
        None => {
          // check if its a std library function (e.g., __mul16, __div16, memcpy, strcpy)
          if let Some((_, _, arg_count, _)) = HELPER_FUNCTIONS.iter().find(|(name, _, _, _)| *name == fn_name) {
            if args.len() != *arg_count {
              return Err(format!(
                "Function '{}' expects {} argument(s), but {} were provided",
                fn_name,
                arg_count,
                args.len()
              ));
            }
            Ok(())
          } else {
            Err(format!("Undeclared function: '{}'.", fn_name))
          }
        }
      }
    }
    Expr::Deref(expr) => {
      // Recursively check the dereferenced expression
      analyze_expr(expr, table)?;
      Ok(())
    }
    Expr::Cast(_target_type, expr) => {
      // Recursively check the casted expression
      analyze_expr(expr, table)?;
      Ok(())
    }
  }
}