#![allow(unused)] // <-- yuck!

use crate::tokenizer::Token;
use std::iter::Peekable;

#[derive(Debug)]
pub enum Type {
  U8,
  I8,
  U16,
  I16,
  Void,
  Ptr(Box<Type>),  // pointer to any type, e.g. Ptr(U8) = u8*
}

#[derive(Debug)]
pub enum Op {
  Plus,
  Minus,
  Multiply,
  Divide,
  Lt,
  Gt,
  Lte,
  Gte,
  EqualsEquals,
  BangEquals,
  Bang,
  Negate
}

#[derive(Debug)]
pub enum Expr {
  Number(i64),
  Identifier(String),
  BinOp(Box<Expr>, Op, Box<Expr>),
  Unary(Op, Box<Expr>),
  Call(String, Vec<Expr>),
  Deref(Box<Expr>),
  Cast(Type, Box<Expr>),
  Index(Box<Expr>, Box<Expr>),
}

#[derive(Debug)]
pub enum Stmt {
  VarDecl(Type, String, Option<Expr>),
  Assign(String, Expr),
  Return(Option<Expr>),
  If(Expr, Vec<Stmt>, Option<Vec<Stmt>>),
  While(Expr, Vec<Stmt>),
  ExprStmt(Expr),
}

#[derive(Debug)]
pub enum TopLevel {
  Function(String, Vec<(Type, String)>, Type, Vec<Stmt>),
  RegDecl(Type, String, u16),
  GlobalVar(Type, String, Option<Expr>),
}

////////////////////////////////////////////////////////////////////
/// Recursive descent
////////////////////////////////////////////////////////////////////

// bottom of recursive descent hierarchy, consumes tokens
fn primary(iter: &mut Peekable<impl Iterator<Item=Token>>) -> Expr {
  match iter.next() {
    Some(Token::l_num(n)) => Expr::Number(n),
    
    Some(Token::l_identifier(name)) => {
      if iter.peek() == Some(&Token::s_lparen) {
        iter.next(); // consume (
        let mut args = Vec::new();
        loop {
          match iter.peek() {
            Some(Token::s_rparen) => {
              iter.next(); // consume )
              break;
            }
            Some(Token::s_comma) => {
              iter.next(); // consume ,
            }
            _ => args.push(equality(iter)),
          }
        }
        Expr::Call(name, args)
      } else {
        Expr::Identifier(name)
      }
    }

    Some(Token::s_lparen) => {
      // grouped expression like (a + b)
      let expr = equality(iter);
      iter.next(); // consume )
      expr
    }
    
    _ => panic!("unexpected token in primary"), // error handling later
  }
}

fn unary(iter: &mut Peekable<impl Iterator<Item=Token>>) -> Expr {
  match iter.peek() {
    Some(Token::s_bang) => {
      iter.next();
      let operand = unary(iter); // recursive, handles !!x
      Expr::Unary(Op::Bang, Box::new(operand))
    }
    
    Some(Token::s_minus) => {
      iter.next();
      let operand = unary(iter);
      Expr::Unary(Op::Negate, Box::new(operand))
    }
    
    Some(Token::s_star) | Some(Token::s_mem_lookup) => {
      iter.next();
      let operand = unary(iter);
      Expr::Deref(Box::new(operand))
    }
    _ => primary(iter), // no unary operator, fall through
  }
}

fn factor(iter: &mut Peekable<impl Iterator<Item=Token>>) -> Expr {
  let mut left = unary(iter);
  
  loop {
    match iter.peek() {
      Some(Token::s_star) => {
        iter.next();
        let right = unary(iter);
        left = Expr::BinOp(Box::new(left), Op::Multiply, Box::new(right));
      }
      Some(Token::s_divide) => {
        iter.next();
        let right = unary(iter);
        left = Expr::BinOp(Box::new(left), Op::Divide, Box::new(right));
      }
      _ => break,
    }
  }
  
  left
}

fn term(iter: &mut Peekable<impl Iterator<Item=Token>>) -> Expr {
  let mut left = factor(iter);
  
  loop {
    match iter.peek() {
      Some(Token::s_plus) => {
        iter.next();
        let right = factor(iter);
        left = Expr::BinOp(Box::new(left), Op::Plus, Box::new(right));
      }
      Some(Token::s_minus) => {
        iter.next();
        let right = factor(iter);
        left = Expr::BinOp(Box::new(left), Op::Minus, Box::new(right));
      }
      _ => break,
    }
  }
  
  left
}

