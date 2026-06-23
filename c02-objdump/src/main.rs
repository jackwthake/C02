use clap::Parser;
use std::{fs, path::PathBuf, process};

use crate::disassembler::{disassemble, dump_data, dump_sections, Mode};

mod disassembler;

#[derive(Parser, Debug)]
#[command(author, version, about = "C02 disassembler", long_about = None)]
struct Args {
  /// Path to the binary file to disassemble
  file: PathBuf,

  /// Show all sections (disassembly + data hex dump)
  #[arg(short = 'a', long = "all")]
  all: bool,

  /// Print section layout only
  #[arg(short = 's', long = "sections")]
  sections: bool,

  /// Hex dump of .data section only
  #[arg(short = 'd', long = "data")]
  data: bool,
}

fn main() {
  let args = Args::parse();
  let path_str = args.file.to_string_lossy().into_owned();

  let bytes = fs::read(&args.file).unwrap_or_else(|e| {
    eprintln!("Error: failed to read file at {}: {}", path_str, e);
    process::exit(1);
  });

  if args.sections {
    dump_sections(&bytes);
  } else if args.data {
    dump_data(&bytes);
  } else if args.all {
    disassemble(&bytes, Mode::All);
  } else {
    disassemble(&bytes, Mode::CodeOnly);
  }
}