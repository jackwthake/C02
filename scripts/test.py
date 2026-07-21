#!/usr/bin/env python3
#
# test.py - golden-file test runner for the c02 compiler.
#
# Layout:
#   test/<stage>/*.c02           inputs, one program per file
#   test/<stage>/golden/*.golden expected stdout, one <name>.golden per input
#
# Most stages compile a single source file directly; "codegen" runs the full
# c02c pipeline (frontend -> linker -> c02-as) instead, since codegen has no
# standalone entry point of its own.
#
# Each case is compiled through the c02c driver and matched against a committed
# golden. Cases come in three flavours, distinguished by filename:
#
#   * positive (any name)   - must parse: exit 0, AST printed to stdout.
#                            The golden holds that stdout. A nonzero exit is a
#                            *crash* (an uncaught exception), reported as its own
#                            category - never a plain mismatch, never blessable.
#   * negative (bad_*.c02)  - must fail to parse: nonzero exit, diagnostic on
#                            stderr. The golden holds that stderr. If a bad_ case
#                            parses cleanly (exit 0) that is an XPASS failure.
#   * warning (warn_*.c02)  - must still succeed: exit 0 (like positive), but
#                            expected to print a non-fatal diagnostic (a
#                            WARN_*) to stderr. The golden holds that stderr,
#                            not stdout - this is the only way to pin a
#                            warning's text down, since a plain positive case
#                            never looks at stderr at all.
#
# Usage:
#   scripts/test.py                 run every stage
#   scripts/test.py parser          run one stage
#   scripts/test.py --bless         regenerate goldens from current output
#   scripts/test.py --no-build      skip the `make` step (use the current bin/)
#   scripts/test.py -v              show the diff for every failure
import argparse
import difflib
import os
import re
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
TEST_ROOT = os.path.join(REPO_ROOT, "test")
DRIVER = os.path.join(REPO_ROOT, "bin", "c02c")
FRONTEND = os.path.join(REPO_ROOT, "bin", "c02-frontend")

# How each stage turns an input path into a command line. Frontend-only stages
# invoke c02-frontend directly (bypassing c02c's codegen) and isolate a single
# stage via a flag, so their goldens stay stage-specific:
#   * parser   - `--parse-only` stops after parsing (its fixtures aren't all
#                semantically valid whole programs, so analysis must not run);
#                `--dump-ast` prints the parsed AST for the golden to diff.
#   * analyzer - full parse + semantic analysis; --dump-ast makes a clean run
#                print the AST to stdout (instead of emitting a binary .o) so the
#                golden stays text-diffable, while negatives (bad_*) still run
#                analysis and print diagnostics to stderr.
#   * codegen  - the *full* pipeline (frontend -> linker -> c02-as) via the
#                `c02c` driver, since codegen never runs standalone on source.
#                `--no-stdlib` keeps goldens independent of libc02's own
#                global/RAM footprint. There's no AST/IR dump to diff on
#                success (codegen's only output is a binary ROM, written to a
#                throwaway path), so positive goldens here just pin "nothing
#                printed"; negatives still pin the diagnostic on stderr.
_CODEGEN_OUT = os.path.join(tempfile.gettempdir(), "c02_codegen_test.rom")
STAGES = {
    "parser":   lambda src: [FRONTEND, "--parse-only", "--dump-ast", src],
    "analyzer": lambda src: [FRONTEND, "--dump-ast", src],
    "codegen":  lambda src: [DRIVER, "--no-stdlib", "-o", _CODEGEN_OUT, src],
}

# --- terminal styling -------------------------------------------------------
_USE_COLOR = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None
_ANSI_RE = re.compile(r"\033\[[0-9;]*m")


def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _USE_COLOR else text


def green(t):  return _c("32", t)
def red(t):    return _c("31", t)
def yellow(t): return _c("33", t)
def dim(t):    return _c("2", t)
def bold(t):   return _c("1", t)


# --- results ----------------------------------------------------------------
# XPASS: a bad_ case that was expected to fail but parsed cleanly.
PASS, MISMATCH, CRASH, XPASS, NO_GOLDEN, BLESSED = \
    "PASS", "MISMATCH", "CRASH", "XPASS", "NO_GOLDEN", "BLESSED"
FAILURES = (MISMATCH, CRASH, XPASS, NO_GOLDEN)


class Result:
    def __init__(self, name, status, detail=""):
        self.name = name
        self.status = status
        self.detail = detail


