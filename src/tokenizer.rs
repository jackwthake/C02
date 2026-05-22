#[derive(PartialEq, Debug, Clone)]
pub enum TokenLocation {
  File { path: String, line: usize },
}

#[derive(PartialEq, Debug, Clone)]
pub enum Token {
  // KEYWORDS
  Kw_fn(TokenLocation),
  Kw_reg(TokenLocation),
  Kw_return(TokenLocation),
  Kw_void(TokenLocation),
  Kw_if(TokenLocation),
  Kw_else(TokenLocation),
  Kw_while(TokenLocation),
  Kw_for(TokenLocation),

  // TYPES
  t_u8(TokenLocation),     // only integer based types, chars can be u8, 6502 has no FPU
  t_i8(TokenLocation),
  t_u16(TokenLocation),
  t_i16(TokenLocation),

  // SYMBOLS
  s_mem_lookup(TokenLocation),     // @
  s_star(TokenLocation),           // *, pointer and multiply
  s_arrow(TokenLocation),          // ->
  s_plus(TokenLocation),           // +
  s_minus(TokenLocation),          // -
  s_divide(TokenLocation),         // /
  s_lparen(TokenLocation),         // (
  s_rparen(TokenLocation),         // )
  s_lbrace(TokenLocation),         // {
  s_rbrace(TokenLocation),         // }
  s_semicolon(TokenLocation),      // ;
  s_equals(TokenLocation),         // =
  s_equalsequals(TokenLocation),   // ==
  s_plus_equals(TokenLocation),    // +=
  s_minus_equals(TokenLocation),   // -=
  s_bang(TokenLocation),           // !
  s_bang_equals(TokenLocation),     // !=
  s_lt(TokenLocation),             // <
  s_gt(TokenLocation),             // >
  s_lte(TokenLocation),            // <=
  s_gte(TokenLocation),            // >=
  s_ampersand(TokenLocation),      // & (Address of operator)
  s_comma(TokenLocation),          // , (for function args)
  
  // LITERALS
  l_num(i64, TokenLocation),
  l_string(String, TokenLocation),
  l_identifier(String, TokenLocation),
}


// Reads in raw source code, and outputs a list of tokens on succes
// program quits on failure after printing all errors.
pub fn tokenize(src: &str, file_path: &str) -> Vec<Token> {
  let mut tokens = Vec::new();
  let mut chars = src.chars().peekable();
  let mut line = 1;
  
  while let Some(c) = chars.next() {
    if c == '\n' {
      line += 1;
      continue;
    }

    let location = TokenLocation::File { path: file_path.to_string(), line };

    match c {
      
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
          tokens.push(Token::s_divide(location));
        }
      },
      
      ';' => tokens.push(Token::s_semicolon(location)),
      '(' => tokens.push(Token::s_lparen(location)),
      ')' => tokens.push(Token::s_rparen(location)),
      '{' => tokens.push(Token::s_lbrace(location)),
      '}' => tokens.push(Token::s_rbrace(location)),
      '@' => tokens.push(Token::s_mem_lookup(location)),
      '&' => tokens.push(Token::s_ampersand(location)),
      ',' => tokens.push(Token::s_comma(location)),
      '*' => tokens.push(Token::s_star(location)),

      '+' => {
        if chars.peek() == Some(&'=') {
          chars.next(); // consume the '='
          tokens.push(Token::s_plus_equals(location));          
        } else {
          tokens.push(Token::s_plus(location))
        }
      },
      
      '!' => {
        if chars.peek() == Some(&'=') {
          tokens.push(Token::s_bang_equals(location));
          chars.next();
        } else {
          tokens.push(Token::s_bang(location))
        }
      },
      
      '-' => {
        if chars.peek() == Some(&'>') {
          chars.next(); // consume the '>'
          tokens.push(Token::s_arrow(location));
        } else if chars.peek() == Some(&'=') {
          chars.next(); // consume the '='
          tokens.push(Token::s_minus_equals(location));          
        } else {
          tokens.push(Token::s_minus(location));
        }
      }
      
      '=' => {
        if chars.peek() == Some(&'=') {
          tokens.push(Token::s_equalsequals(location)); 
          chars.next();
        } else {
          tokens.push(Token::s_equals(location)) 
        }
      },
      
      '<' => {
        if chars.peek() == Some(&'=') {
          tokens.push(Token::s_lte(location));
          chars.next();
        } else {
          tokens.push(Token::s_lt(location))
        }
      },
      
      '>' => {
        if chars.peek() == Some(&'=') {
          tokens.push(Token::s_gte(location));
          chars.next();
        } else {
          tokens.push(Token::s_gt(location))
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
          "fn"     => tokens.push(Token::Kw_fn(location)),
          "reg"    => tokens.push(Token::Kw_reg(location)),
          "return" => tokens.push(Token::Kw_return(location)),
          "void" => tokens.push(Token::Kw_void(location)),
          "if"     => tokens.push(Token::Kw_if(location)),
          "else"   => tokens.push(Token::Kw_else(location)),
          "while"  => tokens.push(Token::Kw_while(location)),
          "for"    => tokens.push(Token::Kw_for(location)),
          "u8"     => tokens.push(Token::t_u8(location)),
          "i8"     => tokens.push(Token::t_i8(location)),
          "u16"    => tokens.push(Token::t_u16(location)),
          "i16"    => tokens.push(Token::t_i16(location)),
          _        => tokens.push(Token::l_identifier(ident, location)),
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
          tokens.push(Token::l_num(val, location));
        } else {
          while let Some(&next) = chars.peek() {
            if next.is_numeric() {
              num.push(next);
              chars.next();
            } else {
              break;
            }
          }
          tokens.push(Token::l_num(num.parse::<i64>().unwrap(), location));
        }
      },
      
      '"' => {
        let mut s = String::new();
        while let Some(c) = chars.next() {
          if c == '"' { break; }
          s.push(c);
        }
        tokens.push(Token::l_string(s, location));
      },
      
      // etc
      _ => {
        eprintln!("Tokenizer error:unexpected character: {} at {}:{}", c, file_path, line);
      }
    }
  }
  
  tokens
}