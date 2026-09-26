#!/usr/bin/env python3
"""Independent filesystem and real-terminal checks for the vantage CLI.

Usage: python3 tests/vantage.py build/vantage
Creates only temporary fixtures; imports no production implementation.
"""
from __future__ import annotations

from collections import defaultdict
import errno
import fcntl
import json
import math
import os
from pathlib import Path
import resource
import select
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import termios
import time

from native_volume import TOTAL_FIELDS, decode_export
from oracle import allocated_size, oracle

EXE = Path(sys.argv[1]).resolve()
RUNS = 0
SKIPS: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(*args: object, cwd: Path | None = None, success: bool = True,
        nofile: int | None = None) -> subprocess.CompletedProcess[bytes]:
    global RUNS
    RUNS += 1
    command = [str(EXE), *(str(arg) for arg in args)]
    def limit_descriptors() -> None:
        if nofile is not None:
            _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            resource.setrlimit(resource.RLIMIT_NOFILE, (min(nofile, hard), hard))

    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=40,
                            preexec_fn=limit_descriptors if nofile is not None else None)
    detail = f'{command!r}: exit {result.returncode}\nstdout={result.stdout!r}\nstderr={result.stderr!r}'
    require((result.returncode == 0) == success, detail)
    require(b'ERROR: AddressSanitizer' not in result.stderr and b'runtime error:' not in result.stderr, detail)
    if not success:
        require(bool(result.stdout.strip() or result.stderr.strip()), f'failure had no explanation: {detail}')
    return result


def document(root: Path, *args: object, success: bool = True, nofile: int | None = None) -> dict:
    result = run(root, '--json', *args, success=success, nofile=nofile)
    try:
        value = json.loads(result.stdout)
    except (ValueError, UnicodeError) as error:
        raise AssertionError(f'invalid JSON from {args!r}: {result.stdout!r}\n{result.stderr!r}') from error
    require(isinstance(value, dict), f'JSON must be an object: {value!r}')
    return value


def expected(root: Path, allocated: bool = False) -> tuple[dict[bytes, dict], int, int]:
    """Compute path totals with lstat; count hardlinked objects independently."""
    rows: dict[bytes, dict] = {}
    unique = {}
    root_bytes = os.fsencode(root)

    def walk(directory: bytes, prefix: bytes) -> int:
        total = 0
        with os.scandir(directory) as entries:
            children = list(entries)
        for child in children:
            info = child.stat(follow_symlinks=False)
            path = prefix + child.name
            if stat.S_ISDIR(info.st_mode):
                kind = 'directory'
                size = walk(child.path, path + b'/')
            elif stat.S_ISREG(info.st_mode):
                kind = 'file'
                size = allocated_size(child.path) if allocated else info.st_size
                unique[(info.st_dev, info.st_ino)] = size
            else:
                kind = 'symlink' if stat.S_ISLNK(info.st_mode) else 'other'
                size = 0
            rows[path] = {'kind': kind, 'bytes': size, 'depth': path.count(b'/') + 1}
            total += size
        return total

    total = walk(root_bytes, b'')
    return rows, total, sum(unique.values())


def item_path(item: dict) -> bytes:
    require(isinstance(item.get('path'), str), f'item missing display path: {item!r}')
    try:
        return bytes.fromhex(item['path_hex'])
    except (KeyError, TypeError, ValueError) as error:
        raise AssertionError(f'item lacks lossless path_hex: {item!r}') from error


def check_items(items: list[dict], want: dict[bytes, dict], total: int, *, files: bool = False) -> set[bytes]:
    paths = set()
    siblings: dict[bytes, list[int]] = defaultdict(list)
    for item in items:
        path = item_path(item)
        require(path in want and path not in paths, f'unknown or repeated path {path!r}')
        paths.add(path)
        value = want[path]
        for key in ('kind', 'bytes', 'depth'):
            require(item.get(key) == value[key], f'{path!r}: {key}={item.get(key)!r}; expected {value[key]!r}')
        require(item.get('unknown_sizes') == 0, f'known fixture size marked unknown: {item!r}')
        parent = path.rpartition(b'/')[0]
        denominator = total if files or not parent else want[parent]['bytes']
        percent = 100 * value['bytes'] / denominator if denominator else 0
        require(isinstance(item.get('percent'), (int, float)) and math.isclose(item['percent'], percent, abs_tol=.011),
                f'{path!r}: incorrect percentage {item.get("percent")!r}; expected {percent}')
        if files:
            require(value['kind'] == 'file', f'global files list contains {path!r}')
        siblings[b'' if files else parent].append(value['bytes'])
    for parent, sizes in siblings.items():
        require(sizes == sorted(sizes, reverse=True), f'items not ranked by bytes under {parent!r}: {sizes}')
    return paths


