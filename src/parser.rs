use crate::tokenizer::{Token, TokenLocation};
use std::fmt;
use std::iter::Peekable;

pub type ParseResult<T> = Result<T, ParseError>;

#[derive(Debug)]
pub enum ParseError {
  UnexpectedEOF { expected: String, context: String, location: Option<TokenLocation> },
  UnexpectedToken { expected: String, found: Token, context: String, location: TokenLocation },
}

impl fmt::Display for ParseError {
  fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    match self {
      ParseError::UnexpectedEOF { expected, context, location } => {
        let loc_str = format_location(location);
        write!(f, "Unexpected end of input while {}: expected {} {}", context, expected, loc_str)
      }
      ParseError::UnexpectedToken { expected, found, context, location } => {
        let loc_str = format_location(&Some(location.clone()));
        write!(f, "Unexpected token while {}: expected {}, found {} {}", context, expected, token_name(found), loc_str)
      }
    }
  }
}

fn format_location(location: &Option<TokenLocation>) -> String {
  match location {
    Some(TokenLocation::File { path, line }) => format!("at {}:{}", path, line),
    None => String::new(),
  }
}

impl std::error::Error for ParseError {}

#[derive(Debug, PartialEq, Clone)]
enum TokenKind {
  Kw_fn,
  Kw_reg,
  Kw_return,
  Kw_void,
  Kw_if,
  Kw_else,
  Kw_while,
  Kw_for,
  t_u8,
  t_i8,
  t_u16,
  t_i16,
  s_mem_lookup,
  s_star,
  s_lbracket,
  s_rbracket,
  s_arrow,
  s_plus,
  s_minus,
  s_divide,
  s_lparen,
  s_rparen,
  s_lbrace,
  s_rbrace,
  s_semicolon,
  s_equals,
  s_equalsequals,
  s_plus_equals,
  s_minus_equals,
  s_bang,
  s_bang_equals,
  s_lt,
  s_gt,
  s_lte,
  s_gte,
  s_ampersand,
  s_comma,
  l_num,
  l_string,
  l_identifier,
}

fn token_kind(token: &Token) -> TokenKind {
  match token {
    Token::Kw_fn(_) => TokenKind::Kw_fn,
    Token::Kw_reg(_) => TokenKind::Kw_reg,
    Token::Kw_return(_) => TokenKind::Kw_return,
    Token::Kw_void(_) => TokenKind::Kw_void,
    Token::Kw_if(_) => TokenKind::Kw_if,
    Token::Kw_else(_) => TokenKind::Kw_else,
    Token::Kw_while(_) => TokenKind::Kw_while,
    Token::Kw_for(_) => TokenKind::Kw_for,
    Token::t_u8(_) => TokenKind::t_u8,
    Token::t_i8(_) => TokenKind::t_i8,
    Token::t_u16(_) => TokenKind::t_u16,
    Token::t_i16(_) => TokenKind::t_i16,
    Token::s_mem_lookup(_) => TokenKind::s_mem_lookup,
    Token::s_star(_) => TokenKind::s_star,
    Token::s_lbracket(_) => TokenKind::s_lbracket,
    Token::s_rbracket(_) => TokenKind::s_rbracket,
    Token::s_arrow(_) => TokenKind::s_arrow,
    Token::s_plus(_) => TokenKind::s_plus,
    Token::s_minus(_) => TokenKind::s_minus,
    Token::s_divide(_) => TokenKind::s_divide,
    Token::s_lparen(_) => TokenKind::s_lparen,
    Token::s_rparen(_) => TokenKind::s_rparen,
    Token::s_lbrace(_) => TokenKind::s_lbrace,
    Token::s_rbrace(_) => TokenKind::s_rbrace,
    Token::s_semicolon(_) => TokenKind::s_semicolon,
    Token::s_equals(_) => TokenKind::s_equals,
    Token::s_equalsequals(_) => TokenKind::s_equalsequals,
    Token::s_plus_equals(_) => TokenKind::s_plus_equals,
    Token::s_minus_equals(_) => TokenKind::s_minus_equals,
    Token::s_bang(_) => TokenKind::s_bang,
    Token::s_bang_equals(_) => TokenKind::s_bang_equals,
    Token::s_lt(_) => TokenKind::s_lt,
    Token::s_gt(_) => TokenKind::s_gt,
    Token::s_lte(_) => TokenKind::s_lte,
    Token::s_gte(_) => TokenKind::s_gte,
    Token::s_ampersand(_) => TokenKind::s_ampersand,
    Token::s_comma(_) => TokenKind::s_comma,
    Token::l_num(_, _) => TokenKind::l_num,
    Token::l_string(_, _) => TokenKind::l_string,
    Token::l_identifier(_, _) => TokenKind::l_identifier,
  }
}

