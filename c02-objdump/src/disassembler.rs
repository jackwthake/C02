use std::collections::HashMap;
use std::io::{Cursor, Read};

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
  ZeroPageIndirect,
  AbsoluteXIndirect,
  ZPGRelative,
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
    AddrMode::ZeroPageIndirect => format!("{} (${:02X})", op, args[0]),
    AddrMode::AbsoluteXIndirect => format!("{} (${:04X},X)", op, u16::from_le_bytes([args[0], args[1]])),
    AddrMode::ZPGRelative => format!("{} ${:02X},${:02X}", op, args[0], args[1]),
  };
  
  println!("{:04X}: {}", addr, formatted);
}

// Total instruction size (opcode + operand bytes) for every 65C02 opcode.
// 1 = implied/accumulator, 2 = immediate/zeropage/relative/indirect, 3 = absolute/BBR/BBS.
// Undefined opcodes default to 1 (W65C02S single-byte NOPs).
fn instruction_size(opcode: u8) -> usize {
  #[rustfmt::skip]
  static SIZES: [u8; 256] = [
  // x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF
      1, 2, 1, 1, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 3, // 0x: BRK ORA . . TSB ORA ASL RMB PHP ORA ASL . TSB ORA ASL BBR
      2, 2, 2, 1, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 3, // 1x: BPL ORA ORA . TRB ORA ASL RMB CLC ORA INC . TRB ORA ASL BBR
      3, 2, 1, 1, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 3, // 2x: JSR AND . . BIT AND ROL RMB PLP AND ROL . BIT AND ROL BBR
      2, 2, 2, 1, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 3, // 3x: BMI AND AND . BIT AND ROL RMB SEC AND DEC . BIT AND ROL BBR
      1, 2, 1, 1, 1, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 3, // 4x: RTI EOR . . . EOR LSR RMB PHA EOR LSR . JMP EOR LSR BBR
      2, 2, 2, 1, 1, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 3, // 5x: BVC EOR EOR . . EOR LSR RMB CLI EOR PHY . . EOR LSR BBR
      1, 2, 1, 1, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 3, // 6x: RTS ADC . . STZ ADC ROR RMB PLA ADC ROR . JMP ADC ROR BBR
      2, 2, 2, 1, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 3, // 7x: BVS ADC ADC . STZ ADC ROR RMB SEI ADC PLY . JMP ADC ROR BBR
      2, 2, 1, 1, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 3, // 8x: BRA STA . . STY STA STX SMB DEY BIT TXA . STY STA STX BBS
      2, 2, 2, 1, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 3, // 9x: BCC STA STA . STY STA STX SMB TYA STA TXS . STZ STA STZ BBS
      2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 3, // Ax: LDY LDA LDX . LDY LDA LDX SMB TAY LDA TAX . LDY LDA LDX BBS
      2, 2, 2, 1, 2, 2, 2, 2, 1, 3, 1, 1, 3, 3, 3, 3, // Bx: BCS LDA LDA . LDY LDA LDX SMB CLV LDA TSX . LDY LDA LDX BBS
      2, 2, 1, 1, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3, // Cx: CPY CMP . . CPY CMP DEC SMB INY CMP DEX WAI CPY CMP DEC BBS
      2, 2, 2, 1, 1, 2, 2, 2, 1, 3, 1, 2, 1, 3, 3, 3, // Dx: BNE CMP CMP . . CMP DEC SMB CLD CMP PHX STP . CMP DEC BBS
      2, 2, 1, 1, 2, 2, 2, 2, 1, 2, 1, 1, 3, 3, 3, 3, // Ex: CPX SBC . . CPX SBC INC SMB INX SBC NOP . CPX SBC INC BBS
      2, 2, 2, 1, 1, 2, 2, 2, 1, 3, 1, 2, 1, 3, 3, 3, // Fx: BEQ SBC SBC . . SBC INC SMB SED SBC PLX . . SBC INC BBS
  ];
  SIZES[opcode as usize] as usize
}