fn comparison(iter: &mut Peekable<impl Iterator<Item=Token>>) -> Expr {
  let mut left = term(iter);
  
  loop {
    match iter.peek() {
      Some(Token::s_lt) => {
        iter.next();
        let right = term(iter);
        left = Expr::BinOp(Box::new(left), Op::Lt, Box::new(right));
      }
      
      Some(Token::s_gt) => {
        iter.next();
        let right = term(iter);
        left = Expr::BinOp(Box::new(left), Op::Gt, Box::new(right));
      }
      
      Some(Token::s_lte) => {
        iter.next();
        let right = term(iter);
        left = Expr::BinOp(Box::new(left), Op::Lte, Box::new(right));
      }
      
      Some(Token::s_gte) => {
        iter.next();
        let right = term(iter);
        left = Expr::BinOp(Box::new(left), Op::Gte, Box::new(right));
      }
      _ => break,
    }
  }
  
  left
}

// root of recursive descent
// equality    looks for == !=      calls comparison
// comparison  looks for < > <= >=  calls term
// term        looks for + -        calls factor
// factor      looks for * /        calls unary
// unary       looks for * @ ! -    calls primary
// primary     looks for literals, identifiers, (expr)   consumes tokens
fn equality(iter: &mut Peekable<impl Iterator<Item=Token>>) -> Expr {
  let mut left = comparison(iter);
  
  loop {
    match iter.peek() {
      Some(Token::s_equalsequals) => {
        iter.next(); // consume ==
        let right = comparison(iter);
        left = Expr::BinOp(Box::new(left), Op::EqualsEquals, Box::new(right));
      }
      Some(Token::s_bang_equals) => {
        iter.next();
        let right = comparison(iter);
        left = Expr::BinOp(Box::new(left), Op::BangEquals, Box::new(right));
      }
      _ => break,
    }
  }
  
  left
}


/////////////////////////////////////////////////////////////////////////////
/// Main Parsing
/////////////////////////////////////////////////////////////////////////////

fn parse_block(iter: &mut Peekable<impl Iterator<Item=Token>>) -> Vec<Stmt> {
  iter.next(); // consume {
  let mut stmts = Vec::new();
  loop {
    match iter.peek() {
      Some(Token::s_rbrace) => {
        iter.next(); // consume }
        break;
      }
      None => panic!("unexpected end of file, expected }}"),
      _ => stmts.push(parse_stmt(iter)),
    }
  }
  stmts
}

fn parse_stmt(iter: &mut Peekable<impl Iterator<Item=Token>>) -> Stmt {
  match iter.peek() {
    Some(Token::Kw_return) => {
      iter.next(); // consume return
      let expr = equality(iter);
      iter.next(); // consume ;
      Stmt::Return(Some(expr))
    }
    
    Some(Token::Kw_while) => {
      iter.next(); // consume while
      iter.next(); // consume (
      let condition = equality(iter);
      iter.next(); // consume )
      let body = parse_block(iter);
      Stmt::While(condition, body)
    }
    
    Some(Token::Kw_if) => {
      iter.next(); // consume if
      let condition = equality(iter);
      let body = parse_block(iter);
      // check for else
      let else_body = if iter.peek() == Some(&Token::Kw_else) {
        iter.next(); // consume else
        Some(parse_block(iter))
      } else {
        None
      };
      Stmt::If(condition, body, else_body)
    }
    
    Some(Token::t_u8) | Some(Token::t_u16) | 
    Some(Token::t_i8) | Some(Token::t_i16) | Some(Token::Kw_void) => {
      let mut t = match iter.next() {
        Some(Token::t_u8) => Type::U8,
        Some(Token::t_i8) => Type::I8,
        Some(Token::t_u16) => Type::U16,
        Some(Token::t_i16) => Type::I16,
        Some(Token::Kw_void) => Type::Void,
        _ => panic!("expected type"),
      };
      
      // check for pointer
      if iter.peek() == Some(&Token::s_star) {
        iter.next();
        t = Type::Ptr(Box::new(t));
      }
      
      let identifier = match iter.next() {
        Some(Token::l_identifier(name)) => name,
        _ => panic!("expected identifier"),
      };
      
      iter.next(); // consume =
      let expr = equality(iter);
      iter.next(); // consume ;
      Stmt::VarDecl(t, identifier, Some(expr))
    }
    
    Some(Token::l_identifier(_)) => {
      let id = match iter.next().unwrap() {
        Token::l_identifier(name) => name,
        _ => unreachable!(),
      };
      
      match iter.next() {
        Some(Token::s_equals) => {
          let expr = equality(iter);
          iter.next(); // consume ;
          Stmt::Assign(id.clone(), expr)
        }
        Some(Token::s_plus_equals) => {
          let expr = equality(iter);
          iter.next(); // consume ;
          Stmt::Assign(id.clone(), 
          Expr::BinOp(
            Box::new(Expr::Identifier(id)),
            Op::Plus,
            Box::new(expr)
          ))
        }
        Some(Token::s_minus_equals) => {
          let expr = equality(iter);
          iter.next(); // consume ;
          Stmt::Assign(id.clone(),
          Expr::BinOp(
            Box::new(Expr::Identifier(id)),
            Op::Minus,
            Box::new(expr)
          ))
        }
        _ => panic!("expected = += or -= after identifier"),
      }
    }    
    _ => {
      todo!();
    }
  }
}