def check_document(value: dict, root: Path, want: dict[bytes, dict], total: int, unique: int) -> None:
    require(value.get('schema') == 1, f'unknown JSON schema: {value!r}')
    require(os.path.realpath(bytes.fromhex(value['root_hex'])) == os.fsencode(root.resolve()), 'root_hex does not identify requested root')
    require(isinstance(value.get('root'), str), 'missing root display path')
    require(value.get('completion') == 'complete' and value.get('graph_valid') is True, f'fixture scan incomplete: {value!r}')
    require(value.get('total_bytes') == total and value.get('unique_bytes') == unique,
            f'incorrect path/unique totals: got {value.get("total_bytes")}/{value.get("unique_bytes")}, expected {total}/{unique}')
    require(value.get('entries') == len(want), f'incorrect entry count: {value.get("entries")} != {len(want)}')
    require(value.get('files') == sum(row['kind'] == 'file' for row in want.values()), 'incorrect file count')
    require(value.get('directories') == sum(row['kind'] == 'directory' for row in want.values()), 'incorrect directory count')
    for field in ('scan_ns', 'index_ns'):
        require(isinstance(value.get(field), int) and value[field] >= 0, f'missing timing {field}')
    require(isinstance(value.get('method'), str) and isinstance(value.get('workers'), int)
            and isinstance(value.get('buffer_bytes'), int), 'missing scanner configuration')
    require('reason' in value, 'missing completion reason')


def fixture(work: Path) -> Path:
    root = work / 'fixture'; root.mkdir()
    for directory in ('alpha/nested', 'beta', 'empty'):
        (root / directory).mkdir(parents=True)
    for path, size in (('alpha/big', 7001), ('alpha/nested/inside', 1201), ('beta/small', 23), ('root-file', 311)):
        (root / path).write_bytes(b'x' * size)
    os.link(root / 'alpha/big', root / 'beta/cross-directory-hardlink')
    for name in ('line\nbreak', 'escape\x1b[31m', 'tab\tname', 'snow-雪', 'quote-"-backslash-\\'):
        (root / name).write_bytes(b'name')
    (root / ('long-' + 'n' * 180 + '.txt')).write_bytes(b'long name')
    try:
        fd = os.open(os.fsencode(root) + b'/invalid-\xff', os.O_CREAT | os.O_WRONLY, 0o600)
        try:
            os.write(fd, b'raw-name')
        finally:
            os.close(fd)
    except OSError as error:
        if error.errno not in (errno.EILSEQ, errno.EINVAL):
            raise
        SKIPS.append('filesystem rejects invalid UTF-8 names')
    (work / 'outside').write_bytes(b'outside' * 1000)
    os.symlink('../outside', root / 'outside-link')
    os.symlink('.', root / 'loop')
    os.symlink('missing', root / 'dangling')
    os.mkfifo(root / 'pipe')
    with socket.socket(socket.AF_UNIX) as endpoint:
        endpoint.bind(str(root / 'socket'))
    (root / 'zero').touch()
    (root / '.hidden').write_bytes(b'hidden')
    (root / 'Thing.app' / 'Contents').mkdir(parents=True)
    (root / 'Thing.app' / 'Contents' / 'binary').write_bytes(b'app payload')
    if sys.platform == 'darwin':
        forked = root / 'resource-fork'
        forked.write_bytes(b'data fork')
        with open(str(forked) + '/..namedfork/rsrc', 'wb') as stream:
            stream.write(b'R' * 16384)
        subprocess.run(['xattr', '-w', 'dev.vantage.fixture', 'metadata' * 128, str(forked)], check=True)
    with (root / 'sparse').open('wb') as stream:
        stream.write(b'x')
        stream.truncate(8 * 1024 * 1024 + 1)
    return root


