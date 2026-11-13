#!/usr/bin/env python3
import glob, json, os, sys
from collections import defaultdict

root = '/root/jobs/fx/cx/v21/s033/lad-node-l0f/raw/profile/cpu'
profiles = glob.glob(root + '/*.cpuprofile')

def read_profile(path):
    data = json.load(open(path))
    nodes = {node['id']: node['callFrame'] for node in data['nodes']}
    weights = defaultdict(float)
    counts = defaultdict(int)
    for sample, delta in zip(data['samples'], data['timeDeltas']):
        frame = nodes[sample]
        url = frame.get('url', '')
        if url.startswith('file://'):
            url = url[7:]
        key = (frame.get('functionName') or '(anonymous)', url, frame.get('lineNumber', -1) + 1)
        weights[key] += delta
        counts[key] += 1
    return data, weights, counts

roles = {}
for path in profiles:
    data = json.load(open(path))
    urls = [node['callFrame'].get('url', '') for node in data['nodes']]
    if any('zlink-raw-server/main.js' in url for url in urls): roles['binding'] = path
    if any('zlink-framework-server/main.js' in url for url in urls): roles['L0'] = path
if set(roles) != {'binding', 'L0'}:
    print('Could not locate both target server profiles:', roles, file=sys.stderr)
    sys.exit(1)

loaded = {role: read_profile(path) for role, path in roles.items()}
for role in ('binding', 'L0'):
    data, _, _ = loaded[role]
    print(f"{role}: {os.path.relpath(roles[role], '/root/jobs/fx/cx/v21/s033/lad-node-l0f')}, samples={len(data['samples'])}")

binding_data, binding_weights, binding_counts = loaded['binding']
l0_data, l0_weights, l0_counts = loaded['L0']
raw_total = sum(binding_counts.values())
l0_total = sum(l0_counts.values())
keys = set(binding_counts) | set(l0_counts)
rows = []
for key in keys:
    raw_pct = 100 * binding_counts[key] / raw_total
    l0_pct = 100 * l0_counts[key] / l0_total
    diff = abs(raw_pct - l0_pct)
    if diff > 1.0:
        rows.append((diff, key, raw_pct, l0_pct,
                     binding_weights[key] / 1000, l0_weights[key] / 1000))
for diff, (name, path, line), raw_pct, l0_pct, raw_ms, l0_ms in sorted(rows, reverse=True):
    print(f'{name}\t{path}:{line}\t{raw_pct:.2f}% ({raw_ms:.1f} ms)\t{l0_pct:.2f}% ({l0_ms:.1f} ms)\t{diff:.2f} pp')
