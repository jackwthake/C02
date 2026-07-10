#!/usr/bin/env python3
import os
from collections import namedtuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.join(SCRIPT_DIR, "../")

# label, raw line total, code (non-blank/non-comment) line total
Section = namedtuple("Section", ["label", "total", "code"])

# A language's comment syntax: the line-comment token (or None), and an
# optional (open, close) block-comment delimiter pair. A plain per-line
# true/false predicate can't skip block comments correctly on its own -- it
# has no memory of "an earlier line opened a comment that hasn't closed yet"
# -- so instead each language gets this small spec, and the one shared state
# machine below (count_effective_lines) tracks that across a file.
CommentStyle = namedtuple("CommentStyle", ["line", "block"])

C_STYLE    = CommentStyle("//", ("/*", "*/"))   # C, Rust
HASKELL    = CommentStyle("--", ("{-", "-}"))
HASH_STYLE = CommentStyle("#", None)            # Python, Makefiles
NO_STYLE   = CommentStyle(None, None)           # unknown: blank-line skip only


def count_effective_lines(lines, style):
    """Lines that aren't blank and aren't wholly comment, per `style`. Block
    comments are tracked across lines; nesting and comment-like tokens inside
    string literals aren't handled -- a LOC estimate, not a real lexer."""
    count = 0
    in_block = False
    for raw in lines:
        line = raw.strip()

        if in_block:
            if not (style.block and style.block[1] in line):
                continue
            line = line.split(style.block[1], 1)[1].strip()
            in_block = False

        if style.block and style.block[0] in line:
            before, _, after = line.partition(style.block[0])
            if style.block[1] in after:
                line = (before + after.split(style.block[1], 1)[1]).strip()
            else:
                line, in_block = before.strip(), True

        if style.line and line.startswith(style.line):
            continue
        if line:
            count += 1
    return count


def count_lines_in(root_dir, ext_styles):
    raw_counts, code_counts = {}, {}
    abs_root = os.path.join(REPO_ROOT, root_dir)
    for dirpath, dirnames, filenames in os.walk(abs_root):
        for f in filenames:
            ext = os.path.splitext(f)[1]
            if f == "Makefile" and not ext:
                ext = ".mk"
            if ext not in ext_styles:
                continue
            filepath = os.path.join(dirpath, f)
            try:
                with open(filepath) as fh:
                    lines = fh.readlines()
            except (OSError, UnicodeDecodeError):
                continue
            raw_counts[ext] = raw_counts.get(ext, 0) + len(lines)
            code_counts[ext] = code_counts.get(ext, 0) + count_effective_lines(lines, ext_styles[ext])
    return raw_counts, code_counts


def count_file(rel_path, style=NO_STYLE):
    try:
        with open(os.path.join(REPO_ROOT, rel_path)) as fh:
            lines = fh.readlines()
    except (OSError, UnicodeDecodeError):
        return 0, 0
    return len(lines), count_effective_lines(lines, style)


def print_loc_table(sections):
    grand_total = sum(s.total for s in sections)
    grand_code = sum(s.code for s in sections)
    if grand_total == 0:
        print("no recognized source files found")
        return

    # measure column widths from actual data
    all_labels = [s.label for s in sections] + ["Grand Total"]
    label_w = max(len(l) for l in all_labels)
    lines_w = max(len(f"{grand_total:,}"), len("Lines"))
    code_w = max(len(f"{grand_code:,}"), len("Code"))

    sep = "─" * (label_w + lines_w + code_w + 13)
    print(f"\n{'Module':<{label_w}}  {'Lines':>{lines_w}}  {'Code':>{code_w}}   % Total")
    print(sep)

    for s in sections:
        pct = s.total / grand_total * 100
        if s.label == "---":
          print(sep)
        else:
          print(f"{s.label:<{label_w}}  {s.total:>{lines_w},}  {s.code:>{code_w},}   {pct:5.1f}%")

    print(sep)
    print(f"{'Grand Total':<{label_w}}  {grand_total:>{lines_w},}  {grand_code:>{code_w},}   100.0%")


def count_dir(ext_styles, makefiles, src_dir):
    raw_counts, code_counts = count_lines_in(src_dir, ext_styles)
    for mkfile in makefiles:
        raw, code = count_file(mkfile, HASH_STYLE)
        if raw:
            raw_counts[".mk"] = raw_counts.get(".mk", 0) + raw
            code_counts[".mk"] = code_counts.get(".mk", 0) + code
    return sum(raw_counts.values()), sum(code_counts.values())


def count_lines():
    compiler_total, compiler_code = count_dir(
        {".c": C_STYLE, ".h": C_STYLE},
        ["Makefile", os.path.join("c02-as", "Makefile")],
        os.path.join("c02-as", "src"))

    frontend_total, frontend_code = count_dir(
        {".hs": HASKELL},
        ["Makefile", os.path.join("c02-frontend", "Makefile")],
        os.path.join("c02-frontend", "src"))

    objdump_total, objdump_code = count_dir(
        {".rs": C_STYLE},
        ["Makefile", os.path.join("c02-objdump", "Makefile")],
        os.path.join("c02-objdump", "src"))
    
    parser_tests_total, parser_test_code = count_dir(
        {".c02": C_STYLE},
        [],
        os.path.join("test", "parser"))

    tooling_total, tooling_code = count_dir(
        {".py": HASH_STYLE},
        [], os.path.join("scripts", ""))

    # To add a new toolchain component, append a Section here and add its
    # directory/extension-style constants above.
    sections = [
        Section("c02-frontend", frontend_total, frontend_code),
        Section("c02-as", compiler_total, compiler_code),
        Section("c02-objdump", objdump_total, objdump_code),
        Section("---", 0, 0),
        Section("Parser Tests", parser_tests_total, parser_test_code),
        Section("Internal Tooling", tooling_total, tooling_code),
    ]

    print_loc_table(sections)


if __name__ == "__main__":
    count_lines()
