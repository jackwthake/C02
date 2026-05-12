#![allow(unused)] // <-- yuck!

use crate::tokenizer::Token;

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
  NotEquals,
}

pub enum Expr {
  Number(i64),
  Identifier(String),
  BinOp(Box<Expr>, Op, Box<Expr>),
  Call(String, Vec<Expr>),
  Deref(Box<Expr>),
  Cast(Type, Box<Expr>),
  Index(Box<Expr>, Box<Expr>),
}

pub enum Stmt {
  VarDecl(Type, String, Option<Expr>),
  Assign(String, Expr),
  Return(Option<Expr>),
  If(Expr, Vec<Stmt>, Option<Vec<Stmt>>),
  While(Expr, Vec<Stmt>),
  ExprStmt(Expr),
}

#[derive(Default)]
pub enum TopLevel {
  #[default]
  Empty, // temporary
  Function(String, Vec<(Type, String)>, Type, Vec<Stmt>),
  RegDecl(Type, String, u16),
  GlobalVar(Type, String, Option<Expr>),
}

pub fn parse(tokens: &Vec<Token>) -> Vec<TopLevel> {
  let mut toplevels = Vec::new();

  toplevels.push(TopLevel::default());

  toplevels
}