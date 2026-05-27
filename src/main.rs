#![allow(non_camel_case_types)]

use std::env;
use std::process;
use std::fs;
use std::time::{Instant};

use crate::disassembler::disassembler;
use crate::generator::{Memory_Map, emit_binary};

mod tokenizer;
mod parser;
mod analyzer;
mod generator;
mod disassembler;

fn main() {
  let args: Vec<String> = env::args().collect();
  let comp_start = Instant::now();
  
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
  
  let mem_map = match ron::from_str(&config) {
    Ok(cfg) => {cfg}
    Err(_) => {
      eprintln!("WARNING: c02_config.ron not found, falling back to defaults.");
      Memory_Map {
        rom_start: 0x0000,
        rom_top: 0xFFFF
      }
    }
  };
  
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

  println!("Compiling: {}", path);
  
  // compiler path
  let contents = match fs::read_to_string(path) {
    Ok(s) => s,
    Err(e) => {
      eprintln!("\n\nError: failed to read file at {}: {}", path, e);
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
      eprintln!("\n\nParse error: {}", e);
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
      eprintln!("\n\nSemantic error: {}", e);
      process::exit(1);
    }
  };
  
  if no_out {
    println!("\n=== SYMBOL TABLE ===");
    println!("{:#?}", symbol_table);
    return;
  }

  let (prog_len, output) = generator::generate(ast, symbol_table, mem_map);

  let output_path = path.strip_suffix(".c02").unwrap_or(path).to_owned() + ".bin";
  
  if let Err(e) = emit_binary(&output, output_path) {
    eprintln!("\n\nFailed to write binary: {}", e);
    process::exit(1);
  }

  let comp_duration = comp_start.elapsed();
  let rom_perc = (prog_len as f64 / ((mem_map.rom_top - mem_map.rom_start) as f64)) * 100.00;
  println!("Done in: {:?}\nProgram size: {} bytes, {:.2}% of total ROM", comp_duration, prog_len, rom_perc);
}