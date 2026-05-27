#![allow(non_camel_case_types)] // dont tell me how to name MY variables.

use clap::Parser;
use std::{fs, mem};
use std::path::PathBuf;
use std::process;
use std::time::Instant;

use crate::disassembler::disassembler;
use crate::generator::{emit_binary, Memory_Map};

mod analyzer;
mod disassembler;
mod generator;
mod parser;
mod tokenizer;

#[derive(Parser, Debug)]
#[command(author, version, about = "C02 Compiler", long_about = None)]
struct Args {
  /// The input source file (.c02) or binary (.bin, .out for -d)
  file: PathBuf,
  
  /// Disassemble a .bin or .out file
  #[arg(short, long)]
  disassemble: bool,
  
  /// Print intermediate compiler stages (Tokens, AST, Symbol Table)
  #[arg(short, long)]
  verbose: bool,

  /// Specify output file path (default: input with .bin extension)
  #[arg(short, long)]
  output: Option<PathBuf>,

  /// Path to c02_config.ron file used for memory map definition, will look in cwd if not passed
  #[arg(short, long)]
  cfg: Option<PathBuf>,
}

fn main() {
  let args = Args::parse();
  let comp_start = Instant::now();
  
  let path_str = args.file.to_string_lossy();
  let disassemble = args.disassemble;
  
  // Validate file extensions
  if disassemble {
    if !path_str.ends_with(".bin") && !path_str.ends_with(".out")  {
      eprintln!("Error: -d expects a .bin or .out file, got:\n\t{}", path_str);
      process::exit(1);
    }
  } else if !path_str.ends_with(".c02") {
    eprintln!("Error: expected a .c02 source file, got:\n\t{}", path_str);
    process::exit(1);
  }
  
  // Config loading
  let config_path = args.cfg.unwrap_or_else(|| PathBuf::from("./c02_config.ron"));
  let config = fs::read_to_string(config_path).unwrap_or_default();
  let mut mem_map = ron::from_str(&config).unwrap_or_else(|_| {
    eprintln!("WARNING: c02_config.ron not found or invalid, using defaults.");
    Memory_Map { rom_start: 0x0000, rom_top: 0xFFFF }
  });

  if mem_map.rom_start > mem_map.rom_top {
    eprintln!("WARNING: bad config, rom start > rom_top. swapping");
    mem::swap(&mut mem_map.rom_start, &mut mem_map.rom_top);
  }

  if mem_map.rom_start == mem_map.rom_top {
    eprintln!("ERROR: ROM defined in config is invalid, available ROM size is 0");
    process::exit(1);
  }
  
  // Handle Disassembly
  if disassemble {
    let bytes = fs::read(&args.file).unwrap_or_else(|e| {
      eprintln!("Error: failed to read file at {}: {}", path_str, e);
      process::exit(1);
    });
    disassembler(bytes, mem_map);
    process::exit(0);
  }
  
  // Compilation Logic
  println!("Compiling: {}", path_str);
  let contents = fs::read_to_string(&args.file).unwrap_or_else(|e| {
    eprintln!("\n\nError: failed to read file at {}: {}", path_str, e);
    process::exit(1);
  });
  
  let tokens = tokenizer::tokenize(&contents, &path_str);
  if args.verbose {
    println!("=== TOKENS ===\n{:?}", tokens);
  }
  
  let ast = parser::parse(tokens).unwrap_or_else(|e| {
    eprintln!("\n\nParse error: {}", e);
    process::exit(1);
  });
  
  if args.verbose {
    println!("\n=== AST ===\n{:#?}", ast);
  }
  
  let symbol_table = analyzer::analyze(&ast).unwrap_or_else(|e| {
    eprintln!("\n\nSemantic error: {}", e);
    process::exit(1);
  });
  
  if args.verbose {
    println!("\n=== SYMBOL TABLE ===\n{:#?}", symbol_table);
  }
  
  let (prog_len, output) = generator::generate(ast, symbol_table, mem_map);
  let output_path = args.output.unwrap_or_else(|| args.file.with_extension("bin"));
  
  if let Err(e) = emit_binary(&output, output_path) {
    eprintln!("\n\nFailed to write binary: {}", e);
    process::exit(1);
  }
  
  let comp_duration = comp_start.elapsed();
  let rom_perc = (prog_len as f64 / ((mem_map.rom_top - mem_map.rom_start) as f64)) * 100.00;
  println!(
    "Done in: {:?}\nProgram size: {} bytes, {:.2}% of total ROM",
    comp_duration, prog_len, rom_perc
  );
}