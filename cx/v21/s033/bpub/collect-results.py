import json
from pathlib import Path
import shutil
import sys
import xml.etree.ElementTree as ET

root = Path('/root/worktree/zlink-4060-binding-close/bindings/java')
out = Path('/root/jobs/fx/cx/v21/s033/bpub')
label = sys.argv[1]
selected = sys.argv[2] if len(sys.argv) > 2 else ''
projects = ('', 'codec/zlink-ext-netty', 'kotlin-contract-test',
            'samples/Zlink.Samples', 'perf/single/Zlink.BindingBench',
            'perf/multi/Zlink.BindingBench.Multi')
rows = []
outputs = []
for project in projects:
    results = root / project / 'build/test-results'
    for path in sorted(results.glob('*/TEST-*.xml')):
        suite = ET.parse(path).getroot()
        if selected and not any(token in suite.get('name', '')
                                for token in selected.split('|')):
            continue
        relative = path.relative_to(root)
        target = out / 'results' / label / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        row = {'path': str(relative), 'name': suite.get('name'),
               **{key: int(suite.get(key, 0))
                  for key in ('tests', 'failures', 'errors', 'skipped')}}
        rows.append(row)
        for node in suite.findall('system-out'):
            if node.text and 'rounds=' in node.text:
                outputs.append(node.text.strip())
summary = {'suites': rows, 'totals': {
    key: sum(row[key] for row in rows)
    for key in ('tests', 'failures', 'errors', 'skipped')},
    'outcomes': outputs}
(out / (label + '-results.json')).write_text(
    json.dumps(summary, indent=2) + '\n')
print(json.dumps({'label': label, 'totals': summary['totals'],
                  'outcomes': outputs}))
