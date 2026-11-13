#!/usr/bin/env python3
import csv
import pathlib
import sys

phase, out, source_pid, target_pid = sys.argv[1:]
clock_ticks = __import__('os').sysconf('SC_CLK_TCK')
rows = []
for role, pid in (('source', source_pid), ('target', target_pid)):
    task_root = pathlib.Path('/proc') / pid / 'task'
    for task in sorted(task_root.iterdir(), key=lambda p: int(p.name)):
        tid = task.name
        stat = (task / 'stat').read_text()
        close = stat.rfind(')')
        comm = stat[stat.find('(') + 1:close]
        fields = stat[close + 2:].split()
        ticks = int(fields[11]) + int(fields[12])
        ns = round(ticks * 1_000_000_000 / clock_ticks)
        status = (task / 'status').read_text().splitlines()
        voluntary = next((int(x.split()[1]) for x in status if x.startswith('voluntary_ctxt_switches:')), 0)
        involuntary = next((int(x.split()[1]) for x in status if x.startswith('nonvoluntary_ctxt_switches:')), 0)
        rows.append((role, tid, comm, ns, voluntary, involuntary))
with open(out, 'w', newline='') as stream:
    writer = csv.writer(stream, delimiter='\t')
    writer.writerow(('role', 'tid', 'comm', 'runtime_ns', 'voluntary', 'involuntary'))
    writer.writerows(rows)
