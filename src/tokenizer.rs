#![allow(unused)] // <-- yuck!

use std::process;

#[derive(PartialEq, Debug)]
pub enum Token {
  // KEYWORDS
  Kw_fn,
  Kw_reg,
  Kw_return,
  Kw_void,
  Kw_if,
  Kw_else,
  Kw_while,
  Kw_for,
  
  // TYPES
  t_u8,     // only integer based types, chars can be u8, 6502 has no FPU
  t_i8,
  t_u16,
  t_i16,
  
  // SYMBOLS
  s_mem_lookup,     // @
  s_star,           // *, pointer and multiply
  s_lbracket,       // [
  s_rbracket,       // ]
  s_arrow,          // ->
  s_plus,           // +
  s_minus,          // -
  s_divide,         // /
  s_lparen,         // (
  s_rparen,         // )
  s_lbrace,         // {
  s_rbrace,         // }
  s_semicolon,      // ;
  s_equals,         // =
  s_equalsequals,   // ==
  s_plus_equals,    // +=
  s_minus_equals,   // -=
  s_bang,           // !
  s_bang_equals,     // !=
  s_lt,             // <
  s_gt,             // >
  s_lte,            // <=
  s_gte,            // >=
  s_ampersand,      // & (for address-of later)
  s_comma,          // , (for function args)
  
  // LITERALS
  l_num(i64),
  l_string(String),
  l_identifier(String),
}


// Reads in raw source code, and outputs a list of tokens on succes
// program quits on failure after printing all errors.
pub fn tokenize(src: &str, file_path: &str) -> Vec<Token> {
  let mut tokens = Vec::new();
  let mut chars = src.chars().peekable();
  let mut line = 1;
  let mut has_errored = false;
  
  while let Some(c) = chars.next() {
    match c {
      '\n' => {
        line += 1;
        continue;
      },
      
      ' ' | '\t' | '\r' => continue, // skip whitespace
      '/' => { // comments or divide
        if chars.peek() == Some(&'/') {
          // consume the rest of the line
          while let Some(&c) = chars.peek() {
            if c == '\n' {
              break;
            }
            chars.next();
          }
        } else {
          tokens.push(Token::s_divide);
        }
      },
      
      ';' => tokens.push(Token::s_semicolon),
      '(' => tokens.push(Token::s_lparen),
      ')' => tokens.push(Token::s_rparen),
      '{' => tokens.push(Token::s_lbrace),
      '}' => tokens.push(Token::s_rbrace),
      '@' => tokens.push(Token::s_mem_lookup),
      '[' => tokens.push(Token::s_lbracket),
      ']' => tokens.push(Token::s_rbracket),
      '&' => tokens.push(Token::s_ampersand),
      ',' => tokens.push(Token::s_comma),
      '*' => tokens.push(Token::s_star),
      
      '+' => {
        if chars.peek() == Some(&'=') {
          chars.next(); // consume the '='
          tokens.push(Token::s_plus_equals);          
        } else {
          tokens.push(Token::s_plus)
        }
      },
      
      '!' => {
        if chars.peek() == Some(&'=') {
          tokens.push(Token::s_bang_equals);
          chars.next();
        } else {
          tokens.push(Token::s_bang)
        }
      },
      
      '-' => {
        if chars.peek() == Some(&'>') {
          chars.next(); // consume the '>'
          tokens.push(Token::s_arrow);
        } else if chars.peek() == Some(&'=') {
          chars.next(); // consume the '='
          tokens.push(Token::s_minus_equals);          
        } else {
          tokens.push(Token::s_minus);
        }
      }
      
      '=' => {
        if chars.peek() == Some(&'=') {
          tokens.push(Token::s_equalsequals); 
          chars.next();
        } else {
          tokens.push(Token::s_equals) 
        }
      },
      
      '<' => {
        if chars.peek() == Some(&'=') {
          tokens.push(Token::s_lte);
          chars.next();
        } else {
          tokens.push(Token::s_lt)
        }
      },
      
      '>' => {
        if chars.peek() == Some(&'=') {
          tokens.push(Token::s_gte);
          chars.next();
        } else {
          tokens.push(Token::s_gt)
        }
      },
      
      c if c.is_alphabetic() || c == '_' => {
        let mut ident = String::from(c);
        while let Some(&next) = chars.peek() {
          if next.is_alphanumeric() || next == '_' {
            ident.push(next);
            chars.next();
          } else {
            break;
          }
        }
        match ident.as_str() {
          "fn"     => tokens.push(Token::Kw_fn),
          "reg"    => tokens.push(Token::Kw_reg),
          "return" => tokens.push(Token::Kw_return),
          "void" => tokens.push(Token::Kw_void),
          "if"     => tokens.push(Token::Kw_if),
          "else"   => tokens.push(Token::Kw_else),
          "while"  => tokens.push(Token::Kw_while),
          "for"    => tokens.push(Token::Kw_for),
          "u8"     => tokens.push(Token::t_u8),
          "i8"     => tokens.push(Token::t_i8),
          "u16"    => tokens.push(Token::t_u16),
          "i16"    => tokens.push(Token::t_i16),
          _        => tokens.push(Token::l_identifier(ident)),
        }
      },
      
      c if c.is_numeric() => {
        let mut num = String::from(c);
        // check for hex prefix
        if c == '0' && chars.peek() == Some(&'x') {
          num.push('x');
          chars.next(); // consume 'x'
          while let Some(&next) = chars.peek() {
            if next.is_ascii_hexdigit() {
              num.push(next);
              chars.next();
            } else {
              break;
            }
          }
          // parse as hex
          let val = i64::from_str_radix(&num[2..], 16).unwrap();
          tokens.push(Token::l_num(val));
        } else {
          while let Some(&next) = chars.peek() {
            if next.is_numeric() {
              num.push(next);
              chars.next();
            } else {
              break;
            }
          }
          tokens.push(Token::l_num(num.parse::<i64>().unwrap()));
        }
      },
      
      '"' => {
        let mut s = String::new();
        while let Some(c) = chars.next() {
          if c == '"' { break; }
          s.push(c);
        }
        tokens.push(Token::l_string(s));
      },
      
      // etc
      _ => {
        eprintln!("unexpected character: {} at {}:{}", c, file_path, line);
        has_errored = true;
      }
    }
  }
  
  if has_errored {
    process::exit(1);
  }
  
  tokens
}