fn collect_jump_targets(bytes: &[u8]) -> HashMap<u16, String> {
  let mut targets: std::collections::BTreeSet<u16> = std::collections::BTreeSet::new();
  let mut i = 0;

  while i < bytes.len() {
    let opcode = bytes[i];
    let size = instruction_size(opcode);

    if (opcode == 0x20 || opcode == 0x4C) && i + 2 < bytes.len() {
      let target = u16::from_le_bytes([bytes[i + 1], bytes[i + 2]]);
      targets.insert(target);
    }

    i += size;
  }

  targets
  .into_iter()
  .enumerate()
  .map(|(i, addr)| (addr, format!("L{}", i)))
  .collect()
}

pub enum Mode {
  CodeOnly,
  All,
}

struct RomInfo {
  base_addr: u16,
  code_end: usize,
  data_end: usize,
  has_boundary: bool,
  nmi: u16,
  reset: u16,
  irq: u16,
}

fn parse_rom(bytes: &[u8]) -> Option<RomInfo> {
  if bytes.len() <= VECTOR_TABLE_SIZE {
    return None;
  }

  let nmi   = u16::from_le_bytes([bytes[bytes.len() - 6], bytes[bytes.len() - 5]]);
  let reset = u16::from_le_bytes([bytes[bytes.len() - 4], bytes[bytes.len() - 3]]);
  let irq   = u16::from_le_bytes([bytes[bytes.len() - 2], bytes[bytes.len() - 1]]);
  let base_addr = reset;

  let code_bytes = &bytes[..bytes.len() - VECTOR_TABLE_SIZE];

  let code_boundary = u16::from_le_bytes([
    bytes[bytes.len() - 8],
    bytes[bytes.len() - 7],
  ]);

  let has_boundary = code_boundary >= base_addr && code_boundary < 0xFFF0;

  let code_end = if has_boundary {
    (code_boundary - base_addr) as usize
  } else {
    code_bytes
      .iter()
      .rposition(|&b| b != 0xEA)
      .map(|i| i + 1)
      .unwrap_or(0)
  };

  let data_end = if has_boundary {
    let marker_pos = bytes.len() - 8;
    bytes[code_end..marker_pos]
      .iter()
      .rposition(|&b| b != 0xEA)
      .map(|i| code_end + i + 1)
      .unwrap_or(code_end)
  } else {
    code_end
  };

  Some(RomInfo { base_addr, code_end, data_end, has_boundary, nmi, reset, irq })
}

pub fn dump_sections(bytes: &[u8]) {
  let info = match parse_rom(bytes) {
    Some(i) => i,
    None => { eprintln!("ROM too small"); return; }
  };

  let rom_size = bytes.len();
  let base = info.base_addr;

  println!("ROM size:  {} bytes ({:#06X})", rom_size, rom_size);
  println!();
  println!("  .text    ${:04X}–${:04X}  ({} bytes)",
    base, base + info.code_end as u16 - 1, info.code_end);

  if info.has_boundary && info.data_end > info.code_end {
    println!("  .data    ${:04X}–${:04X}  ({} bytes)",
      base + info.code_end as u16,
      base + info.data_end as u16 - 1,
      info.data_end - info.code_end);
  }

  println!("  vectors  $FFFA–$FFFF  (6 bytes)");
  println!();
  println!("  NMI      ${:04X}", info.nmi);
  println!("  Reset    ${:04X}", info.reset);
  println!("  IRQ      ${:04X}", info.irq);
}

fn hex_dump(bytes: &[u8], base_addr: u16, offset: usize, len: usize) {
  let end = (offset + len).min(bytes.len());
  let mut i = offset;
  while i < end {
    let addr = base_addr.wrapping_add(i as u16);
    print!("{:04X}:", addr);
    let row_end = (i + 16).min(end);
    for j in i..row_end {
      if j % 8 == 0 && j != i { print!(" "); }
      print!(" {:02X}", bytes[j]);
    }
    // ascii column
    print!("  ");
    for j in i..row_end {
      let ch = bytes[j];
      if ch.is_ascii_graphic() || ch == b' ' {
        print!("{}", ch as char);
      } else {
        print!(".");
      }
    }
    println!();
    i = row_end;
  }
}