def fixture_checks(work: Path) -> None:
    root = fixture(work)
    want, total, unique = expected(root)
    baseline = document(root, '--method', 'posix', '--top', 10000, '--depth', 8)
    check_document(baseline, root, want, total, unique)
    require(baseline['view'] == 'tree', 'default view is not tree')
    require(baseline.get('size') == 'logical', 'default size contract must be logical')
    require(check_items(baseline['items'], want, total) == set(want), 'depth8/top10000 omitted fixture paths')
    check_items(baseline['largest_files'], want, total, files=True)
    require(len(baseline['largest_files']) == baseline['files'], 'largest_files omitted files with unrestricted top')
    require(total - unique == 7001, 'hardlink fixture failed to create expected duplicate accounting')
    automatic = document(root, '--top=10000', '--depth=8')
    check_document(automatic, root, want, total, unique)
    if sys.platform == 'darwin':
        require(automatic['workers'] == min((os.cpu_count() or 8) * 2, 16), 'macOS default worker budget changed')
        require(automatic['buffer_bytes'] == 65536, 'macOS default bulk buffer changed')
    require(check_items(automatic['items'], want, total) == set(want), 'automatic scanner disagrees with POSIX')
    files = document(root, '--files', '--top=3')
    require(files['view'] == 'files' and len(files['items']) == 3, '--files/top did not restrict global ranking')
    selected = check_items(files['items'], want, total, files=True)
    require(sorted(want[path]['bytes'] for path in selected) == sorted(row['bytes'] for row in want.values() if row['kind'] == 'file')[-3:], 'global top3 are not the largest files')
    shallow = document(root, '--top', 10000)
    require(check_items(shallow['items'], want, total) == {p for p in want if b'/' not in p}, 'default depth should show immediate children')
    for depth in (1, 2, 3):
        value = document(root, '--depth', depth, '--top', 10000)
        require(check_items(value['items'], want, total) == {p for p in want if want[p]['depth'] <= depth}, f'depth{depth} incorrect')
    for top in (1, 2):
        value = document(root, '--top', top, '--depth', 8)
        check_items(value['items'], want, total)
        counts = defaultdict(int)
        for item in value['items']:
            path = item_path(item); parent = path.rpartition(b'/')[0]
            counts[parent] += 1
            require(counts[parent] <= top, 'top limit exceeded for one parent')
        require(counts[b''] == top, 'top limit returned too few root entries')
    for options in ((), ('--no-interactive',), ('--no-interactive', '--files', '--top', 10000)):
        report = run(root, *options).stdout
        require(b'\x1b' not in report and b'\x00' not in report and b'\r' not in report, f'piped/static report contains terminal controls: {report!r}')
        if options == ():
            require(b'line\nbreak' not in report and b'tab\tname' not in report, 'filenames injected report lines/tabs')
            require(b'long-' + b'n' * 180 + b'.txt' in report, 'plain report clipped a long filename')
        require(bool(report.strip()), 'empty human report')
    current = json.loads(run('--json', cwd=root).stdout)
    check_document(current, root, want, total, unique)
    for options in (('--workers=1', '--fd-limit=4'), ('--workers', 64, '--timeout=10', '--memory-limit=16MiB')):
        value = document(root, *options)
        check_document(value, root, want, total, unique)
        require(value['workers'] == (1 if options[0] == '--workers=1' else 64), 'explicit worker count was overridden')
    for limit in ('1073741824', '1048576KiB', '1GiB'):
        check_document(document(root, '--memory-limit', limit), root, want, total, unique)
    if sys.platform == 'darwin':
        allocation, alloc_total, alloc_unique = expected(root, allocated=True)
        require(allocation[b'sparse']['bytes'] < want[b'sparse']['bytes'], 'fixture is not sparse on this filesystem')
        for options in (('--allocated',), ('--size=allocated', '--method=posix')):
            value = document(root, '--top=10000', '--depth=8', *options)
            require(value.get('size') == 'allocated', 'allocated option did not select allocated contract')
            check_document(value, root, allocation, alloc_total, alloc_unique)
            require(check_items(value['items'], allocation, alloc_total) == set(allocation), 'allocated view omitted paths')
    else:
        SKIPS.append('Darwin allocated-size oracle requires macOS')
    empty = work / 'empty-root'; empty.mkdir()
    value = document(empty)
    check_document(value, empty, {}, 0, 0)
    require(value['items'] == [] and value['largest_files'] == [], 'empty root yielded entries')
    many = work / 'default-top'; many.mkdir()
    for index in range(25):
        (many / f'file-{index:02d}').write_bytes(b'x' * (index + 1))
    value = document(many)
    require(len(value['items']) == 20 and len(value['largest_files']) == 20, 'default top must be 20 per list')
    require([item['bytes'] for item in value['items']] == list(range(25, 5, -1)), 'default top20 ranking incorrect')
    dash = work / '-root'; dash.mkdir(); (dash / 'data').write_bytes(b'abc')
    value = json.loads(run('--json', '--', '-root', cwd=work).stdout)
    check_document(value, dash, *expected(dash))
    for options in (('--top', 0), ('--top=10001',), ('--depth=0',), ('--depth=9',),
                    ('--workers=0',), ('--workers=65',), ('--fd-limit=3',), ('--size=bogus',),
                    ('--method=bogus',), ('--timeout=-1',), ('--memory-limit=bogus',),
                    ('--memory-limit=0',), ('--memory-limit=18446744073709551615GiB',),
                    ('--timeout=18446744073709551615',), ('--interactive',),
                    ('--json', '--interactive'), ('--unknown',), ('--top',),
                    ('--depth=1x',), ('--workers=-1',)):
        run(root, *options, success=False)
    run(work / 'does-not-exist', success=False)
    run(root / 'root-file', success=False)
    require(b'vantage' in run('--help').stdout.lower(), 'help does not identify command')
    require(b'0.' in run('--version').stdout, 'version does not contain version string')


