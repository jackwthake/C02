#!/usr/bin/env python3
#
# Linker (c02-ld) test suite.
#
# Each subdirectory of tests/ is one self-contained case made of:
#   *.c02   one or more module sources (separate translation units)
#   spec    directives describing how to link them and what to assert
#
# Everything is driven through bin/c02c (the toolchain driver) exactly as a
# user would invoke it — the frontend compiles each source to an object, then
# c02-ld merges them. Runtime assertions reuse the emulator harness's run_rom,
# so a linked program is proven by actually executing the ROM it produces.
#
# ── spec format ────────────────────────────────────────────────────────────
#   DESC: <one line>                 human-readable summary (required)
#   MODE: run | link | reject        what the case checks (required)
#   FILES: a.c02 b.c02               link order (optional; default: sorted *.c02)
#   PARTIAL: yes                     link with `-c` (no codegen, no main needed)
#
#   MODE run    — link + generate a ROM, run it, assert final machine state:
#       EXPECT: mem8[0x6000] == 42   (same target/op vocabulary as test/emu)
#
#   MODE reject — linking MUST fail; assert the diagnostic:
#       STDERR: Redefined function: foo   (substring of c02-ld's stderr)
#
#   MODE link   — linking must succeed; assert on the merged IR (--dump-ir):
#       IR_COUNT: <substring> == N   count of matching lines (e.g. dedup => 1)
#       IR_PRESENT: <substring>      substring must appear
#       IR_ABSENT: <substring>       substring must NOT appear
#     A `link` case with no IR_* directive just asserts the link succeeded.
#
# Run all:            python3 test/linker/run.py        (or `make linker-test`)
# Run one / verbose:  python3 test/linker/run.py -v reg_dedup

import os
import re
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
TESTS_DIR = os.path.join(SCRIPT_DIR, "tests")
ROOT = os.path.realpath(os.path.join(SCRIPT_DIR, "..", ".."))
C02C = os.path.join(ROOT, "bin", "c02c")
C02AS = os.path.join(ROOT, "bin", "c02-as")
EMU_DIR = os.path.join(ROOT, "test", "emu")

# Reuse the emulator harness: run_rom drives py65, and _target_value/_CMP/
# _parse_value give us the identical EXPECT vocabulary the emu suite uses.
sys.path.insert(0, EMU_DIR)
from run import run_rom, _target_value, _CMP, _parse_value, _fmt  # noqa: E402

MAX_CYCLES = 200000
_EXPECT_RE = re.compile(r"(.+?)\s+(==|!=|<=|>=|<|>)\s+(.+)")
_IR_COUNT_RE = re.compile(r"(.+?)\s+==\s+(\d+)$")

# ANSI colors, but only when writing to a terminal.
_TTY = sys.stdout.isatty()
def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _TTY else text


class Spec:
    def __init__(self, path):
        self.path = path
        self.desc = None
        self.mode = None
        self.files = None            # explicit link order, or None => sorted glob
        self.partial = False
        self.expects = []            # (raw, target, op, value)  [run]
        self.stderr = None           # substring                 [reject]
        self.ir_checks = []          # (kind, substring, count)  [link]
        self._parse()

    def _parse(self):
        for line in open(self.path):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            key, _, val = s.partition(":")
            key, val = key.strip(), val.strip()
            if key == "DESC":
                self.desc = val
            elif key == "MODE":
                self.mode = val
            elif key == "FILES":
                self.files = val.split()
            elif key == "PARTIAL":
                self.partial = val.lower() in ("yes", "true", "1")
            elif key == "EXPECT":
                m = _EXPECT_RE.match(val)
                if not m:
                    raise ValueError(f"{self.path}: malformed EXPECT: {val!r}")
                self.expects.append((val, m.group(1), m.group(2),
                                     _parse_value(m.group(3))))
            elif key == "STDERR":
                self.stderr = val
            elif key == "IR_COUNT":
                m = _IR_COUNT_RE.match(val)
                if not m:
                    raise ValueError(f"{self.path}: malformed IR_COUNT: {val!r}")
                self.ir_checks.append(("count", m.group(1).strip(), int(m.group(2))))
            elif key == "IR_PRESENT":
                self.ir_checks.append(("present", val, None))
            elif key == "IR_ABSENT":
                self.ir_checks.append(("absent", val, None))
            else:
                raise ValueError(f"{self.path}: unknown directive {key!r}")
        if self.mode not in ("run", "link", "reject"):
            raise ValueError(f"{self.path}: MODE must be run|link|reject")


