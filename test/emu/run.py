#!/usr/bin/env python3
#
# run.py - emulator-based runtime tests for the c02 code generator (c02-as).
#
# Each *.c02 file in this directory is a self-contained test program. It is
# compiled end-to-end through the c02c driver (c02-frontend -> c02-as) into a
# 32 KB 6502 ROM, loaded into a py65 65C02 emulator, and executed from the
# reset vector until the generated halt loop (JMP-to-self after main returns)
# is reached. Expectations are then checked against final CPU/memory state.
#
# Because `main` must be `void` (SPEC 7.4), programs report results the way
# real hardware would: by writing to memory-mapped registers. The canonical
# observation channel is a `reg` declared at $6000 (mirroring the Ben Eater
# VIA PORTB in docs/memmap.md); the harness reads that address back after halt.
#
# EXPECTATIONS
# ------------
# Each program embeds directives in `//` comments:
#
#   // EXPECT: <target> == <value>      assertion, checked after halt
#   // CYCLES: <n>                       max instructions to execute (default 100k)
#   // DESC:   <text>                    human description (else first comment line)
#
# <target> is one of:
#   mem8[A]   / mem16[A]    a byte / little-endian word in final RAM/IO state
#   rom8[A]   / rom16[A]    a byte / LE word in the ROM image (A is absolute)
#   rom_size               the ROM file size in bytes (structural check)
#   sp / a / x / y         final CPU register
#   trace8[A]              the ordered list of every byte written to A while running
#
# <value> is a decimal or 0x-hex integer, a 'c' char literal, a [list, ...],
# or a "string" (for trace8, compared as the byte sequence). trace8 also
# supports `endswith` in place of `==`.
#
# Usage:
#   tests/emu/run.py                 build (via make) then run every test
#   tests/emu/run.py add sub         run only tests whose name contains a filter
#   tests/emu/run.py --no-build      use the current bin/ as-is
#   tests/emu/run.py --ir-on-fail    dump the IR (c02-as --dump-ir) for failures
#   tests/emu/run.py -v              show every assertion, not just failures
import argparse
import ast
import os
import re
import subprocess
import sys
import tempfile

try:
    from py65.devices.mpu65c02 import MPU as MPU65C02
except ImportError:
    sys.exit("py65 is required for emulator tests: pip install py65")

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
BIN_DIR = os.path.join(REPO_ROOT, "bin")
DRIVER = os.path.join(BIN_DIR, "c02c")
FRONTEND = os.path.join(BIN_DIR, "c02-frontend")
CODEGEN = os.path.join(BIN_DIR, "c02-as")

# Must match c02-as/src/generator.c.
ROM_START = 0x8000
ROM_SIZE = 0x8000
DEFAULT_CYCLES = 100_000

# --- terminal styling -------------------------------------------------------
_USE_COLOR = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _USE_COLOR else text


PASS = _c("32", "PASS")
FAIL = _c("31", "FAIL")


# --- directive parsing ------------------------------------------------------

class Directives:
    def __init__(self, desc, cycles, expects):
        self.desc = desc
        self.cycles = cycles
        self.expects = expects  # list of (raw, target, op, value)


def _parse_value(text):
    """A directive RHS: int (dec/hex), 'c' char, [list], or "string"."""
    text = text.strip()
    if text.startswith('"') or text.startswith('['):
        return ast.literal_eval(text)
    if len(text) == 3 and text[0] == "'" and text[2] == "'":
        return ord(text[1])
    return int(text, 0)