fn parse_args(iter: &mut Peekable<impl Iterator<Item=Token>>) -> Vec<(Type, String)> {
  let mut args = Vec::new();
  iter.next(); // consume (
  
  loop {
    match iter.peek() {
      Some(Token::s_rparen) => {
        iter.next(); // consume )
        break;
      }
      Some(Token::s_comma) => {
        iter.next(); // consume ,
      }
      _ => {
        let t = match iter.next() {
          Some(Token::t_u8) => Type::U8,
          Some(Token::t_i8) => Type::I8,
          Some(Token::t_u16) => Type::U16,
          Some(Token::t_i16) => Type::I16,
          _ => panic!("expected type in argument list"),
        };
        let name = match iter.next() {
          Some(Token::l_identifier(n)) => n,
          _ => panic!("expected identifier in argument list"),
        };
        args.push((t, name));
      }
    }
  }
  args
}

fn parse_toplevel(iter: &mut Peekable<impl Iterator<Item=Token>>) -> TopLevel {
  match iter.peek() {
    Some(Token::Kw_fn) => {
      iter.next(); // consume fn
      let name = match iter.next() {
        Some(Token::l_identifier(n)) => n,
        _ => panic!("expected function name"),
      };
      let args = parse_args(iter);
      // consume ->
      iter.next();
      // parse return type
      let ret = match iter.next() {
        Some(Token::t_u8) => Type::U8,
        Some(Token::t_u16) => Type::U16,
        Some(Token::t_i8) => Type::I8,
        Some(Token::t_i16) => Type::I16,
        Some(Token::Kw_void) => Type::Void,
        _ => panic!("expected return type"),
      };
      let body = parse_block(iter);
      return TopLevel::Function(name, args, ret, body)
    }
    
    Some(Token::Kw_reg) => {
      iter.next(); // consume reg
      let t = match iter.next() {
        Some(Token::t_u8) => Type::U8,
        Some(Token::t_i8) => Type::I8,
        Some(Token::t_u16) => Type::U16,
        Some(Token::t_i16) => Type::I16,
        _ => panic!("expected type after reg"),
      };
      let id = match iter.next() {
        Some(Token::l_identifier(name)) => name,
        _ => panic!("expected identifier after type in reg declaration"),
      };
      match iter.next() {
        Some(Token::s_mem_lookup) => {},
        _ => panic!("expected '@' in reg declaration"),
      }
      let addr = match iter.next() {
        Some(Token::l_num(n)) => n as u16,
        _ => panic!("expected address in reg declaration"),
      };
      iter.next(); // consume ;
      return TopLevel::RegDecl(t, id, addr)
    }
    
    Some(Token::t_u8) | Some(Token::t_u16) | 
    Some(Token::t_i8) | Some(Token::t_i16) => {
      let mut t = match iter.next() {
        Some(Token::t_u8) => Type::U8,
        Some(Token::t_i8) => Type::I8,
        Some(Token::t_u16) => Type::U16,
        Some(Token::t_i16) => Type::I16,
        _ => panic!("expected type"),
      };
      
      // check for pointer
      if iter.peek() == Some(&Token::s_star) {
        iter.next();
        t = Type::Ptr(Box::new(t));
      }
      
      let identifier = match iter.next() {
        Some(Token::l_identifier(name)) => name,
        _ => panic!("expected identifier"),
      };
      
      let initialiser = match iter.peek() {
        Some(Token::s_equals) => {
          iter.next(); // consume =
          let expr = equality(iter);
          Some(expr)
        }
        Some(Token::s_semicolon) => None,
        _ => panic!("expected = or ; after global variable name"),
      };
      
      iter.next(); // consume ;
      return TopLevel::GlobalVar(t, identifier, initialiser)
    }
    _ => panic!("unexpected token at top level"),
  }
}

pub fn parse(tokens: Vec<Token>) -> Vec<TopLevel> {
  let mut iter = tokens.into_iter().peekable();
  let mut toplevels = Vec::new();
  
  while iter.peek().is_some() {
    toplevels.push(parse_toplevel(&mut iter));
  }
  
  println!("\n\nAST:\n\n{:#?}", toplevels);
  toplevels
}