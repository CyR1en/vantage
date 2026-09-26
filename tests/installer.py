#!/usr/bin/env python3
"""Exercise the real shell installer with local releases and a fake HTTPS transport."""
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / 'install.sh'
RELEASE_DIR = None
if '--release-dir' in sys.argv:
    index = sys.argv.index('--release-dir')
    RELEASE_DIR = Path(sys.argv[index + 1]).resolve()
    del sys.argv[index:index + 2]

TRANSPORT = r'''
import json, os, pathlib, shutil, sys
args = sys.argv[1:]
url = args[-1]
with open(os.environ['REQUESTS'], 'a') as log:
    log.write(json.dumps(args) + '\n')
assert args[args.index('--proto') + 1] == '=https'
assert args[args.index('--proto-redir') + 1] == '=https'
assert '--fail' in args and '--location' in args
base = 'https://github.com/CyR1en/vantage/releases/'
assert url.startswith(base), url
if '--write-out' in args:
    assert url == base + 'latest', url
    print(os.environ.get('LATEST_URL', base + 'tag/v0.1.0'), end='')
else:
    assert url.startswith(base + 'download/' + os.environ.get('TAG', 'v0.1.0') + '/')
    name = url.rsplit('/', 1)[1]
    source = pathlib.Path(os.environ['ASSETS']) / name
    destination = pathlib.Path(args[args.index('--output') + 1])
    if os.environ.get('FAIL_DOWNLOAD') or not source.is_file():
        destination.write_bytes(b'partial transfer')
        sys.exit(22)
    shutil.copyfile(source, destination)
'''

PLATFORM = r'''
import os, pathlib, sys
tool = pathlib.Path(sys.argv[0]).name
if tool == 'uname':
    print(os.environ['TEST_OS'] if sys.argv[1] == '-s' else os.environ['TEST_ARCH'])
elif tool == 'sw_vers':
    print(os.environ.get('MACOS_VERSION', '11.0'))
else:
    print(os.environ.get('ROSETTA', '0'))
'''


