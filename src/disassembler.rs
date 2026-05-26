use std::collections::HashMap;
use std::io::{Cursor, Read, Seek, SeekFrom};

use crate::generator::Memory_Map;

const VECTOR_TABLE_SIZE: usize = 6;

#[allow(dead_code)]
pub enum AddrMode {
  Implied,
  Immediate,
  ZeroPage,
  ZeroPageX,
  ZeroPageY,
  Absolute,
  AbsoluteX,
  AbsoluteY,
  Indirect,
  IndirectX,
  IndirectY,
  Relative,
}

fn print_op(op: &str, args: Vec<u8>, addr: u16, mode: AddrMode, labels: &HashMap<u16, String>) {
  let formatted = match mode {
    AddrMode::Implied   => op.to_string(),
    AddrMode::Immediate => format!("{} #${:02X}", op, args[0]),
    AddrMode::ZeroPage  => format!("{} ${:02X}", op, args[0]),
    AddrMode::ZeroPageX => format!("{} ${:02X},X", op, args[0]),
    AddrMode::ZeroPageY => format!("{} ${:02X},Y", op, args[0]),
    AddrMode::Absolute  => {
      let target = u16::from_le_bytes([args[0], args[1]]);
      if let Some(label) = labels.get(&target) {
        format!("{} {}", op, label)
      } else {
        format!("{} ${:04X}", op, target)
      }
    }
    AddrMode::AbsoluteX => format!("{} ${:04X},X", op, u16::from_le_bytes([args[0], args[1]])),
    AddrMode::AbsoluteY => format!("{} ${:04X},Y", op, u16::from_le_bytes([args[0], args[1]])),
    AddrMode::Indirect  => format!("{} (${:04X})", op, u16::from_le_bytes([args[0], args[1]])),
    AddrMode::IndirectX => format!("{} (${:02X},X)", op, args[0]),
    AddrMode::IndirectY => format!("{} (${:02X}),Y", op, args[0]),
    AddrMode::Relative  => format!("{} ${:02X}", op, args[0]),
  };
  
  println!("{:04X}: {}", addr, formatted);
}

fn collect_jump_targets(bytes: &[u8]) -> HashMap<u16, String> {
  let mut targets: std::collections::BTreeSet<u16> = std::collections::BTreeSet::new();
  let mut cursor = Cursor::new(bytes);
  let mut buffer = [0u8; 1];
  
  while cursor.read(&mut buffer).unwrap_or(0) > 0 {
    match buffer[0] {
      0x20 | 0x4C => {
        let mut op = [0u8; 2];
        if cursor.read_exact(&mut op).is_ok() {
          let target = u16::from_le_bytes([op[0], op[1]]);
          targets.insert(target);
        }
      }
      0xA9 | 0xA2 | 0xA0 | 0xA5 | 0xA6 | 0x85 | 0x84 | 0x86 | 0xB1 => {
        cursor.seek(SeekFrom::Current(1)).ok();
      }
      0x8D => {
        cursor.seek(SeekFrom::Current(2)).ok();
      }
      _ => {}
    }
  }
  
  // assign labels in address order so L0 is always the lowest address
  targets
  .into_iter()
  .enumerate()
  .map(|(i, addr)| (addr, format!("L{}", i)))
  .collect()
}

pub fn disassembler(bytes: Vec<u8>, mem_map: Memory_Map) {
  if bytes.len() <= VECTOR_TABLE_SIZE {
    return;
  }
  
  let code_bytes = &bytes[..bytes.len() - VECTOR_TABLE_SIZE];
  
  let meaningful_len = code_bytes
  .iter()
  .rposition(|&b| b != 0xEA)
  .map(|i| i + 1)
  .unwrap_or(0);
  
  let code = &code_bytes[..meaningful_len];
  let labels = collect_jump_targets(code);
  
  let mut cursor = Cursor::new(code);
  let mut buffer = [0u8; 1];
  
  while cursor.read(&mut buffer).unwrap_or(0) > 0 {
    let addr = ((cursor.position() - 1) as u16) + mem_map.rom_start;
    
    if let Some(label) = labels.get(&addr) {
      println!("\n{}:", label);
    }
    
    match buffer[0] {
      0xEA => print_op("NOP", vec![], addr, AddrMode::Implied, &labels),
      0x78 => print_op("SEI", vec![], addr, AddrMode::Implied, &labels),
      0xD8 => print_op("CLD", vec![], addr, AddrMode::Implied, &labels),
      0xBA => print_op("TSX", vec![], addr, AddrMode::Implied, &labels),
      0x9A => print_op("TXS", vec![], addr, AddrMode::Implied, &labels),
      0x48 => print_op("PHA", vec![], addr, AddrMode::Implied, &labels),
      0x68 => print_op("PLA", vec![], addr, AddrMode::Implied, &labels),
      0x60 => print_op("RTS", vec![], addr, AddrMode::Implied, &labels),
      
      0xA9 | 0xA2 | 0xA0 => {
        let mut op = [0u8; 1];
        cursor.read_exact(&mut op).unwrap();
        let mnemonic = match buffer[0] {
          0xA9 => "LDA", 0xA2 => "LDX", 0xA0 => "LDY", _ => unreachable!(),
        };
        print_op(mnemonic, vec![op[0]], addr, AddrMode::Immediate, &labels);
      }
      
      0xA5 | 0xA6 | 0x85 | 0x84 | 0x86 => {
        let mut op = [0u8; 1];
        cursor.read_exact(&mut op).unwrap();
        let mnemonic = match buffer[0] {
          0xA5 => "LDA", 0xA6 => "LDX", 0x85 => "STA", 0x84 => "STY", 0x86 => "STX", _ => unreachable!(),
        };
        print_op(mnemonic, vec![op[0]], addr, AddrMode::ZeroPage, &labels);
      }
      
      0x8D | 0x20 | 0x4C => {
        let mut op = [0u8; 2];
        cursor.read_exact(&mut op).unwrap();
        let mnemonic = match buffer[0] {
          0x8D => "STA", 0x20 => "JSR", 0x4C => "JMP", _ => unreachable!(),
        };
        print_op(mnemonic, vec![op[0], op[1]], addr, AddrMode::Absolute, &labels);
      }
      
      0xB1 => {
        let mut op = [0u8; 1];
        cursor.read_exact(&mut op).unwrap();
        print_op("LDA", vec![op[0]], addr, AddrMode::IndirectY, &labels);
      }
      
      _ => println!("{:04X}: Unknown Opcode: {:02X}", addr, buffer[0]),
    }
  }
}