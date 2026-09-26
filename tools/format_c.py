#!/usr/bin/env python3
"""Apply or check the pinned C format, leaving captured fixtures untouched."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang-format', default='clang-format')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    requirement = (root / 'tools/requirements-format.txt').read_text().strip()
    expected = requirement.removeprefix('clang-format==')
    version = subprocess.check_output([args.clang_format, '--version'], text=True)
    match = re.search(r'\bversion (\d+\.\d+\.\d+)\b', version)
    if not match or match.group(1) != expected:
        print(f'Required {requirement}; found {version.strip()}', file=sys.stderr)
        return 1

    files = sorted(
        path
        for directory in ('src', 'cli', 'tests')
        for path in (root / directory).rglob('*')
        if path.suffix in ('.c', '.h') and path.is_file()
        and not path.is_relative_to(root / 'tests/fixtures')
    )
    changes = {}
    for path in files:
        original = path.read_bytes()
        formatted = subprocess.check_output(
            [args.clang_format, '--style=file', '--Werror', '--fail-on-incomplete-format',
             f'--assume-filename={path}'],
            input=original,
        )
        if formatted != original:
            changes[path] = formatted

    if args.check:
        for path in changes:
            print(f'Needs formatting: {path.relative_to(root)}', file=sys.stderr)
        if changes:
            print('Run make format with the pinned formatter.', file=sys.stderr)
            return 1
        print(f'C format check passed ({len(files)} files).')
        return 0

    if changes:
        scratch = root / 'tmp'
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='format-c-', dir=scratch) as staging:
            for index, (path, formatted) in enumerate(changes.items()):
                replacement = Path(staging) / str(index)
                replacement.write_bytes(formatted)
                shutil.copymode(path, replacement)
                replacement.replace(path)
    print(f'Formatted {len(changes)} of {len(files)} C files.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError) as error:
        print(f'C formatting failed: {error}', file=sys.stderr)
        sys.exit(1)