fn token_name(token: &Token) -> String {
  match token {
    Token::Kw_fn(_) => "fn".into(),
    Token::Kw_reg(_) => "reg".into(),
    Token::Kw_return(_) => "return".into(),
    Token::Kw_void(_) => "void".into(),
    Token::Kw_if(_) => "if".into(),
    Token::Kw_else(_) => "else".into(),
    Token::Kw_while(_) => "while".into(),
    Token::Kw_for(_) => "for".into(),
    Token::t_u8(_) => "u8".into(),
    Token::t_i8(_) => "i8".into(),
    Token::t_u16(_) => "u16".into(),
    Token::t_i16(_) => "i16".into(),
    Token::s_mem_lookup(_) => "@".into(),
    Token::s_star(_) => "*".into(),
    Token::s_lbracket(_) => "[".into(),
    Token::s_rbracket(_) => "]".into(),
    Token::s_arrow(_) => "->".into(),
    Token::s_plus(_) => "+".into(),
    Token::s_minus(_) => "-".into(),
    Token::s_divide(_) => "/".into(),
    Token::s_lparen(_) => "(".into(),
    Token::s_rparen(_) => ")".into(),
    Token::s_lbrace(_) => "{".into(),
    Token::s_rbrace(_) => "}".into(),
    Token::s_semicolon(_) => ";".into(),
    Token::s_equals(_) => "=".into(),
    Token::s_equalsequals(_) => "==".into(),
    Token::s_plus_equals(_) => "+=".into(),
    Token::s_minus_equals(_) => "-=".into(),
    Token::s_bang(_) => "!".into(),
    Token::s_bang_equals(_) => "!=".into(),
    Token::s_lt(_) => "<".into(),
    Token::s_gt(_) => ">".into(),
    Token::s_lte(_) => "<=".into(),
    Token::s_gte(_) => ">=".into(),
    Token::s_ampersand(_) => "&".into(),
    Token::s_comma(_) => ",".into(),
    Token::l_num(n, _) => format!("number {}", n),
    Token::l_string(s, _) => format!("string \"{}\"", s),
    Token::l_identifier(s, _) => format!("identifier {}", s),
  }
}

fn token_location(token: &Token) -> TokenLocation {
  match token {
    Token::Kw_fn(loc) | Token::Kw_reg(loc) | Token::Kw_return(loc) | Token::Kw_void(loc) |
    Token::Kw_if(loc) | Token::Kw_else(loc) | Token::Kw_while(loc) | Token::Kw_for(loc) |
    Token::t_u8(loc) | Token::t_i8(loc) | Token::t_u16(loc) | Token::t_i16(loc) |
    Token::s_mem_lookup(loc) | Token::s_star(loc) | Token::s_lbracket(loc) | Token::s_rbracket(loc) |
    Token::s_arrow(loc) | Token::s_plus(loc) | Token::s_minus(loc) | Token::s_divide(loc) |
    Token::s_lparen(loc) | Token::s_rparen(loc) | Token::s_lbrace(loc) | Token::s_rbrace(loc) |
    Token::s_semicolon(loc) | Token::s_equals(loc) | Token::s_equalsequals(loc) |
    Token::s_plus_equals(loc) | Token::s_minus_equals(loc) | Token::s_bang(loc) |
    Token::s_bang_equals(loc) | Token::s_lt(loc) | Token::s_gt(loc) | Token::s_lte(loc) |
    Token::s_gte(loc) | Token::s_ampersand(loc) | Token::s_comma(loc) => loc.clone(),
    Token::l_num(_, loc) | Token::l_string(_, loc) | Token::l_identifier(_, loc) => loc.clone(),
  }
}

