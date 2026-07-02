#!/usr/bin/env python3
import subprocess
import os
import sys
from collections import namedtuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(SCRIPT_DIR, "../../bin/cc02")
TESTS_DIR = SCRIPT_DIR
GOLDEN_DIR = os.path.join(SCRIPT_DIR, "golden")
SMOKE_BIN_DIR = os.path.join(SCRIPT_DIR, "bin")

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

def should_fail(filename):
    return "_bad_" in filename


# A "_bad_" test is expected to fail at a specific compilation stage, and the
# compiler exits with a distinct code per stage (the *_RET_CODE defines in
# cc02/src/main.c). The stage is encoded in the filename prefix, so e.g.
# parser_bad_type.c02 must exit with PARSER_ERROR_RET_CODE. Failing at the
# wrong stage - or crashing, which lands here as a 139-ish/negative code - is
# a real regression even though the run still "failed".
STAGE_EXIT_CODES = {
    "lexer":    3,  # TOKEN_ERROR_RET_CODE
    "parser":   4,  # PARSER_ERROR_RET_CODE
    "analyzer": 5,  # ANALYZER_ERROR_RET_CODE
}

def expected_exit_code(filename):
    return STAGE_EXIT_CODES.get(filename.split("_", 1)[0])


def normalize(text, path):
    # Diagnostics embed the input file path; collapse the machine-specific
    # absolute path down to the basename so goldens are portable across
    # checkouts and CI. ANSI colour codes are left intact, matching the
    # convention already used by the stdout goldens.
    return text.replace(path, os.path.basename(path))


def get_flags(filename):
    if should_fail(filename):
        return ["--syntax-check-only"]
    if filename.startswith("analyzer"):
        return ["--symbol-dump"]
    if filename.startswith("parser"):
        return ["--ast-dump"]
    if filename.startswith("ir"):
        return ["--ir-dump"]
    return []

def compare_golden(filename, actual, stream_label, require_golden):
    """Compare `actual` against the stored golden for `filename`. Returns True
    on a match, False on a mismatch. A missing golden is tolerated (with a
    note) for good tests, but is a hard failure when `require_golden` is set:
    a _bad_ test with no committed golden silently checks nothing, which is
    exactly the no-op-in-CI trap this harness exists to avoid."""
    golden_path = os.path.join(GOLDEN_DIR, filename + ".golden")
    if not os.path.exists(golden_path):
        if require_golden:
            print(f"  {FAIL} {filename} (no golden — its {stream_label} is unchecked; run --update and commit it)")
            return False
        print(f"  {PASS} {filename} (no golden — run with --update to create)")
        return True
    with open(golden_path) as f:
        expected = f.read()
    if actual != expected:
        print(f"  {FAIL} {filename} ({stream_label} differs from golden)")
        print(f"         rerun with --update if change is intentional")
        return False
    print(f"  {PASS} {filename}")
    return True


def run_test(path):
    filename = os.path.basename(path)
    flags = get_flags(filename)
    result = subprocess.run([BIN] + flags + [path], capture_output=True, text=True)

    if should_fail(filename):
        # Bad test: must fail at the expected stage (exact exit code), and the
        # diagnostic it prints to stderr must match the golden.
        expected = expected_exit_code(filename)
        if expected is None:
            ok, detail = result.returncode != 0, "nonzero exit"
        else:
            ok, detail = result.returncode == expected, f"exit {expected}"
        if not ok:
            print(f"  {FAIL} {filename}")
            print(f"         expected {detail}, got exit {result.returncode}")
            if result.stderr:
                print(f"         stderr: {result.stderr.strip()}")
            return False
        return compare_golden(filename, normalize(result.stderr, path), "stderr", require_golden=True)

    # Good test: must succeed (exit 0), and the dump it prints to stdout must
    # match the golden. A successful compile can still print warnings to
    # stderr (e.g. an invalid `interrupt` signature) — those are checked
    # against a separate `<name>.warn.golden` so they don't collide with the
    # stdout dump golden. Anything on stderr with a nonzero exit would have
    # already been caught above, so stderr here is warning-only by construction.
    if result.returncode != 0:
        print(f"  {FAIL} {filename}")
        print(f"         expected success (exit 0), got exit {result.returncode}")
        if result.stderr:
            print(f"         stderr: {result.stderr.strip()}")
        return False
    stdout_ok = compare_golden(filename, result.stdout, "stdout", require_golden=False)
    stderr_ok = True
    if result.stderr.strip():
        stderr_ok = compare_golden(filename + ".warn", normalize(result.stderr, path), "stderr", require_golden=False)
    return stdout_ok and stderr_ok

