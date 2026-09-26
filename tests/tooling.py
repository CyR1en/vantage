#!/usr/bin/env python3
"""Fault-injection checks for native evidence collection; no disk images created."""
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import macos_validation as validation
import native_volume
from oracle import oracle


def export_bytes(root=b'/fixture', size=0, entries=()):
    def text(value):
        return struct.pack('<I', len(value)) + value
    header = b'SVX1' + struct.pack('<II8Q', 0, size, 1, 0, 0, 0, 0, 0, 0, 0)
    header += text(root) + text(b'') + text(b'posix') + struct.pack('<Q', len(entries))
    return header + b''.join(struct.pack('<QBQQ', parent, kind, length, unknown) + text(name)
                             for parent, kind, length, unknown, name in entries)


def empty_oracle():
    return {'entries': [], 'total_bytes': 0, 'unique_bytes': 0, 'files': 0,
            'directories': 0, 'unknown_sizes': 0}


class EvidenceTests(unittest.TestCase):
    def test_failed_detach_fails_validation_and_preserves_work(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'out'
            argv = ['validation', str(out), '--executable', str(Path(temp) / 'vantage')]
            def command(args, output, label, timeout=600):
                return 1 if label == 'image-cleanup' else 0
            with patch.object(sys, 'argv', argv), patch.object(sys, 'platform', 'darwin'), \
                 patch.object(validation, 'command', command), patch.object(validation, 'environment'), \
                 patch.object(validation, 'image_device', return_value='/dev/mock-private-image'), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(validation.main(), 1)
            state = json.loads((out / 'image-lifecycle.json').read_text())
            self.assertFalse(state['cleaned'])
            self.assertEqual(json.loads((out / 'status.json').read_text())['exit_code'], 1)
            self.assertTrue(Path(state['work']).exists())
            self.assertTrue((out / 'SHA256SUMS').exists())
            shutil.rmtree(state['work'])

    def test_interrupted_attach_still_detaches_discovered_device(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            calls = []
            def command(args, output, label, timeout=600):
                calls.append((args, label))
                return 124 if label == 'image-attach' else 0
            with patch.object(validation, 'command', command), \
                 patch.object(validation, 'image_device', side_effect=['/dev/mock-private-image', None]):
                with self.assertRaisesRegex(RuntimeError, 'image-attach failed'):
                    with validation.apfs_fixture(out):
                        self.fail('an interrupted attach must not reach validation')
            self.assertIn((['hdiutil', 'detach', '/dev/mock-private-image'], 'image-cleanup'), calls)
            state = json.loads((out / 'image-lifecycle.json').read_text())
            self.assertTrue(state['cleaned'])
            self.assertFalse(Path(state['work']).exists())

    def test_validation_status_is_preserved_after_environment_capture(self):
        for status in (0, 1, 124):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as temp:
                out = Path(temp) / 'out'
                @contextlib.contextmanager
                def fixture(*args, **kwargs):
                    yield Path(temp) / 'mount'
                argv = ['validation', str(out), '--executable', str(Path(temp) / 'vantage')]
                with patch.object(sys, 'argv', argv), patch.object(sys, 'platform', 'darwin'), \
                     patch.object(validation, 'environment') as environment, \
                     patch.object(validation, 'apfs_fixture', fixture), \
                     patch.object(validation, 'command', return_value=status) as command, \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(validation.main(), status)
                self.assertEqual(command.call_count, 1)
                self.assertEqual(command.call_args.args[2], 'native-validation')
                self.assertEqual(environment.call_count, 2)
                self.assertEqual(json.loads((out / 'status.json').read_text())['exit_code'], status)

    def test_timeout_and_malformed_outputs_retain_evidence_and_continue(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'out'
            argv = ['native', '/unused/vantage', '/unused/fixture', str(out)]
            calls = 0
            def run(args, **kwargs):
                nonlocal calls
                calls += 1
                if calls == 1:
                    raise subprocess.TimeoutExpired(args, 360, output=b'partial stdout', stderr=b'partial stderr')
                if calls == 4:
                    return subprocess.CompletedProcess(args, 0, b'{broken', b'')
                if calls == 5:
                    return subprocess.CompletedProcess(args, 0, b'SVX1', b'')
                size = args[args.index('--size') + 1]
                if '--export' in args:
                    output = export_bytes(os.fsencode(args[1]), ('logical', 'allocated').index(size))
                else:
                    scan = {**empty_oracle(), 'completion': 'complete', 'entries': 0,
                            'graph_valid': True, 'size': size, 'root_hex': os.fsencode(args[1]).hex()}
                    output = json.dumps(scan).encode()
                return subprocess.CompletedProcess(args, 0, output, b'')
            with patch.object(sys, 'argv', argv), patch.object(native_volume, 'oracle', return_value=empty_oracle()), \
                 patch.object(native_volume.subprocess, 'run', run), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(native_volume.main(), 1)
            rows = json.loads((out / 'summary.json').read_text())
            self.assertEqual(len(rows), 48)
            self.assertEqual(calls, 96)
            self.assertEqual([row['status'] for row in rows[:3]], ['failed'] * 3)
            self.assertTrue(all(row['status'] == 'passed' for row in rows[3:]))
            self.assertEqual((out / '00-auto-j1-logical-volume-export.svx').read_bytes(), b'partial stdout')
            self.assertIn(b'partial stderr', (out / '00-auto-j1-logical-volume-export.stderr').read_bytes())

    def test_mismatched_export_writes_exact_path_differences(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'out'
            root = Path(temp).resolve()
            expected = empty_oracle()
            expected['entries'] = [{'path_hex': b'expected'.hex(), 'kind': 1, 'bytes': 10, 'unknown_sizes': 0}]
            def run(args, **kwargs):
                if '--export' in args:
                    output = export_bytes(os.fsencode(root), entries=[(1, 1, 10, 0, b'wrong')])
                else:
                    output = json.dumps({'completion': 'complete', 'size': 'logical',
                                         'root_hex': os.fsencode(root).hex()}).encode()
                return subprocess.CompletedProcess(args, 0, output, b'')
            argv = ['native', '/unused/vantage', str(root), str(out)]
            cases = [('posix', 1, 'logical', root, 'volume', [])]
            with patch.object(sys, 'argv', argv), patch.object(native_volume, 'oracle', return_value=expected), \
                 patch.object(native_volume, 'validation_cases', return_value=cases), \
                 patch.object(native_volume.subprocess, 'run', run), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(native_volume.main(), 1)
            differences = json.loads((out / '00-posix-j1-logical-volume-differences.json').read_text())
            self.assertEqual({row['path_hex'] for row in differences}, {b'expected'.hex(), b'wrong'.hex()})


class ExportOracleTests(unittest.TestCase):
    def test_decoder_resolves_forward_parents_and_raw_names(self):
        entries = [(2, 1, 7, 0, b'raw\xff'), (3, 5, 0, 0, b'link'), (3, 2, 7, 0, b'folder')]
        decoded = native_volume.decode_export(export_bytes(entries=entries))
        self.assertEqual(decoded['entries'], [
            {'path_hex': b'folder'.hex(), 'kind': 2, 'bytes': 7, 'unknown_sizes': 0},
            {'path_hex': b'folder/raw\xff'.hex(), 'kind': 1, 'bytes': 7, 'unknown_sizes': 0},
            {'path_hex': b'link'.hex(), 'kind': 5, 'bytes': 0, 'unknown_sizes': 0},
        ])

    def test_decoder_rejects_invalid_tree_streams(self):
        cases = [
            export_bytes()[:-1], export_bytes() + b'extra',
            export_bytes(entries=[(0, 2, 0, 0, b'cycle')]),
            export_bytes(entries=[(1, 1, 0, 0, b'child'), (2, 1, 0, 0, b'file-parent')]),
            export_bytes(entries=[(2, 1, 0, 0, b'bad-parent')]),
            export_bytes(entries=[(1, 1, 0, 0, b'a/b')]),
            export_bytes(entries=[(1, 1, 0, 0, b'')]),
            export_bytes(entries=[(2, 1, 0, 0, b'same'), (2, 1, 0, 0, b'same')]),
        ]
        for stream in cases:
            with self.subTest(stream=stream), self.assertRaises(ValueError):
                native_volume.decode_export(stream)

    def test_filesystem_oracle_counts_hardlink_names_without_following_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'folder').mkdir()
            (root / 'source').write_bytes(b'1234567')
            os.link(root / 'source', root / 'folder/alias')
            os.symlink('..', root / 'folder/loop')
            os.symlink('/tmp', root / 'outside')
            os.mkfifo(root / 'fifo')
            result = oracle(root)
            entries = {bytes.fromhex(row['path_hex']): row for row in result['entries']}
            self.assertEqual(set(entries), {b'folder', b'source', b'folder/alias', b'folder/loop', b'outside', b'fifo'})
            self.assertEqual((result['total_bytes'], result['unique_bytes'], result['files'], result['directories']),
                             (14, 7, 2, 1))
            self.assertEqual(entries[b'folder']['bytes'], 7)
            self.assertEqual(entries[b'outside']['bytes'], 0)
            self.assertEqual(entries[b'folder/loop']['kind'], 5)

    def test_filesystem_oracle_excludes_other_devices(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            entry = Mock(name=b'mounted-volume')
            entry.stat.return_value.st_dev = os.lstat(root).st_dev + 1
            with patch('oracle.os.scandir', return_value=contextlib.nullcontext([entry])):
                self.assertEqual(oracle(root), empty_oracle())
            entry.stat.assert_called_once_with(follow_symlinks=False)


if __name__ == '__main__':
    unittest.main()