fn expect_symbol(
  iter: &mut Peekable<impl Iterator<Item=Token>>,
  expected_kind: TokenKind,
  expected_desc: &str,
  context: &str,
) -> ParseResult<()> {
  let token = next_token(iter, context)?;
  if token_kind(&token) == expected_kind {
    Ok(())
  } else {
    let location = token_location(&token);
    Err(ParseError::UnexpectedToken {
      expected: expected_desc.into(),
      found: token,
      context: context.into(),
      location,
    })
  }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Type {
  U8,
  I8,
  U16,
  I16,
  Void,
  Ptr(Box<Type>),  // pointer to any type, e.g. Ptr(U8) = u8*
}

#[derive(Debug, Clone)]
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

#[derive(Debug, Clone)]
pub enum Expr {
  Number(i64),
  Identifier(String),
  BinOp(Box<Expr>, Op, Box<Expr>),
  Unary(Op, Box<Expr>),
  Call(String, Vec<Expr>),
  Deref(Box<Expr>),
  Cast(Type, Box<Expr>),
}

#[derive(Debug)]
pub enum Stmt {
  VarDecl(Type, String, Option<Expr>),
  Assign(String, Expr),
  Return(Option<Expr>),
  If(Expr, Vec<Stmt>, Option<Vec<Stmt>>),
  While(Expr, Vec<Stmt>),
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
fn next_token(iter: &mut Peekable<impl Iterator<Item=Token>>, context: &str) -> ParseResult<Token> {
  iter.next().ok_or(ParseError::UnexpectedEOF {
    expected: "token".into(),
    context: context.into(),
    location: None,
  })
}

fn parse_type(iter: &mut Peekable<impl Iterator<Item=Token>>, context: &str) -> ParseResult<Type> {
  let mut t = match next_token(iter, context)? {
    Token::t_u8(_) => Type::U8,
    Token::t_i8(_) => Type::I8,
    Token::t_u16(_) => Type::U16,
    Token::t_i16(_) => Type::I16,
    Token::Kw_void(_) => Type::Void,
    token => {
      let location = token_location(&token);
      return Err(ParseError::UnexpectedToken {
        expected: "type".into(),
        found: token,
        context: context.into(),
        location,
      })
    }
  };

  while matches!(iter.peek(), Some(Token::s_star(_))) {
    iter.next();
    t = Type::Ptr(Box::new(t));
  }

  Ok(t)
}

fn primary(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<Expr> {
  let token = next_token(iter, "expression")?;

  match token {
    Token::l_num(n, _) => Ok(Expr::Number(n)),

    Token::s_lparen(_) => {
      let is_cast = matches!(iter.peek(), Some(Token::t_u8(_)) | Some(Token::t_i8(_)) | Some(Token::t_u16(_)) | Some(Token::t_i16(_)));

      if is_cast {
        let t = parse_type(iter, "cast type")?;
        expect_symbol(iter, TokenKind::s_rparen, ")", "cast expression")?;
        let val = equality(iter)?;
        Ok(Expr::Cast(t, Box::new(val)))
      } else {
        let expr = equality(iter)?;
        expect_symbol(iter, TokenKind::s_rparen, ")", "grouped expression")?;
        Ok(expr)
      }
    }

    Token::l_identifier(name, _) => {
      if matches!(iter.peek(), Some(Token::s_lparen(_))) {
        iter.next(); // consume (
        let mut args = Vec::new();

        loop {
          match iter.peek() {
            Some(Token::s_rparen(_)) => {
              iter.next(); // consume )
              break;
            }
            Some(Token::s_comma(_)) => {
              iter.next(); // consume ,
            }
            Some(_) => args.push(equality(iter)?),
            None => {
              return Err(ParseError::UnexpectedEOF {
                expected: "closing ')'".into(),
                context: "function call".into(),
                location: None,
              })
            }
          }
        }

        Ok(Expr::Call(name, args))
      } else {
        Ok(Expr::Identifier(name))
      }
    }

    token => {
      let location = token_location(&token);
      Err(ParseError::UnexpectedToken {
        expected: "expression".into(),
        found: token,
        context: "primary".into(),
        location,
      })
    },
  }
}

fn unary(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<Expr> {
  match iter.peek() {
    Some(Token::s_bang(_)) => {
      iter.next();
      let operand = unary(iter)?;
      Ok(Expr::Unary(Op::Bang, Box::new(operand)))
    }

    Some(Token::s_minus(_)) => {
      iter.next();
      let operand = unary(iter)?;
      Ok(Expr::Unary(Op::Negate, Box::new(operand)))
    }

    Some(Token::s_star(_)) | Some(Token::s_mem_lookup(_)) => {
      iter.next();
      let operand = unary(iter)?;
      Ok(Expr::Deref(Box::new(operand)))
    }
    _ => primary(iter),
  }
}

fn factor(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<Expr> {
  let mut left = unary(iter)?;

  loop {
    match iter.peek() {
      Some(Token::s_star(_)) => {
        iter.next();
        let right = unary(iter)?;
        left = Expr::BinOp(Box::new(left), Op::Multiply, Box::new(right));
      }
      Some(Token::s_divide(_)) => {
        iter.next();
        let right = unary(iter)?;
        left = Expr::BinOp(Box::new(left), Op::Divide, Box::new(right));
      }
      _ => break,
    }
  }

  Ok(left)
}

fn term(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<Expr> {
  let mut left = factor(iter)?;

  loop {
    match iter.peek() {
      Some(Token::s_plus(_)) => {
        iter.next();
        let right = factor(iter)?;
        left = Expr::BinOp(Box::new(left), Op::Plus, Box::new(right));
      }
      Some(Token::s_minus(_)) => {
        iter.next();
        let right = factor(iter)?;
        left = Expr::BinOp(Box::new(left), Op::Minus, Box::new(right));
      }
      _ => break,
    }
  }

  Ok(left)
}

fn comparison(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<Expr> {
  let mut left = term(iter)?;

  loop {
    match iter.peek() {
      Some(Token::s_lt(_)) => {
        iter.next();
        let right = term(iter)?;
        left = Expr::BinOp(Box::new(left), Op::Lt, Box::new(right));
      }

      Some(Token::s_gt(_)) => {
        iter.next();
        let right = term(iter)?;
        left = Expr::BinOp(Box::new(left), Op::Gt, Box::new(right));
      }

      Some(Token::s_lte(_)) => {
        iter.next();
        let right = term(iter)?;
        left = Expr::BinOp(Box::new(left), Op::Lte, Box::new(right));
      }

      Some(Token::s_gte(_)) => {
        iter.next();
        let right = term(iter)?;
        left = Expr::BinOp(Box::new(left), Op::Gte, Box::new(right));
      }
      _ => break,
    }
  }

  Ok(left)
}

// root of recursive descent
// equality    looks for == !=      calls comparison
// comparison  looks for < > <= >=  calls term
// term        looks for + -        calls factor
// factor      looks for * /        calls unary
// unary       looks for * @ ! -    calls primary
// primary     looks for literals, identifiers, (expr)   consumes tokens
fn equality(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<Expr> {
  let mut left = comparison(iter)?;

  loop {
    match iter.peek() {
      Some(Token::s_equalsequals(_)) => {
        iter.next();
        let right = comparison(iter)?;
        left = Expr::BinOp(Box::new(left), Op::EqualsEquals, Box::new(right));
      }
      Some(Token::s_bang_equals(_)) => {
        iter.next();
        let right = comparison(iter)?;
        left = Expr::BinOp(Box::new(left), Op::BangEquals, Box::new(right));
      }
      _ => break,
    }
  }

  Ok(left)
}


/////////////////////////////////////////////////////////////////////////////
/// Main Parsing
/////////////////////////////////////////////////////////////////////////////

fn parse_block(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<Vec<Stmt>> {
  expect_symbol(iter, TokenKind::s_lbrace, "{", "block")?;
  let mut stmts = Vec::new();

  loop {
    match iter.peek() {
      Some(Token::s_rbrace(_)) => {
        iter.next(); // consume }
        break;
      }
      Some(_) => stmts.push(parse_stmt(iter)?),
      None => {
        return Err(ParseError::UnexpectedEOF {
          expected: "}".into(),
          context: "block".into(),
          location: None,
        })
      }
    }
  }

  Ok(stmts)
}

fn parse_stmt(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<Stmt> {
  match iter.peek() {
    Some(Token::Kw_return(_)) => {
      iter.next(); // consume return

      if matches!(iter.peek(), Some(Token::s_semicolon(_))) {
        iter.next();
        Ok(Stmt::Return(None))
      } else {
        let expr = equality(iter)?;
        expect_symbol(iter, TokenKind::s_semicolon, ";", "return statement")?;
        Ok(Stmt::Return(Some(expr)))
      }
    }

    Some(Token::Kw_while(_)) => {
      iter.next(); // consume while
      expect_symbol(iter, TokenKind::s_lparen, "(", "while condition")?;
      let condition = equality(iter)?;
      expect_symbol(iter, TokenKind::s_rparen, ")", "while condition")?;
      let body = parse_block(iter)?;
      Ok(Stmt::While(condition, body))
    }

    Some(Token::Kw_if(_)) => {
      iter.next(); // consume if
      expect_symbol(iter, TokenKind::s_lparen, "(", "if condition")?;
      let condition = equality(iter)?;
      expect_symbol(iter, TokenKind::s_rparen, ")", "if condition")?;
      let body = parse_block(iter)?;
      let else_body = if matches!(iter.peek(), Some(Token::Kw_else(_))) {
        iter.next(); // consume else
        Some(parse_block(iter)?)
      } else {
        None
      };
      Ok(Stmt::If(condition, body, else_body))
    }

    Some(Token::t_u8(_)) | Some(Token::t_u16(_)) | Some(Token::t_i8(_)) | Some(Token::t_i16(_)) | Some(Token::Kw_void(_)) => {
      let mut t = parse_type(iter, "variable declaration")?;

      if matches!(iter.peek(), Some(Token::s_star(_))) {
        iter.next();
        t = Type::Ptr(Box::new(t));
      }

      let identifier = match next_token(iter, "variable declaration")? {
        Token::l_identifier(name, _) => name,
        token => {
          let location = token_location(&token);
          return Err(ParseError::UnexpectedToken {
            expected: "identifier".into(),
            found: token,
            context: "variable declaration".into(),
            location,
          })
        }
      };

      expect_symbol(iter, TokenKind::s_equals, "=", "variable declaration")?;
      let expr = equality(iter)?;
      expect_symbol(iter, TokenKind::s_semicolon, ";", "variable declaration")?;
      Ok(Stmt::VarDecl(t, identifier, Some(expr)))
    }

    Some(Token::l_identifier(_, _)) => {
      let id = match next_token(iter, "assignment")? {
        Token::l_identifier(name, _) => name,
        _ => unreachable!(),
      };

      let assign_token = next_token(iter, "assignment")?;
      let expr = equality(iter)?;
      expect_symbol(iter, TokenKind::s_semicolon, ";", "assignment")?;

      let stmt = match assign_token {
        Token::s_equals(_) => Stmt::Assign(id, expr),
        Token::s_plus_equals(_) => Stmt::Assign(
          id.clone(),
          Expr::BinOp(Box::new(Expr::Identifier(id)), Op::Plus, Box::new(expr)),
        ),
        Token::s_minus_equals(_) => Stmt::Assign(
          id.clone(),
          Expr::BinOp(Box::new(Expr::Identifier(id)), Op::Minus, Box::new(expr)),
        ),
        token => {
          let location = token_location(&token);
          return Err(ParseError::UnexpectedToken {
            expected: "=, +=, or -=".into(),
            found: token,
            context: "assignment".into(),
            location,
          })
        }
      };

      Ok(stmt)
    }

    Some(token) => {
      let location = token_location(token);
      Err(ParseError::UnexpectedToken {
        expected: "statement".into(),
        found: token.clone(),
        context: "parse_stmt".into(),
        location,
      })
    },
    None => Err(ParseError::UnexpectedEOF {
      expected: "statement".into(),
      context: "parse_stmt".into(),
      location: None,
    }),
  }
}

fn parse_args(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<Vec<(Type, String)>> {
  expect_symbol(iter, TokenKind::s_lparen, "(", "argument list")?;
  let mut args = Vec::new();

  loop {
    match iter.peek() {
      Some(Token::s_rparen(_)) => {
        iter.next(); // consume )
        break;
      }
      Some(Token::s_comma(_)) => {
        iter.next();
      }
      Some(_) => {
        let t = parse_type(iter, "argument type")?;
        let name = match next_token(iter, "argument name")? {
          Token::l_identifier(n, _) => n,
          token => {
            let location = token_location(&token);
            return Err(ParseError::UnexpectedToken {
              expected: "identifier".into(),
              found: token,
              context: "argument list".into(),
              location,
            })
          }
        };
        args.push((t, name));
      }
      None => {
        return Err(ParseError::UnexpectedEOF {
          expected: ")".into(),
          context: "argument list".into(),
          location: None,
        })
      }
    }
  }
  Ok(args)
}

fn parse_toplevel(iter: &mut Peekable<impl Iterator<Item=Token>>) -> ParseResult<TopLevel> {
  match iter.peek() {
    Some(Token::Kw_fn(_)) => {
      iter.next();
      let name = match next_token(iter, "function declaration")? {
        Token::l_identifier(n, _) => n,
        token => {
          let location = token_location(&token);
          return Err(ParseError::UnexpectedToken {
            expected: "function name".into(),
            found: token,
            context: "function declaration".into(),
            location,
          })
        }
      };

      let args = parse_args(iter)?;
      expect_symbol(iter, TokenKind::s_arrow, "->", "function declaration")?;
      let ret = parse_type(iter, "function return type")?;
      let body = parse_block(iter)?;
      Ok(TopLevel::Function(name, args, ret, body))
    }

    Some(Token::Kw_reg(_)) => {
      iter.next();
      let t = parse_type(iter, "register declaration")?;
      let id = match next_token(iter, "register declaration")? {
        Token::l_identifier(name, _) => name,
        token => {
          let location = token_location(&token);
          return Err(ParseError::UnexpectedToken {
            expected: "identifier".into(),
            found: token,
            context: "register declaration".into(),
            location,
          })
        }
      };
      expect_symbol(iter, TokenKind::s_mem_lookup, "@", "register declaration")?;
      let addr = match next_token(iter, "register declaration")? {
        Token::l_num(n, _) => n as u16,
        token => {
          let location = token_location(&token);
          return Err(ParseError::UnexpectedToken {
            expected: "numeric address".into(),
            found: token,
            context: "register declaration".into(),
            location,
          })
        }
      };
      expect_symbol(iter, TokenKind::s_semicolon, ";", "register declaration")?;
      Ok(TopLevel::RegDecl(t, id, addr))
    }

    Some(Token::t_u8(_)) | Some(Token::t_u16(_)) | Some(Token::t_i8(_)) | Some(Token::t_i16(_)) => {
      let mut t = parse_type(iter, "global declaration")?;

      if matches!(iter.peek(), Some(Token::s_star(_))) {
        iter.next();
        t = Type::Ptr(Box::new(t));
      }

      let identifier = match next_token(iter, "global declaration")? {
        Token::l_identifier(name, _) => name,
        token => {
          let location = token_location(&token);
          return Err(ParseError::UnexpectedToken {
            expected: "identifier".into(),
            found: token,
            context: "global declaration".into(),
            location,
          })
        }
      };

      let initialiser = match iter.peek() {
        Some(Token::s_equals(_)) => {
          iter.next();
          Some(equality(iter)?)
        }
        Some(Token::s_semicolon(_)) => None,
        Some(token) => {
          let location = token_location(token);
          return Err(ParseError::UnexpectedToken {
            expected: "= or ;".into(),
            found: token.clone(),
            context: "global declaration".into(),
            location,
          })
        }
        None => {
          return Err(ParseError::UnexpectedEOF {
            expected: ";".into(),
            context: "global declaration".into(),
            location: None,
          })
        }
      };

      expect_symbol(iter, TokenKind::s_semicolon, ";", "global declaration")?;
      Ok(TopLevel::GlobalVar(t, identifier, initialiser))
    }

    Some(token) => {
      let location = token_location(token);
      Err(ParseError::UnexpectedToken {
        expected: "top-level declaration".into(),
        found: token.clone(),
        context: "parse_toplevel".into(),
        location,
      })
    }
    None => Err(ParseError::UnexpectedEOF {
      expected: "top-level declaration".into(),
      context: "parse_toplevel".into(),
      location: None,
    }),
  }
}

pub fn parse(tokens: Vec<Token>) -> ParseResult<Vec<TopLevel>> {
  let mut iter = tokens.into_iter().peekable();
  let mut toplevels = Vec::new();
  
  while iter.peek().is_some() {
    toplevels.push(parse_toplevel(&mut iter)?);
  }

  Ok(toplevels)
}