def run_valgrind(path):
    filename = os.path.basename(path)
    flags = ["--error-exitcode=69", "--leak-check=full", "--errors-for-leak-kinds=all"]
    c02_flags = get_flags(filename)
    result = subprocess.run(
        ["valgrind"] + flags + [BIN] + c02_flags + [path],
        capture_output=True, text=True
    )

    if result.returncode == 69: # Failed
        print(f"    `- valgrind check: {FAIL} {filename}")
        print(result.stderr)
        return False
    
    print(f"    `- valgrind check: {PASS} {filename}\n")
    return True

def run_smoke_binary(path):
    filename = os.path.basename(path)
    
    # 1. Execute the binary
    run_res = subprocess.run([path], capture_output=True, text=True)
    exec_passed = True
    valg_passed = True

    if run_res.returncode != 0:
        print(f"  {FAIL} {filename} (execution returned {run_res.returncode})")
        if run_res.stderr:
            print(f"         stderr: {run_res.stderr.strip()}")
        exec_passed = False
    else:
        print(f"  {PASS} {filename} (execution)")

    # 2. Run valgrind on the binary
    valg_flags = ["--error-exitcode=69", "--leak-check=full", "--errors-for-leak-kinds=all"]
    valg_res = subprocess.run(
        ["valgrind"] + valg_flags + [path],
        capture_output=True, text=True
    )

    if valg_res.returncode == 69:
        print(f"    `- valgrind check: {FAIL} {filename}")
        print(valg_res.stderr)
        valg_passed = False
    else:
        print(f"    `- valgrind check: {PASS} {filename}\n")

    return exec_passed and valg_passed

def update_golden(path):
    filename = os.path.basename(path)
    flags = get_flags(filename)
    result = subprocess.run([BIN] + flags + [path], capture_output=True, text=True)

    if should_fail(filename):
        # Bad test: golden the diagnostic (stderr) emitted at the failing stage.
        expected = expected_exit_code(filename)
        if expected is not None and result.returncode != expected:
            print(f"  {FAIL} {filename} (expected exit {expected}, got {result.returncode} during update)")
            return
        content = normalize(result.stderr, path)
        warn_content = None
    else:
        # Good test: golden the dump (stdout) from a successful compile, plus
        # any warning printed to stderr (separate golden — see run_test).
        if result.returncode != 0:
            print(f"  {FAIL} {filename} (binary failed during update)")
            if result.stderr:
                print(f"         stderr: {result.stderr.strip()}")
            return
        content = result.stdout
        warn_content = normalize(result.stderr, path) if result.stderr.strip() else None

    os.makedirs(GOLDEN_DIR, exist_ok=True)
    golden_path = os.path.join(GOLDEN_DIR, filename + ".golden")
    with open(golden_path, "w") as f:
        f.write(content)
    if warn_content is not None:
        warn_path = os.path.join(GOLDEN_DIR, filename + ".warn.golden")
        with open(warn_path, "w") as f:
            f.write(warn_content)
    print(f"  updated {filename}")

REPO_ROOT = os.path.join(SCRIPT_DIR, "../..")

