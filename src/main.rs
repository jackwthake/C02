#![allow(non_camel_case_types)]

use std::env;
use std::process;
use std::fs;

mod tokenizer;
mod parser;
mod analyzer;
mod generator;

fn main() {
  let args: Vec<String> = env::args().collect();
  
  if args.len() < 2 {
    eprintln!("Error: Must specify file to compile.");
    eprintln!("Usage: {} <filepath> [--no-out]", args[0]);
    process::exit(1);
  }
  
  let path = &args[1];
  let no_out = args.contains(&"--no-out".to_string());
  
  if !path.ends_with(".c02") {
    eprintln!("Error: not a C02 source file, valid extensions are .c02 but was passed:\n\t{}", path);
    process::exit(1);
  }

  // search for config from current directory
  let config_path = "./c02_config.ron";
  let config = fs::read_to_string(config_path).unwrap_or_else(|_| {
    eprintln!("Warning: no config file found at path {}, using default memory map", config_path);
    String::new() // return empty string to use defaults
  });
  
  let mem_map = ron::from_str(&config).unwrap();
  
  let fd = fs::read_to_string(path);
  
  match fd {
    Ok(contents) => {
      let tokens = tokenizer::tokenize(&contents, path);
      
      if no_out {
        println!("=== TOKENS ===");
        for token in &tokens {
          println!("{:?}", token);
        }
      }
      
      match parser::parse(tokens) {
        Ok(ast) => {
          if no_out {
            println!("\n=== AST ===");
            for node in &ast {
              println!("{:#?}", node);
            }
          }
          
          match analyzer::analyze(&ast) {
            Ok(symbol_table) => {
              if no_out {
                println!("\n=== SYMBOL TABLE ===");
                println!("{:#?}", symbol_table);
                return;
              }
              
              let output = generator::generate(ast, symbol_table, mem_map);
              let output_path = path.strip_suffix(".c02").unwrap_or(path).to_owned() + ".s";
              fs::write(&output_path, &output).unwrap_or_else(|e| {
                eprintln!("Error: failed to write output file at path {}: {}", output_path, e);
                process::exit(1);
              });
            }
            Err(err) => {
              eprintln!("Semantic error: {}", err);
              process::exit(1);
            }
          }
        }
        Err(err) => {
          eprintln!("Parse error: {}", err);
          process::exit(1);
        }
      }
    }
    Err(e) => {
      eprintln!("Error: failed to read file at path {}: {}", path, e);
      process::exit(1);
    }
  }
}