def _sources(case_dir, spec):
    names = spec.files or sorted(f for f in os.listdir(case_dir)
                                 if f.endswith(".c02"))
    return [os.path.join(case_dir, n) for n in names]


def _run_c02c(args):
    return subprocess.run([C02C] + args, capture_output=True, text=True)


def _check_ir(ir_text, checks):
    """Evaluate IR_* directives against --dump-ir output. Returns list of fails."""
    fails = []
    lines = ir_text.splitlines()
    for kind, needle, count in checks:
        if kind == "count":
            got = sum(1 for ln in lines if needle in ln)
            if got != count:
                fails.append(f"IR_COUNT {needle!r}: expected {count}, got {got}")
        elif kind == "present":
            if needle not in ir_text:
                fails.append(f"IR_PRESENT {needle!r}: not found")
        elif kind == "absent":
            if needle in ir_text:
                fails.append(f"IR_ABSENT {needle!r}: unexpectedly present")
    return fails


def run_case(case_dir, tmp_dir, verbose):
    """Return (ok: bool, messages: list[str])."""
    spec = Spec(os.path.join(case_dir, "spec"))
    srcs = _sources(case_dir, spec)
    name = os.path.basename(case_dir)

    if spec.mode == "reject":
        out = os.path.join(tmp_dir, f"{name}.o")
        r = _run_c02c(["-c"] + srcs + ["-o", out])
        if r.returncode == 0:
            return False, ["expected link to fail, but it succeeded"]
        if spec.stderr and spec.stderr not in r.stderr:
            return False, [f"stderr missing {spec.stderr!r}",
                           f"  actual: {r.stderr.strip()!r}"]
        return True, [f"rejected: {spec.stderr}"]

    if spec.mode == "link":
        out = os.path.join(tmp_dir, f"{name}.o")
        if spec.partial:
            r = _run_c02c(["-c"] + srcs + ["-o", out])
            if r.returncode != 0:
                return False, ["link failed", f"  {r.stderr.strip()}"]
            ir = subprocess.run([C02AS, "--dump-ir", out],
                                capture_output=True, text=True).stdout
        else:
            r = _run_c02c(["--dump-ir"] + srcs)
            if r.returncode != 0:
                return False, ["link/codegen failed", f"  {r.stderr.strip()}"]
            ir = r.stdout
        fails = _check_ir(ir, spec.ir_checks)
        return (not fails), (fails or ["linked; IR checks passed"])

    # spec.mode == "run"
    rom_path = os.path.join(tmp_dir, f"{name}.bin")
    r = _run_c02c(srcs + ["-o", rom_path])
    if r.returncode != 0:
        return False, ["link/codegen failed", f"  {r.stderr.strip()}"]
    rom = open(rom_path, "rb").read()
    mpu, trace, halted = run_rom(rom, set(), MAX_CYCLES)
    if not halted:
        return False, [f"program did not reach halt loop in {MAX_CYCLES} cycles"]
    msgs, ok = [], True
    for raw, target, op, expected in spec.expects:
        actual = _target_value(target, rom, mpu, trace)
        exp = [ord(c) for c in expected] if isinstance(expected, str) else expected
        if _CMP[op](actual, exp):
            msgs.append(f"{raw}  [{_fmt(actual)}]")
        else:
            ok = False
            msgs.append(f"FAIL {raw}: got {_fmt(actual)}")
    return ok, msgs


def main():
    args = sys.argv[1:]
    verbose = False
    if args and args[0] in ("-v", "--verbose"):
        verbose = True
        args = args[1:]
    only = set(args)

    for tool in (C02C, C02AS):
        if not os.access(tool, os.X_OK):
            print(f"missing {tool} — run `make` first", file=sys.stderr)
            return 2

    import tempfile
    cases = sorted(d for d in os.listdir(TESTS_DIR)
                   if os.path.isdir(os.path.join(TESTS_DIR, d)))
    if only:
        cases = [c for c in cases if c in only]

    passed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name in cases:
            case_dir = os.path.join(TESTS_DIR, name)
            try:
                ok, msgs = run_case(case_dir, tmp, verbose)
            except Exception as e:  # a broken spec/case is a failure, not a crash
                ok, msgs = False, [f"harness error: {e}"]
            tag = _c("32", "PASS") if ok else _c("31", "FAIL")
            spec_desc = ""
            try:
                spec_desc = Spec(os.path.join(case_dir, "spec")).desc or ""
            except Exception:
                pass
            print(f"  {tag} {name}  {spec_desc}")
            if (verbose or not ok) and msgs:
                for m in msgs:
                    print(f"         {m}")
            passed += ok

    total = len(cases)
    print(f"\n{passed}/{total} passed")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