COMPILER_EXTS = {".c", ".h", ".inc", ".mk"}
HARNESS_EXTS = {".py", ".c02"}
OBJDUMP_EXTS = {".rs", ".mk"}

IGNORED_DIRS = {".git", "bin", "build", "node_modules", "__pycache__", "target"}

COMPILER_SRC_DIR = os.path.join("cc02", "src")
COMPILER_MAKEFILES = ["Makefile", os.path.join("cc02", "Makefile")]
OBJDUMP_DIR = "c02-objdump"
TESTS_REL = os.path.join("cc02", "tests")
EXAMPLES_DIR = "examples"

# label, total lines, list of (child_label, lines) for tree expansion
Section = namedtuple("Section", ["label", "total", "children"])

def count_lines_in(root_dir, extensions):
    counts = {}
    abs_root = os.path.join(REPO_ROOT, root_dir)
    for dirpath, dirnames, filenames in os.walk(abs_root):
        dirnames[:] = [d for d in dirnames if d not in IGNORED_DIRS]
        for f in filenames:
            ext = os.path.splitext(f)[1]
            if f == "Makefile" and not ext:
                ext = ".mk"
            if ext not in extensions:
                continue
            filepath = os.path.join(dirpath, f)
            try:
                with open(filepath) as fh:
                    lines = sum(1 for _ in fh)
            except (OSError, UnicodeDecodeError):
                continue
            counts[ext] = counts.get(ext, 0) + lines
    return counts

def count_lines_by_folder(root_dir, extensions, default_folder="driver"):
    """Count lines grouped by immediate subdirectory; files at the root go into default_folder."""
    folder_counts = {}
    abs_root = os.path.join(REPO_ROOT, root_dir)
    for dirpath, dirnames, filenames in os.walk(abs_root):
        dirnames[:] = [d for d in dirnames if d not in IGNORED_DIRS]
        for f in filenames:
            ext = os.path.splitext(f)[1]
            if f == "Makefile" and not ext:
                ext = ".mk"
            if ext not in extensions:
                continue
            filepath = os.path.join(dirpath, f)
            try:
                with open(filepath) as fh:
                    lines = sum(1 for _ in fh)
            except (OSError, UnicodeDecodeError):
                continue
            relpath = os.path.relpath(filepath, abs_root)
            parts = relpath.split(os.sep)
            folder = parts[0] if len(parts) > 1 else default_folder
            folder_counts[folder] = folder_counts.get(folder, 0) + lines
    return folder_counts

def count_file(rel_path):
    try:
        with open(os.path.join(REPO_ROOT, rel_path)) as fh:
            return sum(1 for _ in fh)
    except (OSError, UnicodeDecodeError):
        return 0

def print_loc_table(sections):
    grand_total = sum(s.total for s in sections)
    if grand_total == 0:
        print("no recognized source files found")
        return

    # measure column widths from actual data
    all_labels = [s.label for s in sections] + ["Grand Total"]
    for s in sections:
        for i, (child_label, _) in enumerate(s.children):
            prefix = "  └─ " if i == len(s.children) - 1 else "  ├─ "
            all_labels.append(prefix + child_label)
    label_w = max(len(l) for l in all_labels)
    lines_w = max(len(f"{grand_total:,}"), len("Lines"))

    sep = "─" * (label_w + lines_w + 11)
    print(f"\n{'Module':<{label_w}}  {'Lines':>{lines_w}}   % Total")
    print(sep)

    for s in sections:
        pct = s.total / grand_total * 100
        print(f"{s.label:<{label_w}}  {s.total:>{lines_w},}   {pct:5.1f}%")
        for i, (child_label, child_lines) in enumerate(s.children):
            prefix = "  └─ " if i == len(s.children) - 1 else "  ├─ "
            child_pct = child_lines / grand_total * 100
            full_label = prefix + child_label
            print(f"{full_label:<{label_w}}  {child_lines:>{lines_w},}   {child_pct:5.1f}%")

    print(sep)
    print(f"{'Grand Total':<{label_w}}  {grand_total:>{lines_w},}   100.0%")

