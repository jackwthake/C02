#!/usr/bin/env python3
import subprocess
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(SCRIPT_DIR, "../../bin/cc02")
TESTS_DIR = SCRIPT_DIR
GOLDEN_DIR = os.path.join(SCRIPT_DIR, "golden")
SMOKE_BIN_DIR = os.path.join(SCRIPT_DIR, "bin")

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

def should_fail(filename):
    return "_bad_" in filename

def get_flags(filename):
    if should_fail(filename):
        return ["--syntax-check-only"]
    return ["--ast-dump"]

def run_test(path):
    filename = os.path.basename(path)
    flags = get_flags(filename)
    result = subprocess.run(
        [BIN] + flags + [path],
        capture_output=True, text=True
    )

    expects_failure = should_fail(filename)
    actually_failed = result.returncode != 0

    if expects_failure != actually_failed:
        print(f"  {FAIL} {filename}")
        print(f"         expected {'failure' if expects_failure else 'success'}, got {'failure' if actually_failed else 'success'}")
        if result.stderr:
            print(f"         stderr: {result.stderr.strip()}")
        return False

    if not expects_failure:
        golden_path = os.path.join(GOLDEN_DIR, filename + ".golden")
        if not os.path.exists(golden_path):
            print(f"  {PASS} {filename} (no golden — run with --update to create)")
        else:
            with open(golden_path) as f:
                expected = f.read()
            if result.stdout != expected:
                print(f"  {FAIL} {filename} (output differs from golden)")
                print(f"         rerun with --update if change is intentional")
                return False
            print(f"  {PASS} {filename}")
    else:
        print(f"  {PASS} {filename} (expected failure)")

    return True

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
    if should_fail(filename):
        print(f"  skip  {filename} (expected failure, no golden needed)")
        return
    result = subprocess.run(
        [BIN, "--ast-dump", path],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"  {FAIL} {filename} (binary failed during update)")
        if result.stderr:
            print(f"         stderr: {result.stderr.strip()}")
        return
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    golden_path = os.path.join(GOLDEN_DIR, filename + ".golden")
    with open(golden_path, "w") as f:
        f.write(result.stdout)
    print(f"  updated {filename}")

if __name__ == "__main__":
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

        print(f"\n{passed} passed, {failed} failed")
        sys.exit(1 if failed else 0)