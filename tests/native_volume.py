#!/usr/bin/env python3
"""Validate complete Vantage exports against the filesystem and retain evidence."""
from __future__ import annotations

import argparse
import gzip
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from oracle import canonical, oracle


TOTAL_FIELDS = ('total_bytes', 'unique_bytes', 'files', 'directories', 'unknown_sizes')


def save_manifest(path: Path, rows: list[dict]) -> None:
    with gzip.open(path, 'wt') as stream:
        for row in rows:
            stream.write(json.dumps(row, separators=(',', ':')) + '\n')


def decode_export(data: bytes) -> dict:
    """Read public SVX1 records independently of the C and Swift implementations."""
    offset = 0

    def take(length: int) -> bytes:
        nonlocal offset
        if length > len(data) - offset:
            raise ValueError('truncated SVX1 export')
        value = data[offset:offset + length]
        offset += length
        return value

    def unpack(layout: str) -> tuple:
        return struct.unpack(layout, take(struct.calcsize(layout)))

    def text_field() -> bytes:
        return take(unpack('<I')[0])

    if take(4) != b'SVX1':
        raise ValueError('invalid SVX1 magic')
    completion, size = unpack('<II')
    scan_ns, total, unique, files, directories, unknown, permissions, errors = unpack('<8Q')
    root, reason, method = (text_field() for _ in range(3))
    count = unpack('<Q')[0]
    if count > (len(data) - offset) // 30:
        raise ValueError('entry count exceeds remaining SVX1 data')
    entries = []
    for _ in range(count):
        parent, kind, size_bytes, unknown_sizes = unpack('<QBQQ')
        name = text_field()
        if not name or name in (b'.', b'..') or b'/' in name or b'\0' in name:
            raise ValueError('invalid SVX1 entry name')
        if parent > count or kind > 7:
            raise ValueError('invalid SVX1 parent or kind')
        entries.append({'parent': parent, 'name': name, 'kind': kind,
                        'bytes': size_bytes, 'unknown_sizes': unknown_sizes})
    if offset != len(data):
        raise ValueError('trailing SVX1 data')

    paths = {count: b''}
    for index in range(count):
        cursor = index
        pending = []
        visited = set()
        while cursor not in paths:
            if cursor in visited:
                raise ValueError('cycle in SVX1 parent graph')
            visited.add(cursor)
            pending.append(cursor)
            parent = entries[cursor]['parent']
            if parent != count and entries[parent]['kind'] != 2:
                raise ValueError('SVX1 parent is not a directory')
            cursor = parent
        for child in reversed(pending):
            entry = entries[child]
            parent_path = paths[entry['parent']]
            paths[child] = parent_path + (b'/' if parent_path else b'') + entry['name']
    if len(set(paths.values())) != count + 1:
        raise ValueError('duplicate SVX1 relative path')
    rows = [{'path_hex': paths[index].hex(), 'kind': entry['kind'], 'bytes': entry['bytes'],
             'unknown_sizes': entry['unknown_sizes']} for index, entry in enumerate(entries)]
    return {'completion': completion, 'size': size, 'scan_ns': scan_ns, 'root_hex': root.hex(),
            'reason': reason.decode(errors='replace'), 'method': method.decode(errors='replace'),
            'total_bytes': total, 'unique_bytes': unique, 'files': files, 'directories': directories,
            'unknown_sizes': unknown, 'permission_errors': permissions, 'errors': errors,
            'entries': canonical(rows)}


def run_capture(argv: list[str], output: Path, label: str, extension: str) -> subprocess.CompletedProcess:
    try:
        run = subprocess.run(argv, capture_output=True, timeout=360)
    except subprocess.TimeoutExpired as error:
        run = subprocess.CompletedProcess(argv, 124, error.stdout or b'',
                                          (error.stderr or b'') + b'\nscan timed out')
    except OSError as error:
        run = subprocess.CompletedProcess(argv, 1, b'', str(error).encode())
    (output / f'{label}.{extension}').write_bytes(run.stdout)
    (output / f'{label}.stderr').write_bytes(run.stderr)
    return run


def validation_cases(root: Path) -> list[tuple]:
    cases = []
    for method in ('auto', 'posix', 'bulk'):
        for workers in (1, 4):
            for size in ('logical', 'allocated'):
                for target, scope in ((root, 'volume'), (root / 'corpus', 'folder')):
                    cases.append((method, workers, size, target, scope, []))
                    cases.append((method, workers, size, target, scope,
                                  ['--fd-limit', '4' if workers == 1 else '12']))
    return cases


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    expected = {}
    results = []
    for index, (method, workers, size, root, scope, options) in enumerate(validation_cases(args.root)):
        key = (str(root), size)
        label = f'{index:02d}-{method}-j{workers}-{size}-{scope}'
        if options:
            label += f'-fd{options[-1]}'
        cmd = [str(args.executable.resolve()), str(root), '--method', method,
               '--workers', str(workers), '--size', size, *options]
        summary = {'case': label, 'argv': cmd, 'passed': False, 'status': 'failed'}
        try:
            if key not in expected:
                expected[key] = oracle(root, size)
                save_manifest(args.output / f'oracle-{index:02d}.jsonl.gz', expected[key]['entries'])
                (args.output / f'oracle-{index:02d}-totals.json').write_text(json.dumps(
                    {field: expected[key][field] for field in TOTAL_FIELDS}, indent=2) + '\n')
            want = expected[key]
            exported = run_capture([*cmd, '--export'], args.output, f'{label}-export', 'svx')
            reported = run_capture([*cmd, '--json'], args.output, f'{label}-report', 'json')
            summary.update(export_exit_code=exported.returncode, json_exit_code=reported.returncode)
            scan = decode_export(exported.stdout)
            report = json.loads(reported.stdout)
            got = scan['entries']
            save_manifest(args.output / f'{label}.jsonl.gz', got)
            if got != want['entries']:
                reference = {row['path_hex']: row for row in want['entries']}
                candidate = {row['path_hex']: row for row in got}
                differences = [{'path_hex': path, 'expected': reference.get(path), 'actual': candidate.get(path)}
                               for path in sorted(reference.keys() | candidate.keys())
                               if reference.get(path) != candidate.get(path)]
                (args.output / f'{label}-differences.json').write_text(json.dumps(differences, indent=2) + '\n')
            assert exported.returncode == 0 and scan['completion'] == 0, scan['reason']
            assert reported.returncode == 0 and report['completion'] == 'complete', report.get('reason')
            assert scan['size'] == ('logical', 'allocated').index(size) and report['size'] == size
            assert scan['root_hex'] == report['root_hex'] == os.fsencode(root).hex()
            assert got == want['entries'], 'full export differs from independent filesystem oracle'
            assert report['entries'] == len(got) and report['graph_valid']
            assert scan['permission_errors'] == scan['errors'] == 0
            for field in TOTAL_FIELDS:
                assert scan[field] == report[field] == want[field], f'{field} differs from filesystem oracle'
            summary.update(passed=True, status='passed')
        except (AssertionError, ValueError, OSError, KeyError, TypeError) as error:
            summary['failure'] = str(error)
        results.append(summary)
        (args.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
        print(f'{"PASS" if summary["passed"] else "FAIL"} {label}', flush=True)
    failures = sum(row['status'] == 'failed' for row in results)
    print(f'native volume: {len(results) - failures} passed, {failures} failed')
    return int(bool(failures))


if __name__ == '__main__':
    sys.exit(main())
