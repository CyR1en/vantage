#!/usr/bin/env python3
"""Run a frozen warm-cache comparison on private APFS corpora and retain evidence."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys

from macos_validation import PROJECT, apfs_fixture, command, environment, hashes

CONFIGURATIONS = {
    ('posix', 1, 0), ('posix-par', 2, 0), ('posix-par', 4, 0),
    ('bulk-par', 2, 65536), ('bulk-par', 4, 65536),
}
CANDIDATE = ('bulk-par', 2, 65536)
BASELINES = {'serial_posix': ('posix', 1, 0), 'parallel_posix_w4': ('posix-par', 4, 0)}
LIMITATION = ('Seeded sessions on one host and one fixture per corpus, collected consecutively; '
              'not independent machines or days. Per-session bootstrap intervals describe '
              'within-session paired ratios. Corpus summaries use session estimates, with no '
              'pooled-trial or between-session confidence interval. Power, thermal and '
              'background conditions are recorded where available, not controlled.')


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ValueError(reason)


def quantile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    point = (len(ordered) - 1) * q
    index = int(point)
    return (ordered[index] if index + 1 == len(ordered) else
            ordered[index] + (ordered[index + 1] - ordered[index]) * (point - index))


def paired(candidate: list[dict], baseline: list[dict], metric: str) -> dict:
    by_round = {row['round']: row for row in baseline}
    ratios = [by_round[row['round']][metric] / row[metric] for row in candidate]
    # Match the report's published deterministic percentile-bootstrap procedure.
    state, mask = 0x6D31535BC4997E81, (1 << 64) - 1
    estimates = []
    for _ in range(2000):
        sample = []
        for _ in ratios:
            state = (state + 0x9E3779B97F4A7C15) & mask
            value = state
            value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & mask
            value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & mask
            value ^= value >> 31
            sample.append(ratios[value % len(ratios)])
        estimates.append(statistics.median(sample))
    return {'median': statistics.median(ratios),
            'lower_95': quantile(estimates, .025), 'upper_95': quantile(estimates, .975),
            'pairs': len(ratios), 'bootstrap_resamples': 2000,
            'bootstrap_seed': '0x6d31535bc4997e81'}


def summarize_session(path: Path, rounds: int, seed: int) -> dict:
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    require(Counter(row['event'] for row in rows) ==
            Counter({'reference': 1, 'verification': 5, 'warmup': 5, 'trial': 5 * rounds}),
            'missing or unexpected reference, preflight, warmup or measured records')
    require(len({row['session'] for row in rows}) == 1, 'session identity changed within one run')
    require(len({row['group'] for row in rows}) == 1, 'environment/contract group changed within one run')
    require(len({row['digest'] for row in rows}) == 1, 'inventory digest differs from the reference')
    for row in rows:
        require(row['completion'] == 'complete', f"{row['event']} failed: {row['reason']}")
        require(row['event'] == 'reference' or row['verification'] == 'verified',
                f"{row['event']} is {row['verification']}: {row['reason']}")
        if row['event'] == 'verification':
            require(row['verification_basis'] == 'exact-posix-manifest', 'preflight lacks exact manifest verification')
        elif row['event'] in ('warmup', 'trial'):
            require(row['verification_basis'] == 'exact-preflight+trial-digest', 'timed run lacks exact preflight and digest verification')
        require(row['readonly_mount'] and row['filesystem'] == 'apfs', 'fixture is not read-only APFS')
        require(row['task'] == 'tree' and row['size_contract'] == 'regular-data-fork-logical',
                'unexpected task/size contract')
        require(row['cache'] == 'warm' and row['consistency'] == 'immutable', 'unexpected cache/consistency contract')
        require(row['graph_valid'] and row['unknown_sizes'] == 0, 'invalid graph or unknown sizes')
        require(str(row['seed']) == str(seed), 'unexpected measurement shuffle seed')
        require(row['total_ns'] > 0 and row['process_wall_ns'] > 0, 'nonpositive scan/process timing')
        require(row['setup_ns'] + row['scan_ns'] + row['finalize_ns'] == row['total_ns'],
                'timing phases do not sum to the total')
    grouped = defaultdict(list)
    trials = [row for row in rows if row['event'] == 'trial']
    for row in trials:
        grouped[(row['method'], row['workers'], row['buffer_bytes'])].append(row)
    require(set(grouped) == CONFIGURATIONS, 'measured configurations differ from the frozen comparison')
    for key, sample in grouped.items():
        require(len(sample) == rounds and {row['round'] for row in sample} == set(range(rounds)),
                f'missing or duplicated measured rounds for {key}')
        require(len({row['config'] for row in sample}) == 1, f'configuration changed within {key}')
    for event in ('verification', 'warmup'):
        require(Counter((row['method'], row['workers'], row['buffer_bytes']) for row in rows
                        if row['event'] == event) == Counter(CONFIGURATIONS),
                f'{event} did not cover each frozen configuration exactly once')
    configurations = []
    for key, sample in sorted(grouped.items()):
        times = [row['total_ns'] / 1e6 for row in sample]
        configurations.append({'method': key[0], 'workers': key[1], 'buffer_bytes': key[2],
                               'config': sample[0]['config'], 'rounds': len(sample),
                               'median_ms': statistics.median(times),
                               'iqr_ms': [quantile(times, .25), quantile(times, .75)],
                               'median_process_wall_ms': statistics.median(row['process_wall_ns'] for row in sample) / 1e6,
                               'median_process_cpu_ms': statistics.median(row['user_ns'] + row['system_ns'] for row in sample) / 1e6,
                               'median_rss_mib': statistics.median(row['max_rss_bytes'] for row in sample) / (1024 * 1024)})
    return {'status': 'verified', 'session': rows[0]['session'], 'group': rows[0]['group'],
            'digest': rows[0]['digest'], 'build': rows[0]['build'], 'shuffle_seed': seed,
            'rounds': rounds, 'small_sample_interval': rounds < 15, 'verified_measured_trials': len(trials),
            'first_to_last_measured_start_seconds': (max(row['timestamp_ns'] for row in trials)
                                                    - min(row['timestamp_ns'] for row in trials)) / 1e9,
            'configurations': configurations,
            'frozen_candidate_comparisons': {
                name: {metric: paired(grouped[CANDIDATE], grouped[key], metric)
                       for metric in ('total_ns', 'process_wall_ns')}
                for name, key in BASELINES.items()}, 'interpretation_limit': LIMITATION}


def summarize_corpus(sessions: list[dict]) -> dict:
    verified = [session for session in sessions if session['status'] == 'verified']
    require(len({session['session'] for session in verified}) == len(verified), 'reused benchmark session identity')
    for key in ('group', 'digest', 'build'):
        require(len({session[key] for session in verified}) <= 1, f'{key} changed between corpus sessions')
    comparisons = {}
    for baseline in BASELINES:
        comparisons[baseline] = {}
        for metric in ('total_ns', 'process_wall_ns'):
            estimates = [session['frozen_candidate_comparisons'][baseline][metric]['median'] for session in verified]
            comparisons[baseline][metric] = {'session_estimates': estimates,
                'median_of_session_estimates': statistics.median(estimates) if estimates else None,
                'range_of_session_estimates': [min(estimates), max(estimates)] if estimates else None}
    return {'verified_sessions': len(verified), 'failed_sessions': len(sessions) - len(verified),
            'frozen_candidate_comparisons': comparisons, 'interpretation_limit': LIMITATION}


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2) + '\n')


def capture_environment(output: Path, executable: Path, label: str, frozen: dict) -> None:
    environment(output, executable, label)
    current = json.loads((output / f'{label}.json').read_text())
    for key in ('binary_sha256', 'source_sha256'):
        require(current[key] == frozen[key], f'{key} changed during the study')


def run_session(output: Path, executable: Path, mount: Path, rounds: int, seed: int, frozen: dict) -> dict:
    output.mkdir()
    capture_environment(output, executable, 'environment-before', frozen)
    result_path = output / 'results.jsonl'
    code = command([str(executable), 'bench', str(mount), '--methods', 'posix,posix-par,bulk-par',
                    '--workers', '2,4', '--buffer', '64KiB', '--rounds', str(rounds), '--warmup', '1',
                    '--seed', str(seed), '--cache', 'warm', '--verify', 'exact',
                    '--consistency', 'immutable', '--out', str(result_path)],
                   output, 'benchmark', timeout=1800)
    report_code = (command([str(executable), 'report', str(result_path)], output, 'report')
                   if result_path.exists() else None)
    try:
        capture_environment(output, executable, 'environment-after', frozen)
        require(code == 0, f'benchmark exited {code}; see benchmark stderr and raw records')
        require(report_code == 0, f'report exited {report_code}; see report stderr')
        summary = summarize_session(result_path, rounds, seed)
    except (OSError, ValueError, KeyError, TypeError) as error:
        summary = {'status': 'failed', 'shuffle_seed': seed, 'error': str(error)}
    summary.update({'benchmark_exit_code': code, 'report_exit_code': report_code})
    write_json(output / 'summary.json', summary)
    hashes(output)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--executable', type=Path, default=PROJECT / 'build/scanbench')
    parser.add_argument('--sessions', type=int, default=5)
    parser.add_argument('--rounds', type=int, default=30)
    parser.add_argument('--quick', action='store_true', help='smaller corpora for harness validation; not the confirmation study')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('the private APFS study requires macOS')
    if not 1 <= args.sessions <= 100 or not 3 <= args.rounds <= 10000:
        parser.error('sessions must be 1..100 and rounds 3..10000')
    args.output = args.output.resolve(); args.executable = args.executable.resolve()
    if not args.executable.is_file():
        parser.error('executable does not exist; build scanbench first')
    if args.output.exists():
        parser.error('output directory must not exist')
    args.output.mkdir(parents=True)
    corpora = [('wide', 'wide', 10000, 2000, 301), ('deep', 'deep', 10000, 100, 302),
               ('mixed', 'mixed', 10000, 1000, 303), ('large', 'mixed', 50000, 5000, 304)]
    if args.quick:
        corpora = [(name, shape, files // 10, max(10, directories // 10), seed)
                   for name, shape, files, directories, seed in corpora]
    study = {'status': 'running', 'quick_validation': args.quick, 'sessions_per_corpus': args.sessions,
             'rounds_per_session': args.rounds, 'frozen_candidate': list(CANDIDATE),
             'expected_measured_trials': len(corpora) * args.sessions * args.rounds * len(CONFIGURATIONS),
             'python_version': sys.version,
             'tool_sha256': {name: hashlib.sha256((PROJECT / 'tools' / name).read_bytes()).hexdigest()
                             for name in ('macos_benchmark_study.py', 'macos_validation.py', 'fixture.py')},
             'corpora': [], 'interpretation_limit': LIMITATION}
    write_json(args.output / 'summary.json', study)
    status = 0; completed = False
    try:
        environment(args.output, args.executable, 'environment-before')
        frozen = json.loads((args.output / 'environment-before.json').read_text())
        for name, shape, files, directories, fixture_seed in corpora:
            output = args.output / name; output.mkdir()
            corpus = {'name': name, 'shape': shape, 'files': files, 'directories': directories,
                      'fixture_seed': fixture_seed, 'sessions': []}
            study['corpora'].append(corpus)
            with apfs_fixture(output, shape=shape, files=files, directories=directories, seed=fixture_seed) as mount:
                for session in range(args.sessions):
                    seed = 1001 + session
                    print(f'{name}: session {session + 1}/{args.sessions}, {args.rounds} measured blocks, seed {seed}', flush=True)
                    result = run_session(output / f'session-{session + 1:02d}', args.executable,
                                         mount, args.rounds, seed, frozen)
                    corpus['sessions'].append(result)
                    if result['status'] != 'verified':
                        status = 1; print(f"{name}: {result['error']}", file=sys.stderr, flush=True)
                    write_json(args.output / 'summary.json', study)
            corpus['aggregate'] = summarize_corpus(corpus['sessions'])
            lifecycle = json.loads((output / 'image-lifecycle.json').read_text())
            require(lifecycle['cleaned'], f'private {name} image remains attached; see image-lifecycle.json')
            write_json(output / 'summary.json', corpus); hashes(output)
            write_json(args.output / 'summary.json', study)
        capture_environment(args.output, args.executable, 'environment-after', frozen)
        completed = True
    except KeyboardInterrupt:
        status = 130; study['error'] = 'study interrupted before completion'
    except (RuntimeError, OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        status = 1; study['error'] = str(error)
        (args.output / 'failure.txt').write_text(str(error) + '\n')
        print(error, file=sys.stderr)
    finally:
        if not completed and status == 0:
            status = 1
        study['status'] = 'verified' if status == 0 else 'failed'
        study['exit_code'] = status
        verified = [session for corpus in study['corpora'] for session in corpus['sessions']
                    if session['status'] == 'verified']
        study['verified_sessions'] = len(verified)
        study['verified_measured_trials'] = sum(session['verified_measured_trials'] for session in verified)
        write_json(args.output / 'summary.json', study); hashes(args.output)
    print(f'Warm study exit {status}; evidence: {args.output}')
    return status


if __name__ == '__main__':
    sys.exit(main())
