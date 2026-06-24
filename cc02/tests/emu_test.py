#!/usr/bin/env python3
"""
Emulator-based runtime tests for the C02 code generator.

Compiles .c02 source files, loads the resulting binary into a py65 65C02
emulator, executes until the halt loop is reached, and checks assertions
against the final CPU/memory state.
"""
import subprocess
import os
import sys
import tempfile

try:
    from py65.devices.mpu65c02 import MPU as MPU65C02
except ImportError:
    print("py65 is required for emulator tests: pip install py65")
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CC02 = os.path.join(SCRIPT_DIR, "../../bin/cc02")

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

ROM_START = 0x8000
ROM_SIZE = 0x8000

# Zero-page layout (must match generator.c)
ZP_FP = 0x00
ZP_RET = 0x02


def compile_c02(source_path, bin_path):
    result = subprocess.run(
        [CC02, "-o", bin_path, source_path],
        capture_output=True, text=True
    )
    return result.returncode == 0, result.stderr


def load_and_run(bin_path, max_cycles=50000):
    mpu = MPU65C02()
    with open(bin_path, "rb") as f:
        rom = f.read()
    for i, byte in enumerate(rom):
        mpu.memory[ROM_START + i] = byte
    mpu.pc = mpu.memory[0xFFFC] | (mpu.memory[0xFFFD] << 8)
    for _ in range(max_cycles):
        old_pc = mpu.pc
        mpu.step()
        if mpu.pc == old_pc:
            break
    return mpu


def run_binary(bin_path, max_cycles=50000):
    return load_and_run(bin_path, max_cycles)


def compile_and_run(source_path, max_cycles=50000):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
        bin_path = tmp.name
    try:
        ok, stderr = compile_c02(source_path, bin_path)
        if not ok:
            return None, stderr
        return run_binary(bin_path, max_cycles), None
    finally:
        if os.path.exists(bin_path):
            os.unlink(bin_path)


# ----------------------------------------------------------------
# Test cases
# ----------------------------------------------------------------

def test_rom_size():
    """Binary is exactly 32K."""
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
        bin_path = tmp.name
    try:
        source = os.path.join(SCRIPT_DIR, "emu_store_const.c02")
        ok, _ = compile_c02(source, bin_path)
        if not ok:
            return False, "compilation failed"
        size = os.path.getsize(bin_path)
        if size != ROM_SIZE:
            return False, f"expected {ROM_SIZE} bytes, got {size}"
        return True, None
    finally:
        if os.path.exists(bin_path):
            os.unlink(bin_path)


def test_reset_vector():
    """Reset vector at $FFFC points to ROM_START."""
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
        bin_path = tmp.name
    try:
        source = os.path.join(SCRIPT_DIR, "emu_store_const.c02")
        ok, _ = compile_c02(source, bin_path)
        if not ok:
            return False, "compilation failed"
        with open(bin_path, "rb") as f:
            rom = bytearray(f.read())
        vec_lo = rom[0xFFFC - ROM_START]
        vec_hi = rom[0xFFFD - ROM_START]
        addr = vec_lo | (vec_hi << 8)
        if addr != ROM_START:
            return False, f"reset vector is ${addr:04X}, expected ${ROM_START:04X}"
        return True, None
    finally:
        if os.path.exists(bin_path):
            os.unlink(bin_path)


def test_nop_fill():
    """Unused ROM is filled with NOP ($EA)."""
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
        bin_path = tmp.name
    try:
        source = os.path.join(SCRIPT_DIR, "emu_store_const.c02")
        ok, _ = compile_c02(source, bin_path)
        if not ok:
            return False, "compilation failed"
        with open(bin_path, "rb") as f:
            rom = bytearray(f.read())
        # Check a region well past the bootstrap but before the vectors
        for i in range(0x100, 0x7FF0):
            if rom[i] != 0xEA:
                return False, f"rom[${ROM_START + i:04X}] = ${rom[i]:02X}, expected $EA"
                break
        return True, None
    finally:
        if os.path.exists(bin_path):
            os.unlink(bin_path)