pub fn disassemble(bytes: &[u8], mode: Mode) {
  let info = match parse_rom(bytes) {
    Some(i) => i,
    None => return,
  };

  let code = &bytes[..info.code_end];
  let labels = collect_jump_targets(code);

  let mut cursor = Cursor::new(code);
  let mut buffer = [0u8; 1];

  while cursor.read(&mut buffer).unwrap_or(0) > 0 {
    let addr = (cursor.position().saturating_sub(1)) as u16;
    let abs_addr = info.base_addr.wrapping_add(addr);

    if let Some(label) = labels.get(&abs_addr) {
      println!("\n{}:", label);
    }

    match buffer[0] {
      // 00-0F: Control & Logic Group
      0x00 => print_op("BRK", vec![], addr, AddrMode::Implied, &labels),
      0x01 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ORA", vec![op[0]], addr, AddrMode::IndirectX, &labels); },
      0x04 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("TSB", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x05 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ORA", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x06 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ASL", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x07 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("RMB0", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x08 => print_op("PHP", vec![], addr, AddrMode::Implied, &labels),
      0x09 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ORA", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0x0A => print_op("ASL", vec![], addr, AddrMode::Implied, &labels),
      0x0C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("TSB", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); }, // W65C02
      0x0D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ORA", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x0E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ASL", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x0F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBR0", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // 10-1F: Branch & Logic Group
      0x10 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BPL", vec![op[0]], addr, AddrMode::Relative, &labels); },
      0x11 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ORA", vec![op[0]], addr, AddrMode::IndirectY, &labels); },
      0x12 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ORA", vec![op[0]], addr, AddrMode::ZeroPageIndirect, &labels); }, // W65C02
      0x14 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("TRB", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x15 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ORA", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x16 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ASL", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x17 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("RMB1", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x18 => print_op("CLC", vec![], addr, AddrMode::Implied, &labels),
      0x19 => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ORA", vec![op[0], op[1]], addr, AddrMode::AbsoluteY, &labels); },
      0x1A => print_op("INC", vec![], addr, AddrMode::Implied, &labels), // W65C02 (accumulator)
      0x1C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("TRB", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); }, // W65C02
      0x1D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ORA", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0x1E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ASL", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0x1F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBR1", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // 20-2F: Logic & Stack Group
      0x20 => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("JSR", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x21 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("AND", vec![op[0]], addr, AddrMode::IndirectX, &labels); },
      0x24 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BIT", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x25 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("AND", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x26 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ROL", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x27 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("RMB2", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x28 => print_op("PLP", vec![], addr, AddrMode::Implied, &labels),
      0x29 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("AND", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0x2A => print_op("ROL", vec![], addr, AddrMode::Implied, &labels),
      0x2C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("BIT", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x2D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("AND", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x2E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ROL", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x2F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBR2", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // 30-3F: Branch & Logic Group
      0x30 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BMI", vec![op[0]], addr, AddrMode::Relative, &labels); },
      0x31 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("AND", vec![op[0]], addr, AddrMode::IndirectY, &labels); },
      0x32 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("AND", vec![op[0]], addr, AddrMode::ZeroPageIndirect, &labels); }, // W65C02
      0x34 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BIT", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); }, // W65C02
      0x35 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("AND", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x36 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ROL", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x37 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("RMB3", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x38 => print_op("SEC", vec![], addr, AddrMode::Implied, &labels),
      0x39 => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("AND", vec![op[0], op[1]], addr, AddrMode::AbsoluteY, &labels); },
      0x3A => print_op("DEC", vec![], addr, AddrMode::Implied, &labels), // W65C02 (accumulator)
      0x3C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("BIT", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); }, // W65C02
      0x3D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("AND", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0x3E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ROL", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0x3F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBR3", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // 40-4F: EOR & Control Group
      0x40 => print_op("RTI", vec![], addr, AddrMode::Implied, &labels),
      0x41 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("EOR", vec![op[0]], addr, AddrMode::IndirectX, &labels); },
      0x45 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("EOR", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x46 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LSR", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x47 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("RMB4", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x48 => print_op("PHA", vec![], addr, AddrMode::Implied, &labels),
      0x49 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("EOR", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0x4A => print_op("LSR", vec![], addr, AddrMode::Implied, &labels),
      0x4C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("JMP", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x4D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("EOR", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x4E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("LSR", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x4F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBR4", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // 50-5F: Branch & Logic Group
      0x50 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BVC", vec![op[0]], addr, AddrMode::Relative, &labels); },
      0x51 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("EOR", vec![op[0]], addr, AddrMode::IndirectY, &labels); },
      0x52 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("EOR", vec![op[0]], addr, AddrMode::ZeroPageIndirect, &labels); }, // W65C02
      0x55 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("EOR", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x56 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LSR", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x57 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("RMB5", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x58 => print_op("CLI", vec![], addr, AddrMode::Implied, &labels),
      0x59 => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("EOR", vec![op[0], op[1]], addr, AddrMode::AbsoluteY, &labels); },
      0x5A => print_op("PHY", vec![], addr, AddrMode::Implied, &labels), // W65C02
      0x5C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("PHZ", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); }, // 65C02 (placeholder)
      0x5D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("EOR", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0x5E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("LSR", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0x5F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBR5", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // 60-6F: Return & ADC Group
      0x60 => print_op("RTS", vec![], addr, AddrMode::Implied, &labels),
      0x61 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ADC", vec![op[0]], addr, AddrMode::IndirectX, &labels); },
      0x64 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STZ", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x65 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ADC", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x66 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ROR", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x67 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("RMB6", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x68 => print_op("PLA", vec![], addr, AddrMode::Implied, &labels),
      0x69 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ADC", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0x6A => print_op("ROR", vec![], addr, AddrMode::Implied, &labels),
      0x6C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("JMP", vec![op[0], op[1]], addr, AddrMode::Indirect, &labels); },
      0x6D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ADC", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x6E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ROR", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x6F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBR6", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // 70-7F: Branch & ADC Group
      0x70 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BVS", vec![op[0]], addr, AddrMode::Relative, &labels); },
      0x71 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ADC", vec![op[0]], addr, AddrMode::IndirectY, &labels); },
      0x72 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ADC", vec![op[0]], addr, AddrMode::ZeroPageIndirect, &labels); }, // W65C02
      0x74 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STZ", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); }, // W65C02
      0x75 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ADC", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x76 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("ROR", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x77 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("RMB7", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x78 => print_op("SEI", vec![], addr, AddrMode::Implied, &labels),
      0x79 => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ADC", vec![op[0], op[1]], addr, AddrMode::AbsoluteY, &labels); },
      0x7A => print_op("PLY", vec![], addr, AddrMode::Implied, &labels), // W65C02
      0x7C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("JMP", vec![op[0], op[1]], addr, AddrMode::AbsoluteXIndirect, &labels); }, // W65C02
      0x7D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ADC", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0x7E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("ROR", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0x7F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBR7", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // 80-8F: Branch & STA Group
      0x80 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BRA", vec![op[0]], addr, AddrMode::Relative, &labels); }, // W65C02
      0x81 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STA", vec![op[0]], addr, AddrMode::IndirectX, &labels); },
      0x84 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STY", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x85 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STA", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x86 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STX", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0x87 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SMB0", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x88 => print_op("DEY", vec![], addr, AddrMode::Implied, &labels),
      0x89 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BIT", vec![op[0]], addr, AddrMode::Immediate, &labels); }, // W65C02
      0x8A => print_op("TXA", vec![], addr, AddrMode::Implied, &labels),
      0x8C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("STY", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x8D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("STA", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x8E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("STX", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0x8F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBS0", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // 90-9F: Branch & STA Group
      0x90 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BCC", vec![op[0]], addr, AddrMode::Relative, &labels); },
      0x91 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STA", vec![op[0]], addr, AddrMode::IndirectY, &labels); },
      0x92 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STA", vec![op[0]], addr, AddrMode::ZeroPageIndirect, &labels); }, // W65C02
      0x94 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STY", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x95 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STA", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0x96 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STX", vec![op[0]], addr, AddrMode::ZeroPageY, &labels); },
      0x97 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SMB1", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0x98 => print_op("TYA", vec![], addr, AddrMode::Implied, &labels),
      0x99 => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("STA", vec![op[0], op[1]], addr, AddrMode::AbsoluteY, &labels); },
      0x9A => print_op("TXS", vec![], addr, AddrMode::Implied, &labels),
      0x9C => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("STZ", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); }, // W65C02
      0x9D => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("STA", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0x9E => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("STZ", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); }, // W65C02
      0x9F => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBS1", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // A0-AF: Load Group
      0xA0 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDY", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0xA1 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDA", vec![op[0]], addr, AddrMode::IndirectX, &labels); },
      0xA2 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDX", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0xA4 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDY", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0xA5 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDA", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0xA6 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDX", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0xA7 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SMB2", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0xA8 => print_op("TAY", vec![], addr, AddrMode::Implied, &labels),
      0xA9 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDA", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0xAA => print_op("TAX", vec![], addr, AddrMode::Implied, &labels),
      0xAC => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("LDY", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0xAD => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("LDA", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0xAE => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("LDX", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0xAF => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBS2", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // B0-BF: Branch & Load Group
      0xB0 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BCS", vec![op[0]], addr, AddrMode::Relative, &labels); },
      0xB1 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDA", vec![op[0]], addr, AddrMode::IndirectY, &labels); },
      0xB2 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDA", vec![op[0]], addr, AddrMode::ZeroPageIndirect, &labels); }, // W65C02
      0xB4 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDY", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0xB5 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDA", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0xB6 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("LDX", vec![op[0]], addr, AddrMode::ZeroPageY, &labels); },
      0xB7 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SMB3", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0xB8 => print_op("CLV", vec![], addr, AddrMode::Implied, &labels),
      0xB9 => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("LDA", vec![op[0], op[1]], addr, AddrMode::AbsoluteY, &labels); },
      0xBA => print_op("TSX", vec![], addr, AddrMode::Implied, &labels),
      0xBC => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("LDY", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0xBD => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("LDA", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0xBE => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("LDX", vec![op[0], op[1]], addr, AddrMode::AbsoluteY, &labels); },
      0xBF => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBS3", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // C0-CF: Compare & Decrement Group
      0xC0 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CPY", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0xC1 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CMP", vec![op[0]], addr, AddrMode::IndirectX, &labels); },
      0xC4 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CPY", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0xC5 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CMP", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0xC6 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("DEC", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0xC7 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SMB4", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0xC8 => print_op("INY", vec![], addr, AddrMode::Implied, &labels),
      0xC9 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CMP", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0xCA => print_op("DEX", vec![], addr, AddrMode::Implied, &labels),
      0xCB => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("WAI", vec![op[0]], addr, AddrMode::Immediate, &labels); }, // W65C02 (placeholder for WAI)
      0xCC => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("CPY", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0xCD => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("CMP", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0xCE => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("DEC", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0xCF => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBS4", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // D0-DF: Branch & SBC Group
      0xD0 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BNE", vec![op[0]], addr, AddrMode::Relative, &labels); },
      0xD1 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CMP", vec![op[0]], addr, AddrMode::IndirectY, &labels); },
      0xD2 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CMP", vec![op[0]], addr, AddrMode::ZeroPageIndirect, &labels); }, // W65C02
      0xD5 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CMP", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0xD6 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("DEC", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0xD7 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SMB5", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0xD8 => print_op("CLD", vec![], addr, AddrMode::Implied, &labels),
      0xD9 => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("CMP", vec![op[0], op[1]], addr, AddrMode::AbsoluteY, &labels); },
      0xDA => print_op("PHX", vec![], addr, AddrMode::Implied, &labels), // W65C02
      0xDB => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("STP", vec![op[0]], addr, AddrMode::Immediate, &labels); }, // W65C02 (placeholder for STP)
      0xDD => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("CMP", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0xDE => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("DEC", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0xDF => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBS5", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // E0-EF: Compare, Increment & SBC Group
      0xE0 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CPX", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0xE1 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SBC", vec![op[0]], addr, AddrMode::IndirectX, &labels); },
      0xE4 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("CPX", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0xE5 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SBC", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0xE6 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("INC", vec![op[0]], addr, AddrMode::ZeroPage, &labels); },
      0xE7 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SMB6", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0xE8 => print_op("INX", vec![], addr, AddrMode::Implied, &labels),
      0xE9 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SBC", vec![op[0]], addr, AddrMode::Immediate, &labels); },
      0xEA => print_op("NOP", vec![], addr, AddrMode::Implied, &labels),
      0xEC => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("CPX", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0xED => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("SBC", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0xEE => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("INC", vec![op[0], op[1]], addr, AddrMode::Absolute, &labels); },
      0xEF => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBS6", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      // F0-FF: Branch & SBC Group
      0xF0 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("BEQ", vec![op[0]], addr, AddrMode::Relative, &labels); },
      0xF1 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SBC", vec![op[0]], addr, AddrMode::IndirectY, &labels); },
      0xF2 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SBC", vec![op[0]], addr, AddrMode::ZeroPageIndirect, &labels); }, // W65C02
      0xF5 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SBC", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0xF6 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("INC", vec![op[0]], addr, AddrMode::ZeroPageX, &labels); },
      0xF7 => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("SMB7", vec![op[0]], addr, AddrMode::ZeroPage, &labels); }, // W65C02
      0xF8 => print_op("SED", vec![], addr, AddrMode::Implied, &labels),
      0xF9 => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("SBC", vec![op[0], op[1]], addr, AddrMode::AbsoluteY, &labels); },
      0xFA => print_op("PLX", vec![], addr, AddrMode::Implied, &labels), // W65C02
      0xFB => { let mut op = [0u8; 1]; cursor.read_exact(&mut op).unwrap(); print_op("PLZ", vec![op[0]], addr, AddrMode::Immediate, &labels); }, // Placeholder
      0xFD => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("SBC", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0xFE => { let mut op = [0u8; 2]; cursor.read_exact(&mut op).unwrap(); print_op("INC", vec![op[0], op[1]], addr, AddrMode::AbsoluteX, &labels); },
      0xFF => { let mut op = [0u8; 3]; cursor.read_exact(&mut op).unwrap(); print_op("BBS7", vec![op[0], op[1]], addr, AddrMode::ZPGRelative, &labels); }, // W65C02
      
      _ => println!("{:04X}: Unknown Opcode: {:02X}", addr, buffer[0]),
    }
  }

  if let Mode::All = mode {
    if info.has_boundary && info.data_end > info.code_end {
      let data_len = info.data_end - info.code_end;
      println!("\n.data  ${:04X}–${:04X}  ({} bytes)\n",
        info.base_addr + info.code_end as u16,
        info.base_addr + info.data_end as u16 - 1,
        data_len);
      hex_dump(bytes, info.base_addr, info.code_end, data_len);
    }
  }
}

pub fn dump_size(bytes: &[u8]) {
  let info = match parse_rom(bytes) {
    Some(i) => i,
    None => { eprintln!("ROM too small"); return; }
  };

  let rom_size = bytes.len();
  let code_bytes = info.code_end;
  let data_bytes = info.data_end - info.code_end;
  let used = info.data_end + VECTOR_TABLE_SIZE + 2;
  let pct = (used as f64 / rom_size as f64 * 100.0) as u32;

  println!("Program uses {} bytes ({}%) of ROM space. Maximum is {} bytes.",
    used, pct, rom_size);
  println!("  .text: {} bytes, .data: {} bytes, vectors: 8 bytes",
    code_bytes, data_bytes);
}


pub fn dump_data(bytes: &[u8]) {
  let info = match parse_rom(bytes) {
    Some(i) => i,
    None => return,
  };

  if info.has_boundary && info.data_end > info.code_end {
    let data_len = info.data_end - info.code_end;
    println!(".data  ${:04X}–${:04X}  ({} bytes)\n",
      info.base_addr + info.code_end as u16,
      info.base_addr + info.data_end as u16 - 1,
      data_len);
    hex_dump(bytes, info.base_addr, info.code_end, data_len);
  } else {
    println!("No data section found.");
  }
}