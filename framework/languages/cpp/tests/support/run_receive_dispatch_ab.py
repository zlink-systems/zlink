#!/usr/bin/env python3
"""Compare actual receive owners in isolated DSOs, including an identical copy.

Build the receive-dispatch cost fixture with ZLINK_RECEIVE_DISPATCH_COST_DSO
and each unmodified Framework archive. Counters from every loaded DSO are
summed because the shared native allocator can resolve to the first DSO.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import statistics


class Counter(ctypes.Structure):
    _fields_ = [('tracking', ctypes.POINTER(ctypes.c_bool)),
                ('count', ctypes.POINTER(ctypes.c_size_t)),
                ('bytes', ctypes.POINTER(ctypes.c_size_t)),
                ('size_filter', ctypes.POINTER(ctypes.c_size_t)),
                ('largest', ctypes.POINTER(ctypes.c_size_t))]


def load(path):
    library = ctypes.CDLL(str(path.resolve()))
    library.zlink_receive_allocation_peer.argtypes = [ctypes.POINTER(Counter)]
    library.zlink_receive_allocation_peer.restype = None
    library.zlink_receive_dispatch_measure.argtypes = [
        ctypes.c_size_t, ctypes.c_int, ctypes.c_bool, ctypes.POINTER(Counter),
        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_double)]
    library.zlink_receive_dispatch_measure.restype = ctypes.c_int
    return library


def measure(library, peers, path, iterations, tracking):
    def snapshot():
        return [(peer.tracking[0], peer.count[0], peer.bytes[0],
                 peer.size_filter[0], peer.largest[0]) for peer in peers]

    previous = snapshot()
    count, size, elapsed = ctypes.c_size_t(), ctypes.c_size_t(), ctypes.c_double()
    result = library.zlink_receive_dispatch_measure(
        iterations, path, tracking, peers, len(peers), ctypes.byref(count),
        ctypes.byref(size), ctypes.byref(elapsed))
    if result:
        raise RuntimeError(f'receive owner failed: path={path}, exit={result}')
    if snapshot() != previous:
        raise RuntimeError('receive owner did not restore every DSO counter')
    return {'count': count.value, 'bytes': size.value,
            'ns_per_rotation': elapsed.value / iterations}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--baseline-copy', type=Path, required=True)
    parser.add_argument('--current', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--count-iterations', type=int, default=64)
    parser.add_argument('--timing-iterations', type=int, default=1000)
    args = parser.parse_args()
    if args.count_iterations < 1 or args.timing_iterations < 1:
        parser.error('iteration counts must be positive')
    paths = [args.baseline, args.baseline_copy, args.current]
    libraries = list(map(load, paths))
    unique = {}
    for library in libraries:
        counter = Counter()
        library.zlink_receive_allocation_peer(ctypes.byref(counter))
        address = ctypes.cast(counter.tracking, ctypes.c_void_p).value
        unique[address] = counter
    peers = (Counter * len(unique))(*unique.values())
    failures, counts = [], {}
    for path in (0, 2):
        values = [measure(lib, peers, path, args.count_iterations, True)
                  for lib in libraries]
        counts[path] = values
        if any(values[0][key] != values[1][key] for key in ('count', 'bytes')):
            failures.append(f'path {path}: identical-archive calibration differs')
        if values[0]['count'] - values[2]['count'] < args.count_iterations:
            failures.append(f'path {path}: reduction is less than one allocation per rotation')
        if values[2]['bytes'] >= values[0]['bytes']:
            failures.append(f'path {path}: allocated bytes did not decrease')
    injected = measure(libraries[2], peers, 3, args.count_iterations, True)
    if (injected['count'] - counts[2][2]['count'] != args.count_iterations
            or injected['bytes'] - counts[2][2]['bytes'] != 64 * args.count_iterations):
        failures.append('explicit allocation positive probe was not counted exactly')
    rounds = []
    for round_index in range(12):
        row = {}
        for index in ((0, 2) if round_index % 2 == 0 else (2, 0)):
            row[index] = measure(libraries[index], peers, 2,
                                 args.timing_iterations, False)['ns_per_rotation']
        rounds.append(row)
    medians = {index: statistics.median(row[index] for row in rounds) for index in (0, 2)}
    result = {
        'inputs': [{'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                   for path in paths],
        'count_iterations': args.count_iterations, 'counts': counts,
        'positive_probe': injected, 'timing_iterations': args.timing_iterations,
        'timing_rounds': rounds, 'timing_medians_ns': medians, 'failures': failures}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'counts': counts, 'timing_medians_ns': medians, 'failures': failures}))
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