def count_lines():
    # --- Compiler ---
    compiler_ext_counts = count_lines_in(COMPILER_SRC_DIR, COMPILER_EXTS)
    for mkfile in COMPILER_MAKEFILES:
        lines = count_file(mkfile)
        if lines:
            compiler_ext_counts[".mk"] = compiler_ext_counts.get(".mk", 0) + lines
    compiler_total = sum(compiler_ext_counts.values())

    folder_counts = count_lines_by_folder(COMPILER_SRC_DIR, COMPILER_EXTS)
    compiler_children = sorted(folder_counts.items(), key=lambda x: x[1], reverse=True)

    # --- Test Harness ---
    harness_counts = count_lines_in(TESTS_REL, {*COMPILER_EXTS, *HARNESS_EXTS})
    for ext, lines in count_lines_in(EXAMPLES_DIR, HARNESS_EXTS).items():
        harness_counts[ext] = harness_counts.get(ext, 0) + lines
    harness_total = sum(harness_counts.values())

    # --- Disassembler ---
    objdump_total = sum(count_lines_in(OBJDUMP_DIR, OBJDUMP_EXTS).values())

    # To add a new toolchain component, append a Section here and add its
    # directory/extension constants above.
    sections = [
        Section("Compiler",     compiler_total, compiler_children),
        Section("Test Harness", harness_total,  []),
        Section("Disassembler", objdump_total,  []),
    ]

    if compiler_total == 0 and harness_total == 0:
        print("no recognized source files found")
        return

    print_loc_table(sections)

if __name__ == "__main__":
    if "--cloc" in sys.argv:
        print("--- Lines of Code ---")
        count_lines()
        sys.exit(0)

    updating = "--update" in sys.argv
    tests = sorted(f for f in os.listdir(TESTS_DIR) if f.endswith(".c02"))

    if not tests:
        print(f"no .c02 files found in {TESTS_DIR}")
        sys.exit(1)

    passed = failed = 0
    
    print("--- Compiler Tests ---")
    for t in tests:
        path = os.path.join(TESTS_DIR, t)
        if updating:
            update_golden(path)
        else:
            if run_test(path):
                passed += 1
            else:
                failed += 1
            
            if run_valgrind(path):
                passed += 1
            else:
                failed += 1

    if not updating:
        print("\n--- Smoke Binary Tests ---")
        if os.path.exists(SMOKE_BIN_DIR):
            # Find all executable files in the bin directory
            smoke_bins = sorted(
                f for f in os.listdir(SMOKE_BIN_DIR) 
                if os.path.isfile(os.path.join(SMOKE_BIN_DIR, f)) and os.access(os.path.join(SMOKE_BIN_DIR, f), os.X_OK)
            )
            
            if not smoke_bins:
                print(f"  no executables found in {SMOKE_BIN_DIR}")
            
            for b in smoke_bins:
                bin_path = os.path.join(SMOKE_BIN_DIR, b)
                if run_smoke_binary(bin_path):
                    passed += 1
                else:
                    failed += 1
        else:
            print(f"  directory {SMOKE_BIN_DIR} not found, skipping smoke tests.")

        # Emulator tests (requires py65)
        try:
            from emu_test import TESTS as EMU_TESTS, main as emu_main
            print("\n--- Emulator Tests ---")
            for name, fn in EMU_TESTS:
                ok, detail = fn()
                if ok:
                    print(f"  {PASS} {name}")
                    passed += 1
                else:
                    print(f"  {FAIL} {name}")
                    print(f"         {detail}")
                    failed += 1
        except ImportError:
            print("\n--- Emulator Tests ---")
            print("  skipped (py65 not installed)")

        print(f"\n{passed} passed, {failed} failed")
        sys.exit(1 if failed else 0)