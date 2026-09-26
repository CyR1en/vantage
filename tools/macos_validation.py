#!/usr/bin/env python3
"""Create a private read-only APFS fixture, validate it, and retain run evidence."""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parent.parent


def interrupted(signum, frame):
    raise KeyboardInterrupt


def command(argv: list[str], output: Path, label: str, timeout: int = 600) -> int:
    with (output / f'{label}.stdout').open('w') as stdout, (output / f'{label}.stderr').open('w') as stderr:
        try:
            result = subprocess.run(argv, stdout=stdout, stderr=stderr, timeout=timeout)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = 124
    with (output / 'commands.jsonl').open('a') as f:
        f.write(json.dumps({'argv': argv, 'exit_code': code, 'stdout': f'{label}.stdout',
                            'stderr': f'{label}.stderr'}) + '\n')
    return code


def environment(output: Path, executable: Path, label: str) -> None:
    metadata = {'binary_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
                'source_sha256': {str(p.relative_to(PROJECT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in sorted([*(PROJECT / 'src').glob('*'),
                                                   *(PROJECT / 'cli').glob('*')]) if p.is_file()},
                'validation_sha256': {str(p.relative_to(PROJECT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in sorted([PROJECT / 'Makefile', *(PROJECT / 'tests').rglob('*.py'),
                                                       *(PROJECT / 'tests').rglob('*.c'), *(PROJECT / 'tests').rglob('*.h'),
                                                       *(PROJECT / 'tools').glob('*.py'), *(PROJECT / 'tools').glob('*.sh')])},
                'platform': []}
    for argv in (['sw_vers'], ['uname', '-a'], ['xcrun', '--sdk', 'macosx', '--show-sdk-version']):
        run = subprocess.run(argv, text=True, capture_output=True, timeout=30)
        metadata['platform'].append({'argv': argv, 'exit_code': run.returncode,
                                         'stdout': run.stdout, 'stderr': run.stderr})
    (output / f'{label}.json').write_text(json.dumps(metadata, indent=2) + '\n')


def hashes(output: Path) -> None:
    paths = sorted(p for p in output.rglob('*') if p.is_file() and p.name != 'SHA256SUMS')
    (output / 'SHA256SUMS').write_text(''.join(
        f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(output)}\n' for p in paths))


def image_device(image: Path) -> str | None:
    result = subprocess.run(['hdiutil', 'info', '-plist'], capture_output=True, timeout=30, check=True)
    for item in plistlib.loads(result.stdout).get('images', []):
        if Path(item.get('image-path', '')).resolve() != image.resolve():
            continue
        for entity in item.get('system-entities', []):
            if entity.get('dev-entry'):
                return entity['dev-entry']
        raise RuntimeError('private image is attached without a known device; preserving it')
    return None


@contextlib.contextmanager
def apfs_fixture(output: Path, shape: str = 'mixed', files: int = 10000,
                 directories: int = 1000, seed: int = 42):
    work = Path(tempfile.mkdtemp(prefix='vantage-apfs-', dir='/tmp'))
    mount = work / 'mount'; mount.mkdir()
    image = work / 'fixture.sparseimage'
    state = {'work': str(work), 'mount': str(mount), 'image': str(image), 'cleaned': False}
    try:
        steps = [(['hdiutil', 'create', '-size', '2g', '-fs', 'APFS', '-type', 'SPARSE',
                   '-volname', 'VantageFixture', str(image)], 'image-create'),
                 (['hdiutil', 'attach', str(image), '-mountpoint', str(mount), '-nobrowse', '-owners', 'on'], 'image-attach')]
        for argv, label in steps:
            if command(argv, output, label):
                raise RuntimeError(f'{label} failed; see captured stderr')
        (mount / '.metadata_never_index').touch()
        if command([sys.executable, str(PROJECT / 'tools/fixture.py'), str(mount / 'corpus'),
                    '--shape', shape, '--files', str(files), '--directories', str(directories),
                    '--seed', str(seed)], output, 'fixture'):
            raise RuntimeError('fixture generation failed')
        if command(['hdiutil', 'detach', str(mount)], output, 'image-detach-writable'):
            raise RuntimeError('writable fixture detach failed')
        if command(['hdiutil', 'attach', str(image), '-readonly', '-mountpoint', str(mount), '-nobrowse'], output, 'image-readonly-attach'):
            raise RuntimeError('read-only fixture attach failed')
        yield mount
    finally:
        cleanup_error = None
        try:
            device = image_device(image)
            mounted = os.path.ismount(mount)
            if device or mounted:
                target = str(mount) if mounted else device
                if command(['hdiutil', 'detach', target], output, 'image-cleanup'):
                    raise RuntimeError('private image detach failed')
            if os.path.ismount(mount) or image_device(image):
                raise RuntimeError('private image is still attached after cleanup')
            shutil.rmtree(work)
            state['cleaned'] = True
        except (OSError, RuntimeError, subprocess.SubprocessError, plistlib.InvalidFileException) as error:
            cleanup_error = error
            state['cleanup_error'] = str(error)
            print(f'Private image could not be detached; retained at {work}', file=sys.stderr)
        (output / 'image-lifecycle.json').write_text(json.dumps(state, indent=2) + '\n')
        if cleanup_error:
            raise RuntimeError(f'fixture cleanup failed; evidence retained: {cleanup_error}') from cleanup_error


def main() -> int:
    signal.signal(signal.SIGTERM, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--executable', type=Path, default=PROJECT / 'build/vantage')
    parser.add_argument('--files', type=int, default=10000)
    parser.add_argument('--directories', type=int, default=1000)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('native validation requires macOS')
    args.output = args.output.resolve(); args.executable = args.executable.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    status = 0
    try:
        environment(args.output, args.executable, 'environment-before')
        with apfs_fixture(args.output, files=args.files, directories=args.directories) as mount:
            status = command([sys.executable, str(PROJECT / 'tests/native_volume.py'), str(args.executable),
                              str(mount), str(args.output / 'native')], args.output, 'native-validation')
            environment(args.output, args.executable, 'environment-after')
    except KeyboardInterrupt:
        status = 130
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        (args.output / 'failure.txt').write_text(str(error) + '\n')
        print(error, file=sys.stderr)
        status = 1
    finally:
        (args.output / 'status.json').write_text(json.dumps({'exit_code': status}) + '\n')
        hashes(args.output)
    print(f'Native validation exit {status}; evidence: {args.output}')
    return status


if __name__ == '__main__':
    sys.exit(main())