def run_case(src_path, argv_for):
    """Compile one input, returning (stdout, stderr, returncode)."""
    # Pass a repo-relative source path: the parser echoes it back in its error
    # diagnostics, so an absolute path would bake this machine's checkout
    # location into every negative golden and break on any other machine (CI).
    rel_src = os.path.relpath(src_path, REPO_ROOT)
    proc = subprocess.run(
        argv_for(rel_src),
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    # c02-as colors its failure messages unconditionally (no TTY check), so a
    # captured pipe still carries the escape codes. Strip them here, once, so
    # every stage's goldens stay plain, diffable text - never baked into a
    # committed golden file.
    stdout = _ANSI_RE.sub("", proc.stdout)
    stderr = _ANSI_RE.sub("", proc.stderr)
    return stdout, stderr, proc.returncode


def check_case(name, src_path, golden_path, argv_for, bless):
    stdout, stderr, code = run_case(src_path, argv_for)
    basename = os.path.basename(name)
    negative = basename.startswith("bad_")
    warning = basename.startswith("warn_")

    if negative:
        # A negative must fail to parse. Parsing cleanly is an XPASS - the case
        # no longer tests what it claims, and there is nothing to bless.
        if code == 0:
            return Result(name, XPASS, "expected a parse error, but the input parsed cleanly")
        observed = stderr
    elif warning:
        # A warning case must still succeed - same crash handling as positive -
        # but the thing being pinned down is the diagnostic text on stderr.
        if code != 0:
            why = f"exit {code}" + (f"\n{stderr.rstrip()}" if stderr else "")
            return Result(name, CRASH, why)
        observed = stderr
    else:
        # A positive must parse. A nonzero exit is an uncaught exception: its own
        # category so it can't masquerade as a mismatch, and never blessable.
        if code != 0:
            why = f"exit {code}" + (f"\n{stderr.rstrip()}" if stderr else "")
            return Result(name, CRASH, why)
        observed = stdout

    if bless:
        with open(golden_path, "w") as f:
            f.write(observed)
        return Result(name, BLESSED)

    if not os.path.exists(golden_path):
        return Result(name, NO_GOLDEN, "no golden - run with --bless to create it")

    with open(golden_path) as f:
        expected = f.read()

    if observed == expected:
        return Result(name, PASS)

    diff = "".join(difflib.unified_diff(
        expected.splitlines(keepends=True),
        observed.splitlines(keepends=True),
        fromfile=f"{name}.golden", tofile=f"{name} (actual)",
    ))
    return Result(name, MISMATCH, diff)


def run_stage(stage, argv_for, bless, verbose):
    stage_dir = os.path.join(TEST_ROOT, stage)
    golden_dir = os.path.join(stage_dir, "golden")
    if not os.path.isdir(stage_dir):
        print(yellow(f"  (no test/{stage}/ directory - skipping)"))
        return []
    if bless:
        os.makedirs(golden_dir, exist_ok=True)

    inputs = sorted(f for f in os.listdir(stage_dir) if f.endswith(".c02"))
    if not inputs:
        print(yellow(f"  (no .c02 cases in test/{stage}/)"))
        return []

    results = []
    for fname in inputs:
        name = fname[:-4]  # strip .c02
        src = os.path.join(stage_dir, fname)
        golden = os.path.join(golden_dir, name + ".golden")
        r = check_case(name, src, golden, argv_for, bless)
        results.append(r)
        _print_line(r, verbose)
    return results


def _print_line(r, verbose):
    marks = {
        PASS:      green("  ok  "),
        BLESSED:   yellow(" bless"),
        MISMATCH:  red("  ✗   "),
        CRASH:     red(" CRASH"),
        XPASS:     red(" XPASS"),
        NO_GOLDEN: yellow("  ?   "),
    }
    tag = "" if r.status in (PASS, BLESSED) else red(f"  {r.status}")
    print(f"  {marks[r.status]}  {r.name}{tag}")
    # Always explain a crash / xpass / missing-golden; show diffs only with -v.
    if r.detail and (verbose or r.status in (CRASH, XPASS, NO_GOLDEN)):
        print("".join("      " + line + "\n" for line in r.detail.splitlines()))


def main():
    ap = argparse.ArgumentParser(description="c02 golden-file test runner")
    ap.add_argument("stages", nargs="*", help="stages to run (default: all)")
    ap.add_argument("--bless", action="store_true",
                    help="regenerate goldens from current output")
    ap.add_argument("--no-build", action="store_true",
                    help="skip `make` and use the current bin/")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="show a diff for every mismatch")
    args = ap.parse_args()

    if not args.no_build:
        print(bold("==> building"))
        build = subprocess.run(["make"], cwd=REPO_ROOT,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if build.returncode != 0:
            print(red("build failed:"))
            print(build.stderr)
            return 2

    if not os.access(DRIVER, os.X_OK):
        print(red(f"driver not found or not executable: {DRIVER}"))
        print("  run `make` first, or drop --no-build")
        return 2

    # Toolchain sanity check: the driver must actually compile a trivial valid
    # program. Without this, a driver-level failure (e.g. a missing frontend
    # binary) exits nonzero and would be mistaken for an expected parse error -
    # and --bless would enshrine that infra error as a negative golden.
    with tempfile.NamedTemporaryFile("w", suffix=".c02", dir=TEST_ROOT, delete=False) as tf:
        tf.write("fn main() -> void { return; }\n")
        smoke_path = tf.name
    try:
        out, err, code = run_case(smoke_path, STAGES[list(STAGES)[0]])
    finally:
        os.unlink(smoke_path)
    if code != 0 or err:
        print(red("toolchain sanity check failed - the driver cannot compile a "
                  "trivial valid program:"))
        print((err or out).rstrip())
        print("  the toolchain is broken; refusing to run (goldens would be garbage)")
        return 2

    stages = args.stages or list(STAGES)
    unknown = [s for s in stages if s not in STAGES]
    if unknown:
        print(red(f"unknown stage(s): {', '.join(unknown)}"))
        print(f"  known stages: {', '.join(STAGES)}")
        return 2

    all_results = []
    for stage in stages:
        print(bold(f"\n==> {stage}"))
        all_results += run_stage(stage, STAGES[stage], args.bless, args.verbose)

    # --- summary ------------------------------------------------------------
    total = len(all_results)
    passed = sum(r.status == PASS for r in all_results)
    blessed = sum(r.status == BLESSED for r in all_results)
    failed = [r for r in all_results if r.status in FAILURES]

    print(bold("\n" + "─" * 40))
    if args.bless:
        print(f"blessed {blessed} golden(s); {len(failed)} case(s) could not be blessed")
    elif not failed:
        print(green(bold(f"✓ all {passed}/{total} passed")))
    else:
        summary = ", ".join(f"{sum(r.status == s for r in failed)} {s.lower()}"
                            for s in FAILURES
                            if any(r.status == s for r in failed))
        print(red(bold(f"✗ {passed}/{total} passed") + f"  ({summary})"))

    return 1 if failed and not args.bless else 0


if __name__ == "__main__":
    sys.exit(main())