class InstallerTests(unittest.TestCase):
    def setUp(self):
        (ROOT / 'tmp').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='installer-', dir=ROOT / 'tmp')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'tools'
        self.assets = self.root / 'assets'
        self.home = self.root / "home with ' spaces"
        self.scratch = self.root / 'scratch'
        for directory in (self.bin, self.assets, self.home, self.scratch):
            directory.mkdir()
        for tool in ('tar', 'gzip', 'mktemp', 'awk', 'cut', 'cat', 'chmod', 'cp', 'mkdir', 'mv', 'rm'):
            (self.bin / tool).symlink_to(shutil.which(tool))
        self.hash_tool = 'sha256sum' if shutil.which('sha256sum') else 'shasum'
        (self.bin / self.hash_tool).symlink_to(shutil.which(self.hash_tool))
        self.helper('curl', TRANSPORT)
        for tool in ('uname', 'sw_vers', 'sysctl'):
            self.helper(tool, PLATFORM)
        self.env = {**os.environ, 'HOME': str(self.home), 'PATH': str(self.bin),
                    'TMPDIR': str(self.scratch), 'ASSETS': str(self.assets),
                    'REQUESTS': str(self.root / 'requests'), 'TEST_OS': 'Linux',
                    'TEST_ARCH': 'x86_64'}
        self.destination = self.home / '.local/bin/vantage'

    def helper(self, name, source):
        path = self.bin / name
        path.write_text(f'#!{sys.executable}\n{source}')
        path.chmod(0o755)

    def release(self, target='linux-x86_64', version='v0.1.0', program=None, extra=None):
        if program is None:
            program = f'#!/bin/sh\nprintf "vantage {version[1:]}\\n"\n'.encode()
        path = self.assets / f'vantage-{version}-{target}.tar.gz'
        with tarfile.open(path, 'w:gz') as archive:
            members = [('vantage', program), ('LICENSE', b'fixture license')]
            if extra:
                members.append(extra)
            for name, data in members:
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                entry.mode = 0o755 if name == 'vantage' else 0o644
                archive.addfile(entry, io.BytesIO(data))
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        Path(str(path) + '.sha256').write_text(f'{digest}  {path.name}\n')
        return path

    def install(self, *args, success=True, shell='/bin/sh', script=None):
        result = subprocess.run([shell, '-s', '--', *args],
                                input=script if script is not None else INSTALLER.read_bytes(),
                                env=self.env, cwd=self.root, capture_output=True, timeout=20)
        detail = result.stdout.decode() + result.stderr.decode()
        self.assertEqual(result.returncode == 0, success, detail)
        self.assertEqual(list(self.scratch.iterdir()), [], detail)
        self.assertEqual(list(self.home.rglob('.vantage-install.*')), [], detail)
        return detail

    def requests(self):
        path = self.root / 'requests'
        return [json.loads(line)[-1] for line in path.read_text().splitlines()] if path.exists() else []

    def existing_install(self):
        self.destination.parent.mkdir(parents=True, exist_ok=True)
        self.destination.write_bytes(b'existing install')

    def test_platforms_and_architecture_aliases(self):
        for os_name, arch, target in [('Darwin', 'arm64', 'darwin-arm64'),
                                     ('Darwin', 'x86_64', 'darwin-x86_64'),
                                     ('Linux', 'aarch64', 'linux-arm64'),
                                     ('Linux', 'x86_64', 'linux-x86_64')]:
            with self.subTest(os=os_name, arch=arch):
                self.env.update(TEST_OS=os_name, TEST_ARCH=arch)
                self.release(target)
                output = self.install()
                self.assertIn('Installed vantage 0.1.0', output)
                self.assertTrue(os.access(self.destination, os.X_OK))
                self.assertIn('Add this directory', output)
                self.assertIn(f'vantage-v0.1.0-{target}.tar.gz.sha256', self.requests()[-1])

    def test_rosetta_selects_native_arm64(self):
        self.env.update(TEST_OS='Darwin', TEST_ARCH='x86_64', ROSETTA='1')
        self.release('darwin-arm64')
        self.install()
        self.assertIn('darwin-arm64', self.requests()[-1])

    def test_pinned_upgrade_and_custom_directory(self):
        self.release(version='v0.2.0-rc.1')
        self.env['TAG'] = 'v0.2.0-rc.1'
        custom = self.home / 'custom bin'
        custom.mkdir()
        (custom / 'vantage').write_text('old version')
        self.env['PATH'] += os.pathsep + str(custom)
        output = self.install('--version', 'v0.2.0-rc.1', '--install-dir', str(custom))
        self.assertNotIn('/latest', '\n'.join(self.requests()))
        self.assertIn('Run: vantage', output)
        self.assertIn('0.2.0-rc.1', (custom / 'vantage').read_text())

    def test_checksum_corruption_preserves_installation(self):
        asset = self.release()
        asset.write_bytes(asset.read_bytes() + b'corrupt')
        self.existing_install()
        self.assertIn('checksum mismatch', self.install(success=False))
        self.assertEqual(self.destination.read_bytes(), b'existing install')

    def test_missing_checksum_and_failed_transfer_preserve_installation(self):
        asset = self.release()
        Path(str(asset) + '.sha256').unlink()
        self.existing_install()
        self.assertIn('checksum', self.install(success=False))
        self.env['FAIL_DOWNLOAD'] = '1'
        self.assertIn('could not download', self.install(success=False))
        self.assertEqual(self.destination.read_bytes(), b'existing install')

    def test_invalid_or_duplicate_checksum_is_rejected(self):
        asset = self.release()
        checksum = Path(str(asset) + '.sha256')
        valid = checksum.read_text()
        for content in ('invalid', valid + valid, 'z' * 64 + f'  {asset.name}\n'):
            with self.subTest(content=content):
                checksum.write_text(content)
                self.assertIn('invalid release checksum', self.install(success=False))
                self.assertFalse(self.destination.exists())

    def test_archive_cannot_write_outside_staging(self):
        outside = self.root / 'escaped'
        self.release(extra=(str(outside), b'bad'))
        self.assertIn('unexpected release archive', self.install(success=False))
        self.assertFalse(outside.exists())
        self.assertFalse(self.destination.exists())

    def test_unrunnable_or_wrong_version_preserves_installation(self):
        self.existing_install()
        for program in (b'#!/bin/sh\nexit 1\n', b'#!/bin/sh\necho vantage 9.9.9\n'):
            with self.subTest(program=program):
                self.release(program=program)
                self.install(success=False)
                self.assertEqual(self.destination.read_bytes(), b'existing install')

    def test_replaces_symlink_without_modifying_its_target(self):
        self.release()
        original = self.root / 'other-binary'
        original.write_text('untouched')
        self.destination.parent.mkdir(parents=True)
        self.destination.symlink_to(original)
        self.install()
        self.assertFalse(self.destination.is_symlink())
        self.assertEqual(original.read_text(), 'untouched')

    def test_directory_destination_is_rejected(self):
        self.release()
        self.destination.mkdir(parents=True)
        self.assertIn('is a directory', self.install(success=False))
        self.assertEqual(list(self.destination.iterdir()), [])

    def test_unsupported_platforms_and_old_macos_fail_before_download(self):
        for os_name, arch, version in [('FreeBSD', 'x86_64', '11.0'),
                                       ('Linux', 'armv7l', '11.0'),
                                       ('Darwin', 'x86_64', '10.15.7')]:
            with self.subTest(os=os_name, arch=arch):
                self.env.update(TEST_OS=os_name, TEST_ARCH=arch, MACOS_VERSION=version)
                self.install(success=False)
                self.assertEqual(self.requests(), [])

    def test_bad_arguments_fail_before_download(self):
        for args in [('--wat',), ('--version',), ('--install-dir', ''),
                     ('--version', 'v1/../../bad'), ('--version', '1.0.0')]:
            with self.subTest(args=args):
                self.install(*args, success=False)
                self.assertEqual(self.requests(), [])

    def test_unexpected_latest_redirect_is_rejected(self):
        self.env['LATEST_URL'] = 'https://example.com/tag/v0.1.0'
        self.assertIn('unexpected release URL', self.install(success=False))
        self.assertEqual(len(self.requests()), 1)

    def test_shasum_fallback(self):
        if not shutil.which('shasum'):
            self.skipTest('shasum is not installed')
        (self.bin / self.hash_tool).unlink()
        (self.bin / 'shasum').symlink_to(shutil.which('shasum'))
        self.release()
        self.install()

    def test_help_and_truncated_stream_make_no_changes(self):
        self.assertIn('Usage:', self.install('--help'))
        truncated = INSTALLER.read_bytes().split(b'    asset=vantage-')[0]
        self.install(script=truncated, success=False)
        self.assertEqual(self.requests(), [])
        self.assertFalse(self.destination.exists())

    def test_bash_piped_install(self):
        self.release()
        self.install(shell='/bin/bash')

    @unittest.skipUnless(RELEASE_DIR, 'use --release-dir to test a native release archive')
    def test_native_release_archive(self):
        os_name = 'darwin' if sys.platform == 'darwin' else 'linux'
        arch = 'arm64' if platform.machine() in ('arm64', 'aarch64') else 'x86_64'
        assets = list(RELEASE_DIR.glob(f'vantage-*-{os_name}-{arch}.tar.gz'))
        self.assertEqual(len(assets), 1, assets)
        asset = assets[0]
        tag = re.fullmatch(rf'vantage-(v.+)-{os_name}-{arch}\.tar\.gz', asset.name)[1]
        shutil.copyfile(asset, self.assets / asset.name)
        checksum = Path(str(asset) + '.sha256')
        shutil.copyfile(checksum, self.assets / checksum.name)
        self.env.update(TEST_OS=platform.system(), TEST_ARCH=platform.machine(), TAG=tag)
        self.install('--version', tag)
        result = subprocess.run([self.destination, '--version'], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), f'vantage {tag[1:]}')


if __name__ == '__main__':
    unittest.main()