def parse_directives(path):
    desc = None
    cycles = DEFAULT_CYCLES
    expects = []
    first_comment = None
    for line in open(path):
        s = line.strip()
        if not s.startswith("//"):
            continue
        body = s[2:].strip()
        if first_comment is None and body:
            first_comment = body
        m = re.match(r"EXPECT:?\s+(.*)", body)
        if m:
            rest = m.group(1)
            # split on the operator, keeping it
            om = re.match(r"(.+?)\s+(==|!=|<=|>=|<|>|endswith)\s+(.+)", rest)
            if not om:
                raise ValueError(f"{path}: malformed EXPECT: {body!r}")
            target, op, val = om.group(1).strip(), om.group(2), om.group(3)
            expects.append((rest, target, op, _parse_value(val)))
            continue
        m = re.match(r"CYCLES:?\s+(.*)", body)
        if m:
            cycles = int(m.group(1), 0)
            continue
        m = re.match(r"DESC:?\s+(.*)", body)
        if m:
            desc = m.group(1)
    return Directives(desc or first_comment or "", cycles, expects)


# --- compile + run ----------------------------------------------------------

def compile_rom(src, out):
    r = subprocess.run([DRIVER, src, "-o", out], capture_output=True, text=True)
    return r.returncode == 0, r.stderr


class TracingMemory(list):
    """A 64 KB memory that records every byte written to watched addresses."""
    def __init__(self, watch):
        super().__init__([0] * 0x10000)
        self._watch = watch
        self.trace = {a: [] for a in watch}

    def __setitem__(self, addr, val):
        if addr in self._watch:
            self.trace[addr].append(val & 0xFF)
        super().__setitem__(addr, val)


def run_rom(rom, watch, max_cycles):
    mpu = MPU65C02()
    mem = TracingMemory(watch)
    for i, b in enumerate(rom[:ROM_SIZE]):
        mem[ROM_START + i] = b
    mpu.memory = mem
    mpu.pc = mem[0xFFFC] | (mem[0xFFFD] << 8)
    halted = False
    for _ in range(max_cycles):
        old = mpu.pc
        mpu.step()
        if mpu.pc == old:      # JMP-to-self halt loop
            halted = True
            break
    return mpu, mem.trace, halted


# --- assertion evaluation ---------------------------------------------------

_CMP = {
    "==": lambda a, b: a == b,
    "!=": lambda a, b: a != b,
    "<":  lambda a, b: a < b,
    ">":  lambda a, b: a > b,
    "<=": lambda a, b: a <= b,
    ">=": lambda a, b: a >= b,
}


def _target_value(target, rom, mpu, trace):
    m = re.match(r"(mem8|mem16|rom8|rom16|trace8)\[(.+)\]$", target)
    if m:
        kind, addr = m.group(1), int(m.group(2), 0)
        if kind == "mem8":
            return mpu.memory[addr]
        if kind == "mem16":
            return mpu.memory[addr] | (mpu.memory[addr + 1] << 8)
        if kind == "rom8":
            return rom[addr - ROM_START]
        if kind == "rom16":
            return rom[addr - ROM_START] | (rom[addr - ROM_START + 1] << 8)
        if kind == "trace8":
            return trace.get(addr, [])
    if target == "rom_size":
        return len(rom)
    if target in ("sp", "a", "x", "y"):
        return getattr(mpu, target)
    raise ValueError(f"unknown EXPECT target: {target!r}")


def eval_expect(raw, target, op, expected, rom, mpu, trace):
    actual = _target_value(target, rom, mpu, trace)
    if op == "endswith":
        exp = list(expected) if isinstance(expected, (list, tuple)) else \
              [ord(c) for c in expected]
        ok = list(actual)[-len(exp):] == exp
    else:
        if isinstance(expected, str):
            expected = [ord(c) for c in expected]
        ok = _CMP[op](actual, expected)
    return ok, actual


def _fmt(v):
    if isinstance(v, int):
        return f"{v} (0x{v:X})"
    return repr(v)


# --- which targets need trace / running -------------------------------------

def watched_addrs(directives):
    addrs = set()
    for _, target, _, _ in directives.expects:
        m = re.match(r"trace8\[(.+)\]$", target)
        if m:
            addrs.add(int(m.group(1), 0))
    return addrs


def needs_run(directives):
    # A test needs execution unless every target is purely structural (rom*/rom_size).
    for _, target, _, _ in directives.expects:
        if not (target.startswith("rom8") or target.startswith("rom16")
                or target == "rom_size"):
            return True
    return False


