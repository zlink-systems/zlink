#!/usr/bin/env python3
"""Alternate isolated copies of the real mailbox owner in one process."""
import argparse
import ctypes
import hashlib
import json
import math
from pathlib import Path
import random
import statistics


def load(path):
    library = ctypes.CDLL(str(path.resolve()))
    operation = library.zlink_mailbox_fifo_measure
    operation.argtypes = [
        ctypes.c_size_t, ctypes.c_size_t, ctypes.c_bool,
        ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_double)]
    operation.restype = ctypes.c_int
    return operation


def measure(library, iterations, batch, tracking):
    count, size, elapsed = ctypes.c_size_t(), ctypes.c_size_t(), ctypes.c_double()
    if library(iterations, batch, tracking, ctypes.byref(count), ctypes.byref(size),
               ctypes.byref(elapsed)):
        raise RuntimeError('mailbox owner failed')
    return {'allocations': count.value / iterations, 'bytes': size.value / iterations,
            'ns_per_record': elapsed.value / (iterations * batch)}


def interval(values):
    # Paired bootstrap of the geometric mean speedup; preserve each old/new pair.
    logs = [math.log(value) for value in values]
    rng = random.Random(1672)
    samples = sorted(math.exp(statistics.mean(rng.choices(logs, k=len(logs))))
                     for _ in range(10000))
    return [samples[249], samples[9749]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', action='append', required=True,
                        help='name=DSO path; first entry is the baseline')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pairs', type=int, default=32)
    args = parser.parse_args()
    if args.pairs < 16:
        parser.error('at least 16 pairs are required')
    paths = dict(value.split('=', 1) for value in args.variant)
    libraries = {name: load(Path(path)) for name, path in paths.items()}
    names = list(paths)
    result = {'inputs': {name: {'path': path, 'sha256': hashlib.sha256(
        Path(path).read_bytes()).hexdigest()} for name, path in paths.items()}, 'batches': {}}
    for batch in (1, 128):
        counts = {name: measure(lib, 64, batch, True) for name, lib in libraries.items()}
        rows = []
        iterations = 10000 if batch == 1 else 1000
        for index in range(args.pairs):
            order = names if index % 2 == 0 else list(reversed(names))
            rows.append({name: measure(libraries[name], iterations, batch, False)
                         ['ns_per_record'] for name in order})
        comparisons = {}
        for old, new in ((a, b) for i, a in enumerate(names) for b in names[i + 1:]):
            ratios = [row[old] / row[new] for row in rows]
            comparisons[f'{old}/{new}'] = {
                'speedup': math.exp(statistics.mean(map(math.log, ratios))),
                'paired_bootstrap_95_ci': interval(ratios)}
        result['batches'][batch] = {'counts_per_rotation': counts, 'iterations': iterations,
                                   'timing_pairs': rows, 'comparisons': comparisons}
        print(json.dumps({'batch': batch, 'counts': counts, 'comparisons': comparisons}), flush=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