def traversal_checks(work: Path) -> None:
    """Retained product scans handle broad/deep trees under descriptor limits."""
    root = work / 'traversal'; root.mkdir()
    for index in range(640):
        directory = root / f'd{index:03d}'; directory.mkdir()
        if index % 3:
            (directory / 'payload').write_bytes(bytes([index & 255]) * (index + 1))
    deep = root
    for _ in range(80):
        deep /= 'deep'; deep.mkdir()
    (deep / 'last').write_bytes(b'end')
    os.link(root / 'd001/payload', deep / 'hardlink')
    os.symlink('.', root / 'cycle')
    want, total, unique = expected(root)
    inventory = oracle(root)
    methods = ('posix', 'bulk') if sys.platform == 'darwin' else ('posix',)
    for method in methods:
        for workers, fd_limit, nofile in ((1, 4, None), (4, 12, None), (4, 256, 40)):
            value = document(root, '--method', method, '--workers', workers,
                             '--fd-limit', fd_limit, '--top', 10000, '--depth', 8,
                             '--timeout', 0, nofile=nofile)
            check_document(value, root, want, total, unique)
            require(check_items(value['items'], want, total) ==
                    {path for path, row in want.items() if row['depth'] <= 8},
                    'bounded descriptors omitted visible paths')
            require(check_items(value['largest_files'], want, total, files=True) ==
                    {path for path, row in want.items() if row['kind'] == 'file'},
                    'bounded descriptors omitted deeply nested files')
            exported = decode_export(run(root, '--export', '--method', method,
                                         '--workers', workers, '--fd-limit', fd_limit,
                                         nofile=nofile).stdout)
            require(exported['completion'] == 0 and exported['entries'] == inventory['entries'],
                    'export disagrees with independent complete tree inventory')
            require(all(exported[field] == inventory[field] for field in TOTAL_FIELDS),
                    'export totals disagree with independent filesystem accounting')
    limited = run(root, '--memory-limit', '4KiB', success=False)
    require(limited.returncode == 71 and b'managed memory limit exceeded' in limited.stderr,
            'memory exhaustion did not produce the documented resource failure')
    if sys.platform != 'darwin':
        for options in (('--method', 'bulk'), ('--allocated',)):
            result = run(root, '--json', *options, success=False)
            value = json.loads(result.stdout)
            require(result.returncode == 2 and value['completion'] == 'unsupported',
                    'unavailable native scan contract was not explicitly unsupported')


