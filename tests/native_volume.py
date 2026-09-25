#!/usr/bin/env python3
"""Exact native volume matrix with an independent oracle and durable evidence."""
from __future__ import annotations

import argparse
import gzip
import json
from pathlib import Path
import subprocess
import sys

from oracle import canonical, oracle


def save_manifest(path: Path, rows: list[dict]) -> None:
    with gzip.open(path, 'wt') as f:
        for row in rows:
            f.write(json.dumps(row, separators=(',', ':')) + '\n')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    expected = {}
    results = []
    cases = []
    for method in ('posix', 'posix-par', 'bulk', 'bulk-par', 'catalog', 'catalog-pipe'):
        for task, size in (('tree', 'logical'), ('usage', 'logical'), ('enumerate', 'logical'),
                           ('tree', 'allocated'), ('usage', 'allocated')):
            cases.append((method, task, size, args.root, []))
    for partitions in (2, 4):
        cases.append(('catalog-partition', 'tree', 'logical', args.root, ['--partitions', str(partitions)]))
    for method in ('catalog', 'catalog-pipe'):
        cases.append((method, 'tree', 'logical', args.root / 'corpus', ['--allow-volume-scan', '--fd-limit', '4']))

    for index, (method, task, size, root, options) in enumerate(cases):
        key = (str(root), task, size)
        label = f'{index:02d}-{method}-{task}-{size}'
        if key not in expected:
            expected[key] = oracle(root, task, size)
            save_manifest(args.output / f'oracle-{index:02d}.jsonl.gz', expected[key])
        want = expected[key]
        manifest = args.output / f'{label}.jsonl'
        cmd = [str(args.executable.resolve()), 'scan', str(root), '--method', method,
               '--task', task, '--size', size, '--consistency', 'immutable',
               '--manifest-json', str(manifest), *options]
        try:
            run = subprocess.run(cmd, text=True, capture_output=True, timeout=360)
        except subprocess.TimeoutExpired as error:
            def decode(value):
                return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
            run = subprocess.CompletedProcess(cmd, 124, decode(error.stdout), decode(error.stderr) + '\nscan timed out')
        except OSError as error:
            run = subprocess.CompletedProcess(cmd, 1, '', str(error))
        (args.output / f'{label}-result.jsonl').write_text(run.stdout)
        (args.output / f'{label}.log').write_text(run.stderr)
        summary = {'case': label, 'argv': cmd, 'exit_code': run.returncode, 'passed': False, 'status': 'failed'}
        try:
            scan = json.loads(run.stdout)
            if method == 'catalog-partition' and run.returncode == 2 and scan['completion'] == 'unsupported':
                summary.update(status='unsupported', reason=scan['reason'])
                results.append(summary)
                (args.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
                print(f'UNSUPPORTED {label}: {scan["reason"]}', flush=True)
                continue
            got = canonical([json.loads(line) for line in manifest.read_text().splitlines()])
            save_manifest(manifest.with_suffix('.jsonl.gz'), got)
            manifest.unlink()
            if got != want:
                reference = {(r['device'], r['parent'], r['name_hex']): r for r in want}
                candidate = {(r['device'], r['parent'], r['name_hex']): r for r in got}
                differences = [{'key': k, 'expected': reference.get(k), 'actual': candidate.get(k)}
                               for k in sorted(reference.keys() | candidate.keys())
                               if reference.get(k) != candidate.get(k)]
                (args.output / f'{label}-differences.json').write_text(json.dumps(differences, indent=2) + '\n')
            assert run.returncode == 0 and scan['completion'] == 'complete', scan.get('reason')
            assert got == want, 'full manifest differs from independent oracle'
            assert scan['entries'] == len(want)
            total = sum(row['size'] for row in want if row['kind'] == 1)
            unique = {(row['device'], row['id']): row['size'] for row in want if row['kind'] == 1}
            assert scan['bytes'] == total and scan['unique_bytes'] == sum(unique.values())
            assert scan['setup_ns'] + scan['scan_ns'] + scan['finalize_ns'] == scan['total_ns']
            assert task != 'tree' or scan['graph_valid']
            summary['passed'] = True
            summary['status'] = 'passed'
        except (AssertionError, ValueError, OSError, KeyError, TypeError) as error:
            summary['failure'] = str(error)
        results.append(summary)
        (args.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
        print(f'{"PASS" if summary["passed"] else "FAIL"} {label}', flush=True)
    failures = sum(row['status'] == 'failed' for row in results)
    passed = sum(row['status'] == 'passed' for row in results)
    unsupported = sum(row['status'] == 'unsupported' for row in results)
    print(f'native volume: {passed} passed, {unsupported} unsupported experimental cases, {failures} failed')
    return int(bool(failures))


if __name__ == '__main__':
    sys.exit(main())