def test_bootstrap_stack_init():
    """Bootstrap initializes the hardware stack pointer to $FF."""
    source = os.path.join(SCRIPT_DIR, "emu_store_const.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    if mpu.sp != 0xFF:
        return False, f"SP = ${mpu.sp:02X}, expected $FF"
    return True, None


def test_bootstrap_fp_init():
    """Bootstrap sets FP (ZP $00-$01) to $01FF."""
    source = os.path.join(SCRIPT_DIR, "emu_store_const.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    fp = mpu.memory[ZP_FP] | (mpu.memory[ZP_FP + 1] << 8)
    if fp != 0x01FF:
        return False, f"FP = ${fp:04X}, expected $01FF"
    return True, None


def test_halt_loop():
    """CPU halts (PC stuck on JMP to self) after main returns."""
    source = os.path.join(SCRIPT_DIR, "emu_store_const.c02")
    mpu, err = compile_and_run(source, max_cycles=1000)
    if mpu is None:
        return False, f"compilation failed: {err}"
    # Read the instruction at PC — should be JMP ($4C) to itself
    if mpu.memory[mpu.pc] != 0x4C:
        return False, f"expected JMP ($4C) at halt, got ${mpu.memory[mpu.pc]:02X}"
    target = mpu.memory[mpu.pc + 1] | (mpu.memory[mpu.pc + 2] << 8)
    if target != mpu.pc:
        return False, f"JMP target ${target:04X} != PC ${mpu.pc:04X}"
    return True, None


def test_interrupts_disabled():
    """Bootstrap disables interrupts (SEI sets I flag)."""
    source = os.path.join(SCRIPT_DIR, "emu_store_const.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    if not (mpu.p & 0x04):
        return False, "interrupt flag not set after SEI"
    return True, None


def test_decimal_mode_clear():
    """Bootstrap clears decimal mode (CLD clears D flag)."""
    source = os.path.join(SCRIPT_DIR, "emu_store_const.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    if mpu.p & 0x08:
        return False, "decimal flag still set after CLD"
    return True, None


def test_store_const_to_abs():
    """PORTB = 42 writes 42 to address $6000."""
    source = os.path.join(SCRIPT_DIR, "emu_store_const.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != 42:
        return False, f"memory[$6000] = {val}, expected 42"
    return True, None


def test_copy_var_to_abs():
    """u8 x = 42; PORTB = x; writes 42 to address $6000 via variable."""
    source = os.path.join(SCRIPT_DIR, "emu_copy_var.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != 42:
        return False, f"memory[$6000] = {val}, expected 42"
    return True, None


def test_u16_copy_and_return():
    """u16 x = 0x1234; return x; stores both bytes in RET."""
    source = os.path.join(SCRIPT_DIR, "emu_u16_return.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    lo = mpu.memory[ZP_RET]
    hi = mpu.memory[ZP_RET + 1]
    if lo != 0x34:
        return False, f"RET low = ${lo:02X}, expected $34"
    if hi != 0x12:
        return False, f"RET high = ${hi:02X}, expected $12"
    return True, None


def test_forward_jump():
    """if/else forward jump skips else body, PORTB gets if-body value."""
    source = os.path.join(SCRIPT_DIR, "emu_forward_jump.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != 42:
        return False, f"memory[$6000] = {val}, expected 42"
    return True, None


def _test_cmp(source_file, description):
    """Helper: compile an if/else comparison test, expect PORTB = 42."""
    def test():
        source = os.path.join(SCRIPT_DIR, source_file)
        mpu, err = compile_and_run(source)
        if mpu is None:
            return False, f"compilation failed: {err}"
        val = mpu.memory[0x6000]
        if val != 42:
            return False, f"memory[$6000] = {val}, expected 42"
        return True, None
    test.__doc__ = description
    return test


test_cmp_gt  = _test_cmp("emu_cmp_gt.c02",  "10 > 5 takes if-body")
test_cmp_gte = _test_cmp("emu_cmp_gte.c02", "10 >= 10 takes if-body (boundary)")
test_cmp_eq  = _test_cmp("emu_cmp_eq.c02",  "10 == 10 takes if-body")
test_cmp_neq = _test_cmp("emu_cmp_neq.c02", "10 != 5 takes if-body")
test_cmp_lte = _test_cmp("emu_cmp_lte.c02", "10 <= 10 takes if-body (boundary)")


def test_string_deref():
    """Global string pointer, loop with *p deref, writes chars to PORTB."""
    source = os.path.join(SCRIPT_DIR, "emu_string_deref.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != ord('!'):
        return False, f"memory[$6000] = ${val:02X}, expected ${ord('!'):02X} ('!')"
    return True, None


def test_inc_global():
    """Increment a global variable (u8 counter = 41; ++counter → 42)."""
    source = os.path.join(SCRIPT_DIR, "emu_inc_global.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != 42:
        return False, f"memory[$6000] = {val}, expected 42"
    return True, None


def test_cmp_global():
    """Compare local against global (x=10 > threshold=50 is false → else body)."""
    source = os.path.join(SCRIPT_DIR, "emu_cmp_global.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != 42:
        return False, f"memory[$6000] = {val}, expected 42"
    return True, None


def test_neg_u8():
    """i8 x=42; y=-x; z=-y → 42 (double negate round-trips)."""
    source = os.path.join(SCRIPT_DIR, "emu_neg_u8.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != 42:
        return False, f"memory[$6000] = {val}, expected 42"
    return True, None


def test_add_u8():
    """u8 x=10, y=32; PORTB = x+y → 42."""
    source = os.path.join(SCRIPT_DIR, "emu_add_u8.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != 42:
        return False, f"memory[$6000] = {val}, expected 42"
    return True, None


def test_sub_u8():
    """u8 x=50, y=8; PORTB = x-y → 42."""
    source = os.path.join(SCRIPT_DIR, "emu_sub_u8.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != 42:
        return False, f"memory[$6000] = {val}, expected 42"
    return True, None


def test_add_const():
    """u8 x=40; PORTB = x+2 → 42."""
    source = os.path.join(SCRIPT_DIR, "emu_add_const.c02")
    mpu, err = compile_and_run(source)
    if mpu is None:
        return False, f"compilation failed: {err}"
    val = mpu.memory[0x6000]
    if val != 42:
        return False, f"memory[$6000] = {val}, expected 42"
    return True, None


# ----------------------------------------------------------------
# Runner
# ----------------------------------------------------------------

TESTS = [
    ("rom_size", test_rom_size),
    ("reset_vector", test_reset_vector),
    ("nop_fill", test_nop_fill),
    ("bootstrap_stack_init", test_bootstrap_stack_init),
    ("bootstrap_fp_init", test_bootstrap_fp_init),
    ("halt_loop", test_halt_loop),
    ("interrupts_disabled", test_interrupts_disabled),
    ("decimal_mode_clear", test_decimal_mode_clear),
    ("store_const_to_abs", test_store_const_to_abs),
    ("copy_var_to_abs", test_copy_var_to_abs),
    ("u16_copy_and_return", test_u16_copy_and_return),
    ("forward_jump", test_forward_jump),
    ("cmp_gt", test_cmp_gt),
    ("cmp_gte", test_cmp_gte),
    ("cmp_eq", test_cmp_eq),
    ("cmp_neq", test_cmp_neq),
    ("cmp_lte", test_cmp_lte),
    ("string_deref", test_string_deref),
    ("inc_global", test_inc_global),
    ("cmp_global", test_cmp_global),
    ("neg_u8", test_neg_u8),
    ("add_u8", test_add_u8),
    ("sub_u8", test_sub_u8),
    ("add_const", test_add_const),
]


def main():
    passed = failed = 0
    for name, fn in TESTS:
        ok, detail = fn()
        if ok:
            print(f"  {PASS} {name}")
            passed += 1
        else:
            print(f"  {FAIL} {name}")
            print(f"         {detail}")
            failed += 1
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