def permission_check(work: Path) -> None:
    root = work / 'permission'; root.mkdir()
    denied = root / 'denied'; denied.mkdir(); (denied / 'data').write_bytes(b'x')
    if sys.platform == 'darwin':
        setup = subprocess.run(['chmod', '+a', 'everyone deny list', str(denied)], capture_output=True)
        if setup.returncode:
            SKIPS.append(f'ACL permission fixture unavailable: {setup.stderr.decode(errors="replace").strip()}')
            return
        cleanup = lambda: subprocess.run(['chmod', '-N', str(denied)], check=True, capture_output=True)
    elif os.geteuid() != 0:
        denied.chmod(0)
        cleanup = lambda: denied.chmod(0o700)
    else:
        SKIPS.append('permission denial cannot be established while running as root')
        return
    try:
        try:
            with os.scandir(denied) as entries:
                list(entries)
        except PermissionError:
            pass
        else:
            SKIPS.append('filesystem did not enforce the permission-denial fixture')
            return
        for method in ('auto', 'posix'):
            value = document(root, '--method', method, success=False)
            require(value.get('completion') in ('partial', 'failed'), f'denied scan mislabeled complete: {value!r}')
            require(bool(value.get('reason')), f'denied scan missing reason: {value!r}')
            if value['completion'] == 'partial':
                plain = run(root, '--method', method, '--no-interactive', success=False)
                require(b'lower bounds' in plain.stdout.lower(), f'partial report omitted lower-bound warning: {plain.stdout!r}')
    finally:
        cleanup()


class Terminal:
    def __init__(self, root: Path, *args: str, stderr_pty: bool = True):
        global RUNS
        RUNS += 1
        self.master, self.slave = os.openpty()
        self.before = termios.tcgetattr(self.slave)
        self.interactive = '--no-interactive' not in args
        self.resize(24, 100, notify=False)
        self.process = subprocess.Popen([str(EXE), str(root), *args], stdin=self.slave,
                                        stdout=self.slave, stderr=self.slave if stderr_pty else subprocess.PIPE,
                                        env={**os.environ, 'TERM': 'xterm-256color'})
        self.output = bytearray()

    def resize(self, rows: int, columns: int, *, notify: bool = True) -> None:
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
        if notify:
            self.process.send_signal(signal.SIGWINCH)

    def read(self, deadline: float) -> bool:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        ready, _, _ = select.select([self.master], [], [], min(remaining, .2))
        if ready:
            try:
                data = os.read(self.master, 65536)
                self.output.extend(data)
                return bool(data)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
        return False

    def expect(self, marker: bytes, offset: int = 0) -> None:
        deadline = time.monotonic() + 8
        while marker not in self.output[offset:] and time.monotonic() < deadline:
            self.read(deadline)
            if self.process.poll() is not None:
                break
        require(marker in self.output[offset:], f'TTY missing {marker!r}: exit={self.process.poll()}, output={bytes(self.output[offset:])!r}')

    def send(self, keys: bytes, marker: bytes) -> None:
        offset = len(self.output)
        os.write(self.master, keys)
        self.expect(marker, offset)

    def finish(self, code: int) -> None:
        deadline = time.monotonic() + 8
        while self.process.poll() is None and time.monotonic() < deadline:
            self.read(deadline)
        require(self.process.poll() == code, f'TTY exit {self.process.poll()}, expected {code}: {bytes(self.output)!r}')
        while select.select([self.master], [], [], 0)[0]:
            if not self.read(time.monotonic() + 1):
                break
        # Darwin sets PENDIN when raw mode returns to canonical mode, even for
        # an isolated tcsetattr roundtrip. Flush test input to clear that state.
        if sys.platform == 'darwin':
            termios.tcflush(self.slave, termios.TCIFLUSH)
        after = termios.tcgetattr(self.slave)
        require(after == self.before, f'terminal attributes were not restored exactly: before={self.before!r}, after={after!r}')
        if self.interactive:
            require(b'\x1b[?25h' in self.output and b'\x1b[?1049l' in self.output, 'TUI did not restore cursor/alternate-screen state')

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.kill(); self.process.wait(timeout=5)
        if self.process.stderr is not None:
            self.process.stderr.close()
        os.close(self.master); os.close(self.slave)


