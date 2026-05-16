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
      // could be a plain identifier OR a function call
      // peek ahead to see if there's a (
      if iter.peek() == Some(&Token::s_lparen) {
        iter.next(); // consume (
        // parse arguments...
        Expr::Call(name, vec![]) // empty args for now
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
      let condition = equality(iter);
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
      let t = match iter.next() {
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
        // handle pointer type
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
    _ => {
      todo!();
    }
  }
}

pub fn parse(tokens: Vec<Token>) -> Vec<TopLevel> {
  let mut iter = tokens.into_iter().peekable();
  
  // temporary test - just parse one expression and print it
  let expr = parse_stmt(&mut iter);
  println!("{:#?}", expr);
  
  vec![] // return empty for now
}