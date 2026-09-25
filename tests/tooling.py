#!/usr/bin/env python3
"""Fault-injection checks for native evidence collection; no disk images created."""
import contextlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import macos_validation as validation
import native_volume


class EvidenceTests(unittest.TestCase):
    def test_failed_detach_fails_validation_and_preserves_work(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'out'
            argv = ['validation', str(out), '--executable', str(Path(temp) / 'scanner'), '--validate-only']
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

    def test_probe_failure_is_not_overwritten_by_passing_matrix(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'out'
            @contextlib.contextmanager
            def fixture(*args, **kwargs):
                yield Path(temp) / 'mount'
            argv = ['validation', str(out), '--executable', str(Path(temp) / 'scanner'), '--validate-only']
            with patch.object(sys, 'argv', argv), patch.object(sys, 'platform', 'darwin'), \
                 patch.object(validation, 'environment'), patch.object(validation, 'apfs_fixture', fixture), \
                 patch.object(validation, 'command', side_effect=[1, 0]), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(validation.main(), 1)

    def test_timeout_and_malformed_json_retain_evidence_and_continue(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'out'
            argv = ['native', '/unused/scanner', '/unused/fixture', str(out)]
            calls = 0
            def run(args, **kwargs):
                nonlocal calls
                calls += 1
                if calls == 1:
                    raise subprocess.TimeoutExpired(args, 360, output=b'partial stdout', stderr=b'partial stderr')
                manifest = Path(args[args.index('--manifest-json') + 1]); manifest.write_text('')
                if calls == 2:
                    return subprocess.CompletedProcess(args, 0, '{"completion":"complete"}', '')
                scan = {'completion': 'complete', 'entries': 0, 'bytes': 0, 'unique_bytes': 0,
                        'setup_ns': 1, 'scan_ns': 1, 'finalize_ns': 1, 'total_ns': 3, 'graph_valid': True}
                return subprocess.CompletedProcess(args, 0, json.dumps(scan), '')
            with patch.object(sys, 'argv', argv), patch.object(native_volume, 'oracle', return_value=[]), \
                 patch.object(native_volume.subprocess, 'run', run), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(native_volume.main(), 1)
            rows = json.loads((out / 'summary.json').read_text())
            self.assertEqual(len(rows), 34)
            self.assertEqual([r['status'] for r in rows[:2]], ['failed', 'failed'])
            self.assertTrue(all(r['status'] == 'passed' for r in rows[2:]))
            self.assertEqual((out / '00-posix-tree-logical-result.jsonl').read_text(), 'partial stdout')
            self.assertIn('partial stderr', (out / '00-posix-tree-logical.log').read_text())


if __name__ == '__main__':
    unittest.main()
