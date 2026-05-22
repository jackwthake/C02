import argparse
import sys
from pathlib import Path

def count_lines_in_file(filepath):
    """Counts lines in a file. Returns 0 if it's a binary file."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='strict') as f:
            return sum(1 for _ in f)
    except (UnicodeDecodeError, PermissionError, IsADirectoryError):
        # If it fails to decode as UTF-8, it's likely a binary file.
        return 0

def main():
    parser = argparse.ArgumentParser(description="Count total lines in all non-binary files.")
    parser.add_argument("dir", help="Path to the directory to search")
    args = parser.parse_args()

    target_dir = Path(args.dir)
    if not target_dir.is_dir():
        print(f"Error: {args.dir} is not a valid directory.")
        sys.exit(1)

    total_lines = 0
    # Use rglob('*') if you want to include subdirectories. 
    # Use iterdir() if you only want the top-level directory.
    for file_path in target_dir.rglob('*'):
        if file_path.is_file():
            total_lines += count_lines_in_file(file_path)

    print(f"Total lines across all text files: {total_lines}")

if __name__ == "__main__":
    main()