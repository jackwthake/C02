#![allow(non_camel_case_types)]

use std::env;
use std::process;
use std::fs;

mod tokenizer;
mod parser;

fn main() {
  let args: Vec<String> = env::args().collect();
  
  if args.len() < 2 {
    eprintln!("Error: Must specify file to compile.");
    eprintln!("Usage: {} <filepath>", args[0]);
    process::exit(1);
  }
  
  let path = &args[1];
  let fd = fs::read_to_string(path);
  
  if !path.ends_with(".c02") {
    eprintln!("Error: not a C02 source file, valid extensions are .c02 but was passed:\n\t{}", path);
    process::exit(1);
  }
  
  match fd {
    Ok(contents) => {
      let tokens = tokenizer::tokenize(&contents, path);
      let ast = parser::parse(&tokens);
      println!("{:#?}", tokens);
    } Err(e) => {
      eprintln!("Error: failed to read file at path {}: {}", path, e);
      process::exit(1);
    }
  }
}
