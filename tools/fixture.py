#!/usr/bin/env python3
"""Create a deterministic scan corpus in a NEW directory; never overwrite a tree."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import random
import socket
import subprocess
import sys


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('root', type=Path)
    p.add_argument('--shape', choices=('mixed', 'flat', 'wide', 'deep', 'empty'), default='mixed')
    p.add_argument('--files', type=int, default=10000)
    p.add_argument('--directories', type=int, default=1000)
    p.add_argument('--seed', type=int, default=42)
    p.add_argument('--creation-order', choices=('natural', 'reverse', 'random'), default='natural')
    p.add_argument('--max-file-bytes', type=int, default=4096)
    args = p.parse_args()
    if not 0 <= args.files <= 10000000 or not 0 <= args.directories <= 1000000 or not 0 <= args.max_file_bytes <= 1048576:
        p.error('counts or file-size limits exceed fixture safety bounds')
    if args.root.exists() or args.root.is_symlink():
        p.error('root must not exist; the generator never overwrites an existing tree')
    if args.shape == 'deep' and args.directories > 150:
        p.error('deep fixtures are capped at 150 levels for portable setup and cleanup')
    rng = random.Random(args.seed)
    args.root.mkdir(parents=False)
    dirs = [args.root]
    directory_count = 0 if args.shape == 'flat' else args.directories
    order = list(range(directory_count))
    if args.creation_order == 'reverse':
        order.reverse()
    elif args.creation_order == 'random':
        rng.shuffle(order)
    if args.shape == 'deep':
        parent = args.root
        for _ in order:
            parent = parent / 'd'; parent.mkdir(); dirs.append(parent)
    else:
        for index in order:
            child = args.root / f'd{index:07d}'; child.mkdir(); dirs.append(child)
        # Assign names independently of creation order, keeping the logical tree stable.
        dirs = [args.root] + sorted(dirs[1:])
    file_order = list(range(0 if args.shape == 'empty' else args.files))
    if args.creation_order == 'reverse':
        file_order.reverse()
    elif args.creation_order == 'random':
        rng.shuffle(file_order)
    bytes_written = 0
    for index in file_order:
        parent = dirs[-1] if args.shape == 'deep' else dirs[index % len(dirs)]
        size = (index * 7919 + args.seed) % (args.max_file_bytes + 1)
        (parent / f'f{index:09d}').write_bytes(bytes([index & 255]) * size)
        bytes_written += size
    compressed = None
    if args.shape == 'mixed':
        (args.root / '.hidden').write_bytes(b'hidden')
        (args.root / 'Empty.app' / 'Contents').mkdir(parents=True)
        source = args.root / 'hardlink-source'; source.write_bytes(b'one object, two names')
        os.link(source, args.root / 'hardlink-alias')
        os.link(source, args.root / 'Empty.app' / 'Contents' / 'cross-directory-hardlink')
        os.symlink('.', args.root / 'symlink-loop-not-followed')
        os.symlink('hardlink-source', args.root / 'symlink-file')
        os.symlink('missing-target', args.root / 'symlink-dangling')
        os.mkfifo(args.root / 'fifo')
        # A relative bind avoids AF_UNIX's much shorter pathname limit.
        previous = os.open('.', os.O_RDONLY)
        try:
            os.chdir(args.root)
            with socket.socket(socket.AF_UNIX) as sock:
                sock.bind('socket')
        finally:
            os.fchdir(previous)
            os.close(previous)
        if sys.platform == 'darwin':
            forked = args.root / 'resource-fork'
            forked.write_bytes(b'data fork')
            with open(str(forked) + '/..namedfork/rsrc', 'wb') as f:
                f.write(b'R' * 16384)
            subprocess.run(['xattr', '-w', 'dev.vantage.fixture', 'extended metadata' * 128, str(forked)], check=True)
            source = args.root / '.compression-source'
            source.write_bytes(b'compressed fixture\n' * 16384)
            destination = args.root / 'compressed'
            subprocess.run(['ditto', '--noclone', '--hfsCompression', str(source), str(destination)], check=True)
            source.unlink()
            compressed = bool(destination.stat().st_flags & 0x20)
        with (args.root / 'sparse-256MiB').open('wb') as f:
            f.seek(256 * 1024 * 1024); f.write(b'x')
        for name in ('space name', 'quote"name', 'line\nbreak', '雪'):
            (args.root / name).write_bytes(b'name fixture')
    # Metadata goes to stdout, outside the corpus; do not silently add a manifest file.
    print(json.dumps({'root': str(args.root.resolve()), 'shape': args.shape,
                      'requested_files': args.files, 'requested_directories': args.directories,
                      'creation_order': args.creation_order, 'seed': args.seed,
                      'ordinary_file_bytes_written': bytes_written,
                      'compressed_fixture_flag': compressed,
                      'note': 'mixed adds cross-directory hardlinks, symlinks, special files, sparse, hidden, package and filename fixtures; Darwin adds resource fork and xattr'}))


if __name__ == '__main__':
    main()