def terminal_checks(work: Path) -> None:
    root = work / 'terminal'; (root / 'alpha/nested').mkdir(parents=True)
    (root / 'alpha/big').write_bytes(b'x' * 100)
    (root / 'alpha/nested/small').write_bytes(b'x')
    breadcrumb = b'At: ' + os.fsencode(root)
    terminal = Terminal(root)
    try:
        terminal.expect(breadcrumb + b'\x1b')
        terminal.send(b'\r', breadcrumb + b'/alpha\x1b')
        terminal.send(b'\x7f', breadcrumb + b'\x1b')
        terminal.send(b'f', b'Biggest files')
        terminal.send(b'g', b'vantage | Tree')
        terminal.resize(6, 12)
        terminal.send(b'f', b'\x1b')
        offset = len(terminal.output)
        terminal.resize(24, 100)
        terminal.expect(b'Biggest files', offset)
        terminal.send(b'g', breadcrumb + b'\x1b')
        os.write(terminal.master, b'q')
        terminal.finish(0)
    finally:
        terminal.close()
    for stop, expected_code in ((b'\x03', 130), (signal.SIGTERM, 143), (b'\x1a', 0), (b'\x1b', 0)):
        terminal = Terminal(root, '--interactive')
        try:
            terminal.expect(breadcrumb)
            if isinstance(stop, bytes):
                os.write(terminal.master, stop)
            else:
                terminal.process.send_signal(stop)
            terminal.finish(expected_code)
        finally:
            terminal.close()
    terminal = Terminal(root, '--no-interactive', stderr_pty=False)
    try:
        terminal.finish(0)
        require(b'\x1b' not in terminal.output, '--no-interactive emitted ANSI to a terminal')
        require(bool(terminal.output), '--no-interactive emitted no report')
    finally:
        terminal.close()


def export_checks(work: Path) -> None:
    """The --export stream used by the macOS app: header, totals and every entry."""
    root = work / 'export-root'
    (root / 'dir/inner').mkdir(parents=True)
    (root / 'dir/inner/big.bin').write_bytes(b'x' * 700)
    (root / 'dir/small.txt').write_bytes(b'x' * 20)
    (root / 'top.dat').write_bytes(b'x' * 5)
    # Vary record sizes across several export-buffer boundaries.
    extra = {}
    for i in range(1500):
        name = f'entry-{i:04d}-' + 'x' * (i % 101)
        size = i % 53
        (root / name).write_bytes(b'x' * size)
        extra[os.fsencode(name)] = size
    result = run(root, '--export')
    data = result.stdout
    require(data[:4] == b'SVX1', f'export magic missing: {data[:16]!r}')
    require(b'progress ' in result.stderr, 'export mode did not report progress')
    completion, size_mode = struct.unpack_from('<II', data, 4)
    scan_ns, total, unique, files, dirs, unknown, perms, errors = struct.unpack_from('<8Q', data, 12)
    require(completion == 0 and size_mode == 0, 'export header completion/size contract wrong')
    expected_total = 725 + sum(extra.values())
    require((total, unique, files, dirs) == (expected_total, expected_total, 1503, 2), f'export totals wrong: {total} {unique} {files} {dirs}')
    offset = 12 + 64
    texts = []
    for _ in range(3):
        (length,) = struct.unpack_from('<I', data, offset); offset += 4
        texts.append(data[offset:offset + length]); offset += length
    require(texts[0] == os.fsencode(root.resolve()), f'export root path wrong: {texts[0]!r}')
    (count,) = struct.unpack_from('<Q', data, offset); offset += 8
    entries = []
    for _ in range(count):
        parent, kind, size, _unknown, length = struct.unpack_from('<QBQQI', data, offset)
        offset += struct.calcsize('<QBQQI')
        entries.append((parent, kind, size, data[offset:offset + length])); offset += length
    require(offset == len(data), 'export has trailing or truncated data')
    names = {name: (parent, kind, size) for parent, kind, size, name in entries}
    require(set(names) == {b'dir', b'inner', b'big.bin', b'small.txt', b'top.dat'} | extra.keys(), 'export names wrong')
    require(all(names[name] == (count, 1, size) for name, size in extra.items()), 'export records corrupted across buffer boundaries')
    require(names[b'dir'][1:] == (2, 720) and names[b'inner'][1:] == (2, 700), 'export directory totals wrong')
    require(names[b'top.dat'][0] == count and entries[names[b'big.bin'][0]][3] == b'inner', 'export parent links wrong')
    run(root, '--export', '--json', success=False)


def main() -> None:
    require(EXE.is_file(), f'vantage executable missing: {EXE}')
    with tempfile.TemporaryDirectory(prefix='vantage-test-', dir='/tmp') as temporary:
        work = Path(temporary).resolve()
        fixture_checks(work)
        traversal_checks(work)
        permission_check(work)
        export_checks(work)
        terminal_checks(work)
    print(f'vantage: {RUNS} CLI/PTY invocations passed')
    for reason in SKIPS:
        print(f'SKIP: {reason}')


if __name__ == '__main__':
    main()
