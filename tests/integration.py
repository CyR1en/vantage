#!/usr/bin/env python3
"""Independent os.scandir/lstat oracle and end-to-end CLI tests.

The scanner has no Python dependency. Python is used only for test/fixture tools.
All test data and outputs are confined to a newly created temporary directory.
"""
from __future__ import annotations

import json
import contextlib
import shutil
import traceback
import os
from pathlib import Path
import resource
import socket
import subprocess
import sys
import tempfile

from oracle import canonical, oracle

EXE = Path(sys.argv[1]).resolve()
RUNS = 0


def run(*args: object, expected: tuple[int, ...] = (0,), nofile: int | None = None) -> subprocess.CompletedProcess[str]:
    global RUNS
    RUNS += 1
    def limit() -> None:
        if nofile is not None:
            _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            resource.setrlimit(resource.RLIMIT_NOFILE, (min(nofile, hard), hard))
    result = subprocess.run([str(EXE), *(str(arg) for arg in args)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=40, preexec_fn=limit if nofile is not None else None)
    if result.returncode not in expected:
        raise AssertionError(f"command {args} exited {result.returncode}\n{result.stdout}\n{result.stderr}")
    if 'runtime error:' in result.stderr or 'ERROR: AddressSanitizer' in result.stderr:
        raise AssertionError(result.stderr)
    return result


def records(result: subprocess.CompletedProcess[str]) -> list[dict]:
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def assert_manifest(root: Path, target: Path, *options: object, task: str = 'tree', size_contract: str = 'logical') -> dict:
    result = records(run('scan', root, '--task', task, '--size', size_contract, '--manifest-json', target, *options))[0]
    got = canonical([json.loads(line) for line in target.read_text().splitlines()])
    want = oracle(root, task, size_contract)
    assert got == want, f"oracle mismatch for {options} / {task}"
    assert result['entries'] == len(want) and result['completion'] == 'complete'
    assert result['setup_ns'] + result['scan_ns'] + result['finalize_ns'] == result['total_ns']
    if task == 'tree':
        assert result['graph_valid']
    total = sum(x['size'] for x in want if x['kind'] == 1)
    unique = {(x['device'], x['id']): x['size'] for x in want if x['kind'] == 1}
    assert result['bytes'] == total and result['unique_bytes'] == sum(unique.values())
    return result



@contextlib.contextmanager
def workspace():
    with tempfile.TemporaryDirectory(prefix='scanbench-test-', dir='/tmp') as temp:
        work = Path(temp)
        try:
            yield work
        finally:
            destination = os.environ.get('SCANBENCH_TEST_OUTPUT')
            if destination:
                output = Path(destination)
                output.mkdir(parents=True, exist_ok=True)
                for path in work.iterdir():
                    if path.is_file() and path.suffix in ('.json', '.jsonl', '.log'):
                        shutil.copy2(path, output / path.name)


def native_checks(root: Path, work: Path) -> None:
    failures = []

    def check(label, operation):
        try:
            operation()
        except Exception:
            detail = traceback.format_exc()
            failures.append(label)
            (work / f'{label}-failure.log').write_text(detail)
            print(f'FAIL {label}\n{detail}', file=sys.stderr)

    for method in ('bulk', 'bulk-par'):
        for pack in (False, True):
            for omit in (False, True):
                for uuid in (False, True):
                    label = f'{method}-pack{int(pack)}-omit{int(omit)}-uuid{int(uuid)}'
                    opts = ['--method', method, '--workers', 4]
                    if pack:
                        opts += ['--pack-invalid']
                    if omit:
                        opts += ['--omit-objtype']
                    if uuid:
                        opts += ['--extra', 'uuid']
                    check(label, lambda opts=opts, label=label: assert_manifest(root, work / f'{label}.jsonl', *opts))
        label = f'{method}-skip'
        check(label, lambda method=method, label=label: assert_manifest(
            root, work / f'{label}.jsonl', '--method', method, '--workers', 4,
            '--pack-invalid', '--skip-empty', '--consistency', 'immutable'))
        for task in ('usage', 'enumerate'):
            label = f'{method}-{task}'
            check(label, lambda method=method, label=label, task=task: assert_manifest(
                root, work / f'{label}.jsonl', '--method', method, '--workers', 4,
                '--pack-invalid', '--omit-objtype', task=task))
    for method in ('posix', 'bulk', 'bulk-par'):
        for task in ('tree', 'usage', 'enumerate'):
            label = f'{method}-{task}-allocated'
            check(label, lambda method=method, task=task, label=label: assert_manifest(
                root, work / f'{label}.jsonl', '--method', method, task=task, size_contract='allocated'))

    denied = work / 'denied'; denied.mkdir()
    (denied / 'file').write_bytes(b'permission fixture')
    (denied / 'dir').mkdir()
    paths = [str(denied / 'file'), str(denied / 'dir')]
    try:
        subprocess.run(['chmod', '+a', 'everyone deny readattr', *paths], check=True)
        for method in ('bulk', 'bulk-par'):
            for pack in (False, True):
                label = f'{method}-permission-pack{int(pack)}'
                def permission_case(method=method, pack=pack):
                    opts = ['--method', method, '--extra', 'uuid']
                    if pack:
                        opts += ['--pack-invalid']
                    result = records(run('scan', denied, *opts, expected=(1,)))[0]
                    assert result['completion'] == 'partial' and result['errno'] in (1, 13)
                    assert result['counters']['permission_errors'] == 2
                    assert result['counters']['malformed_records'] == 0
                check(label, permission_case)
    finally:
        subprocess.run(['chmod', '-N', *paths], check=True)
    assert not failures, f'native cases failed: {", ".join(failures)}'


def main() -> None:
    with workspace() as work:
        root = work / 'fixture'; root.mkdir()
        (root / 'zero').touch()
        (root / '.hidden').write_bytes(b'abc')
        (root / 'data').write_bytes(b'0123456789')
        os.link(root / 'data', root / 'hardlink')
        os.symlink('.', root / 'loop')
        os.symlink('does-not-exist', root / 'dangling')
        os.mkfifo(root / 'fifo')
        sock = socket.socket(socket.AF_UNIX); sock.bind(str(root / 'socket'))
        (root / 'Thing.app' / 'Contents').mkdir(parents=True)
        (root / 'Thing.app' / 'Contents' / 'binary').write_bytes(b'abc\x00def')
        for name in ('spaces and "quotes"', 'tab\tname', 'newline\nname', '雪', 'e\u0301'):
            (root / name).write_bytes(name.encode())
        if sys.platform != 'darwin':
            fd = os.open(os.fsencode(root) + b'/invalid-\xff', os.O_CREAT | os.O_WRONLY, 0o600)
            os.write(fd, b'x'); os.close(fd)
        with (root / 'sparse').open('wb') as f:
            f.seek(16 * 1024 * 1024); f.write(b'x')
        for j in range(640):
            directory = root / f'd{j:03d}'; directory.mkdir()
            if j % 3:
                (directory / 'file').write_bytes(bytes([j & 255]) * (j + 1))
        deep = root
        for _ in range(80):
            deep = deep / 'deep'; deep.mkdir()
        (deep / 'last').write_bytes(b'end')
        os.link(root / 'data', deep / 'cross-directory-hardlink')
        os.symlink('data', root / 'file-link')
        if sys.platform == 'darwin':
            forked = root / 'resource-fork'
            forked.write_bytes(b'data fork')
            with open(str(forked) + '/..namedfork/rsrc', 'wb') as f:
                f.write(b'R' * 16384)
            subprocess.run(['xattr', '-w', 'com.scanbench.fixture', 'extended metadata' * 128, str(forked)], check=True)

        expected = assert_manifest(root, work / 'posix.jsonl', '--method', 'posix')
        for order in ('dfs', 'fifo', 'random', 'id-band'):
            got = assert_manifest(root, work / f'{order}.jsonl', '--method', 'posix-par',
                                  '--workers', 4, '--order', order, '--fd-limit', 12, '--reduce', 'hash')
            assert got['digest'] == expected['digest']
            assert got['counters']['reopens'] > 1
        assert_manifest(root, work / 'adaptive.jsonl', '--method', 'posix-par', '--workers', 4, '--adaptive')
        for task in ('enumerate', 'usage'):
            assert_manifest(root, work / f'{task}.jsonl', '--method', 'posix-par', '--workers', 2, task=task)
        low = records(run('scan', root, '--method', 'posix-par', '--workers', 2,
                          '--fd-limit', 256, nofile=40))[0]
        assert low['completion'] == 'complete' and low['digest'] == expected['digest']
        empty = work / 'empty'; empty.mkdir()
        assert_manifest(empty, work / 'empty.jsonl', '--method', 'posix-par', '--workers', 8)
        manifest = work / 'inventory.sbi'
        run('scan', root, '--manifest', manifest)
        for reduction in ('sort', 'hash'):
            replay = records(run('replay', manifest, '--rounds', 3, '--reduce', reduction))
            assert len(replay) == 3 and all(x['verification'] == 'verified' and x['method'] == 'collector-replay' for x in replay)
        bad = work / 'truncated.sbi'; bad.write_bytes(manifest.read_bytes()[:70])
        run('replay', bad, expected=(1,))
        output = work / 'bench.jsonl'
        run('bench', root, '--methods', 'posix,posix-par', '--workers', '1,4',
            '--rounds', 3, '--warmup', 1, '--out', output)
        rows = [json.loads(line) for line in output.read_text().splitlines()]
        trials = [r for r in rows if r['event'] == 'trial']
        assert len(trials) == 9 and all(r['verification'] == 'verified' for r in trials)
        assert len({r['session'] for r in rows}) == 1
        report = run('report', output).stdout
        assert '95% percentile bootstrap' in report and '9 complete' in report
        first = records(run('bench', root, '--methods', 'posix', '--cache', 'first-pass'))
        assert len(first) == 1 and first[0]['verification'] == 'unverified'
        run('bench', root, '--methods', 'posix,posix-par', '--cache', 'first-pass', expected=(64,))
        run('scan', root, '--workers', 0, expected=(64,))
        run('scan', root, '--method', 'not-a-method', expected=(64,))
        small = records(run('scan', root, '--memory-limit', '4KiB', expected=(1,)))[0]
        assert small['completion'] == 'failed'
        limited = records(run('scan', root, '--queue-limit', 1, expected=(1,)))[0]
        assert limited['completion'] == 'failed'
        malicious = work / 'malformed.jsonl'; malicious.write_text('{"schema":1,"event":"trial",}\n')
        run('report', malicious, expected=(1,))
        if sys.platform == 'darwin':
            native_checks(root, work)
        else:
            for method in ('bulk', 'bulk-par'):
                result = records(run('scan', root, '--method', method, expected=(2,)))[0]
                assert result['completion'] == 'unsupported' and result['method'] == method
            run('scan', root, '--size', 'allocated', expected=(2,))
        sock.close()
    print(f'integration: {RUNS} CLI invocations passed; independent filesystem oracle, hard links, sparse files, no-follow, invalid names, deep paths, low FD limits, reductions, benchmark protocol, failure paths and replay')


if __name__ == '__main__':
    main()
