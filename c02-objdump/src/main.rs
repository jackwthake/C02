use clap::Parser;
use std::{fs, path::PathBuf, process};

use crate::disassembler::disassembler;

mod disassembler;

#[derive(Parser, Debug)]
#[command(author, version, about = "C02 disassembler", long_about = None)]
struct Args {
  /// Path to the binary file to disassemble
  file: PathBuf,
}

fn main() {
  let args = Args::parse();
  let path_str = args.file.to_string_lossy().into_owned();

  let bytes = fs::read(&args.file).unwrap_or_else(|e| {
    eprintln!("Error: failed to read file at {}: {}", path_str, e);
    process::exit(1);
  });

  disassembler(bytes);
}