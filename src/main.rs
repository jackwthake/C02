#![allow(non_camel_case_types)]

use std::env;
use std::process;
use std::fs;

use crate::disassembler::disassembler;
use crate::generator::emit_binary;

mod tokenizer;
mod parser;
mod analyzer;
mod generator;
mod disassembler;

fn main() {
  let args: Vec<String> = env::args().collect();
  
  if args.len() < 2 {
    eprintln!("Error: Must specify file to compile.");
    eprintln!("Usage: {} <filepath> [--no-out] [-d]", args[0]);
    process::exit(1);
  }
  
  let path = &args[1];
  let disassemble = args.contains(&"-d".to_string());
  let no_out = args.contains(&"--no-out".to_string());
  
  if disassemble {
    if !path.ends_with(".bin") {
      eprintln!("Error: -d expects a .bin file, got:\n\t{}", path);
      process::exit(1);
    }
  } else if !path.ends_with(".c02") {
    eprintln!("Error: expected a .c02 source file, got:\n\t{}", path);
    process::exit(1);
  }
  
  let config_path = "./c02_config.ron";
  let config = fs::read_to_string(config_path).unwrap_or_else(|_| {
    eprintln!("Warning: no config file found at {}, using default memory map", config_path);
    String::new()
  });
  
  let mem_map = ron::from_str(&config).unwrap();
  
  if disassemble {
    match fs::read(path) {
      Ok(bytes) => disassembler(bytes, mem_map),
      Err(e) => {
        eprintln!("Error: failed to read file at {}: {}", path, e);
        process::exit(1);
      }
    }
    process::exit(0);
  }
  
  // compiler path
  let contents = match fs::read_to_string(path) {
    Ok(s) => s,
    Err(e) => {
      eprintln!("Error: failed to read file at {}: {}", path, e);
      process::exit(1);
    }
  };
  
  let tokens = tokenizer::tokenize(&contents, path);
  if no_out {
    println!("=== TOKENS ===");
    for token in &tokens {
      println!("{:?}", token);
    }
  }
  
  let ast = match parser::parse(tokens) {
    Ok(ast) => ast,
    Err(e) => {
      eprintln!("Parse error: {}", e);
      process::exit(1);
    }
  };
  
  if no_out {
    println!("\n=== AST ===");
    for node in &ast {
      println!("{:#?}", node);
    }
  }
  
  let symbol_table = match analyzer::analyze(&ast) {
    Ok(st) => st,
    Err(e) => {
      eprintln!("Semantic error: {}", e);
      process::exit(1);
    }
  };
  
  if no_out {
    println!("\n=== SYMBOL TABLE ===");
    println!("{:#?}", symbol_table);
    return;
  }
  
  let output = generator::generate(ast, symbol_table, mem_map);
  let output_path = path.strip_suffix(".c02").unwrap_or(path).to_owned() + ".bin";
  
  if let Err(e) = emit_binary(&output, output_path) {
    eprintln!("Failed to write binary: {}", e);
    process::exit(1);
  }
}