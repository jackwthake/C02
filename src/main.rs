#![allow(non_camel_case_types)]

use std::env;
use std::process;
use std::fs;

mod tokenizer;
mod parser;
mod generator;

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
      match parser::parse(tokens) {
        Ok(ast) => {
          let output = generator::generate(ast);

          // write output to file with same name but .s extension
          let output_path = path.strip_suffix(".c02").unwrap_or(path).to_owned() + ".s";
          fs::write(&output_path, &output).unwrap_or_else(|e| {
            eprintln!("Error: failed to write output file at path {}: {}", output_path, e);
            process::exit(1);
          });
        }

        Err(err) => {
          eprintln!("Parse error: {}", err);
          process::exit(1);
        }
      }
    } Err(e) => {
      eprintln!("Error: failed to read file at path {}: {}", path, e);
      process::exit(1);
    }
  }
}