# --- per-file driver --------------------------------------------------------

def run_test(path, ir_on_fail, verbose):
    name = os.path.splitext(os.path.basename(path))[0]
    directives = parse_directives(path)
    if not directives.expects:
        return name, None, ["no EXPECT directives"], directives.desc

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
        bin_path = tmp.name
    try:
        ok, stderr = compile_rom(path, bin_path)
        if not ok:
            return name, False, [f"compile failed: {stderr.strip().splitlines()[-1] if stderr.strip() else '(no output)'}"], directives.desc
        rom = open(bin_path, "rb").read()

        mpu = trace = None
        halted = True
        if needs_run(directives):
            mpu, trace, halted = run_rom(rom, watched_addrs(directives),
                                         directives.cycles)

        failures = []
        for raw, target, op, expected in directives.expects:
            good, actual = eval_expect(raw, target, op, expected, rom, mpu, trace or {})
            if verbose or not good:
                mark = _c("32", "ok") if good else _c("31", "x ")
                print(f"      {mark} {raw}"
                      + ("" if good else f"   -> got {_fmt(actual)}"))
            if not good:
                failures.append(raw)
        if needs_run(directives) and not halted:
            failures.append(f"did not halt within {directives.cycles} instructions")

        if failures and ir_on_fail:
            _dump_ir(path)
        return name, (not failures), failures, directives.desc
    finally:
        if os.path.exists(bin_path):
            os.unlink(bin_path)


def _dump_ir(path):
    """Localize a failure: was it the frontend's lowering or c02-as's codegen?
    Emit the .o directly and print the IR c02-as actually consumed."""
    with tempfile.NamedTemporaryFile(suffix=".o", delete=False) as tmp:
        obj = tmp.name
    try:
        fe = subprocess.run([FRONTEND, "-o", obj, path],
                            capture_output=True, text=True)
        if fe.returncode != 0:
            print(_c("33", "      [frontend rejected it; codegen never ran]"))
            return
        ir = subprocess.run([CODEGEN, obj, "--dump-ir", "-o", os.devnull],
                            capture_output=True, text=True)
        print(_c("33", "      --- IR consumed by c02-as ---"))
        for ln in ir.stdout.splitlines():
            print(_c("33", "      | ") + ln)
    finally:
        if os.path.exists(obj):
            os.unlink(obj)


def build():
    r = subprocess.run(["make", "-C", REPO_ROOT], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit("build failed")


def main():
    ap = argparse.ArgumentParser(description="c02-as emulator test runner")
    ap.add_argument("filters", nargs="*", help="only run tests whose name contains one of these")
    ap.add_argument("--no-build", action="store_true", help="skip the make step")
    ap.add_argument("--ir-on-fail", action="store_true", help="dump IR for failing tests")
    ap.add_argument("-v", "--verbose", action="store_true", help="show passing assertions too")
    args = ap.parse_args()

    if not args.no_build:
        build()
    for tool in (DRIVER, FRONTEND, CODEGEN):
        if not os.access(tool, os.X_OK):
            sys.exit(f"missing {tool} (run 'make' first, or drop --no-build)")

    files = sorted(f for f in os.listdir(SCRIPT_DIR) if f.endswith(".c02"))
    if args.filters:
        files = [f for f in files if any(x in f for x in args.filters)]

    passed = failed = 0
    for f in files:
        name, ok, failures, desc = run_test(os.path.join(SCRIPT_DIR, f),
                                             args.ir_on_fail, args.verbose)
        if ok:
            print(f"  {PASS} {name}  {_c('90', desc)}")
            passed += 1
        else:
            print(f"  {FAIL} {name}  {_c('90', desc)}")
            for d in failures:
                print(f"         {d}")
            failed += 1

    total = passed + failed
    summary = f"\n{passed}/{total} passed"
    print(_c("32" if not failed else "31", summary))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
