#!/usr/bin/env python3
"""Compare the public request fixture in two locally bound Framework DSOs.

Build both libraries from the same test_cpp_framework_request_observer_lifetime.cpp
with ZLINK_REQUEST_OBSERVER_DSO, their exact PIC Framework archive and -Wl,-Bsymbolic.
The fixture objects and every linked static dependency must also support PIC.
The baseline archive must be built from the unmodified --baseline-ref source.
No product source rewriting or machine-specific allocation threshold is used.
A candidate must reduce at least one byte (or one allocation in count mode) per
submission. Comparing unchanged HEAD artifacts with each other is deliberately RED.
The timing pass reports submission elapsed nanoseconds, separately from counting.
"""

import argparse
import ctypes
import hashlib
import json
import pathlib
import statistics
import subprocess
import sys


def load(path):
    library = ctypes.CDLL(str(path.resolve()), mode=ctypes.RTLD_LOCAL)
    entry = library.zlink_request_observer_measure
    entry.argtypes = [ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_uint64),
                      ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    entry.restype = ctypes.c_int
    return library, entry


def measure(entry, iterations, ready, track):
    allocations = ctypes.c_size_t()
    elapsed = ctypes.c_uint64()
    allocated_bytes = ctypes.c_size_t()
    largest = ctypes.c_size_t()
    status = entry(iterations, int(ready), int(track),
                   ctypes.byref(allocations), ctypes.byref(elapsed),
                   ctypes.byref(allocated_bytes), ctypes.byref(largest))
    if status:
        raise RuntimeError(f'public fixture failed with status {status}')
    return {'allocations': allocations.value, 'bytes': allocated_bytes.value,
            'largest': largest.value, 'ns_per_submit': elapsed.value / iterations}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', required=True)
    parser.add_argument('--baseline-provenance', required=True, type=pathlib.Path,
                        help='JSON with source_ref, source_sha256, archive, archive_sha256, '
                             'dso_sha256, and compile_flags from the baseline build')
    parser.add_argument('--baseline', required=True, type=pathlib.Path)
    parser.add_argument('--current', required=True, type=pathlib.Path)
    parser.add_argument('--metric', choices=['bytes', 'count'], default='bytes')
    parser.add_argument('--iterations', type=int, default=1000)
    parser.add_argument('--rounds', type=int, default=12)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    args = parser.parse_args()
    if args.iterations <= 0 or args.rounds <= 0:
        parser.error('iterations and rounds must be positive')
    repository = pathlib.Path(__file__).resolve().parents[5]
    original = subprocess.check_output(
        ['git', 'show', f'{args.baseline_ref}:framework/languages/cpp/framework/src/'
                       'runtime/mesh/raw_mesh_node_owner.cpp'], cwd=repository)
    source_ref = subprocess.check_output(
        ['git', 'rev-parse', args.baseline_ref], cwd=repository, text=True).strip()
    provenance = json.loads(args.baseline_provenance.read_text())
    expected = {
        'source_ref': source_ref,
        'source_sha256': hashlib.sha256(original).hexdigest(),
        'dso_sha256': hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
    }
    for key, value in expected.items():
        if provenance.get(key) != value:
            parser.error(f'baseline provenance mismatch: {key}')
    archive = pathlib.Path(provenance['archive'])
    if hashlib.sha256(archive.read_bytes()).hexdigest() != provenance['archive_sha256']:
        parser.error('baseline archive hash differs from build provenance')
    if not provenance.get('compile_flags'):
        parser.error('baseline provenance has no compile flags')
    libraries = {label: load(path) for label, path in
                 [('baseline', args.baseline), ('current', args.current)]}
    trackers = {}
    for label, (library, _) in libraries.items():
        getter = library.zlink_request_observer_get_tracker
        getter.argtypes = [ctypes.POINTER(ctypes.c_void_p)] * 5
        getter.restype = None
        values = [ctypes.c_void_p() for _ in range(5)]
        getter(*(ctypes.byref(value) for value in values))
        trackers[label] = [value.value for value in values]
    if trackers['baseline'] != trackers['current']:
        for label, peer in [('baseline', 'current'), ('current', 'baseline')]:
            setter = libraries[label][0].zlink_request_observer_set_peer_tracker
            setter.argtypes = [ctypes.c_void_p] * 5
            setter.restype = None
            setter(*trackers[peer])
    result = {
        'baseline_ref': source_ref,
        'baseline_provenance': provenance,
        'baseline_tu_sha256': hashlib.sha256(original).hexdigest(),
        'iterations': args.iterations, 'rounds': args.rounds, 'metric': args.metric,
        'isolation': 'RTLD_LOCAL/Bsymbolic; aggregate both fixture allocator counters',
        'libraries': {label: {'path': str(path.resolve()),
                             'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                      for label, path in [('baseline', args.baseline), ('current', args.current)]},
        'cases': {},
    }
    failures = []
    for case, ready in [('pending', False), ('ready-rejection', True)]:
        # Warm both implementations before counting, and keep measurement modes separate.
        for label in ['baseline', 'current']:
            measure(libraries[label][1], 8, ready, False)
        counts = {label: measure(libraries[label][1], args.iterations, ready, True)
                  for label in ['baseline', 'current']}
        samples = {'baseline': [], 'current': []}
        for round_index in range(args.rounds):
            order = ['baseline', 'current'] if round_index % 2 == 0 else ['current', 'baseline']
            for label in order:
                samples[label].append(measure(libraries[label][1], args.iterations,
                                              ready, False)['ns_per_submit'])
        medians = {label: statistics.median(values) for label, values in samples.items()}
        allocation_reduction = counts['baseline']['allocations'] - counts['current']['allocations']
        byte_reduction = counts['baseline']['bytes'] - counts['current']['bytes']
        reduction = allocation_reduction if args.metric == 'count' else byte_reduction
        result['cases'][case] = {
            'count_pass': counts, 'allocation_reduction': allocation_reduction,
            'byte_reduction': byte_reduction,
            'required_reduction': args.iterations, 'samples_ns_per_submit': samples,
            'median_ns_per_submit': medians,
            'time_change_percent': 100 * (medians['current'] / medians['baseline'] - 1),
        }
        if reduction < args.iterations:
            failures.append(f'{case}: {args.metric} reduction {reduction} < {args.iterations}')
    result['focused_status'] = {}
    for label, (library, _) in libraries.items():
        focused = library.zlink_request_observer_verify
        focused.argtypes = []
        focused.restype = ctypes.c_int
        result['focused_status'][label] = focused()
        if result['focused_status'][label]:
            failures.append(f'{label} fixture lifetime verification failed')
    result['failures'] = failures
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
