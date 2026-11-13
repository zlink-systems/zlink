import bisect, collections, json, pathlib, re, statistics
base = pathlib.Path('/root/jobs/fx/cx/v21/s033/dseg3')
meta = json.loads((base / 'capture.json').read_text())
sp, tp = meta['pids']['source'], meta['pids']['target']
trace_prefix = pathlib.Path(meta['trace_prefix'])
def load(path):
    rows = []
    with path.open() as source:
        for line in source:
            row = json.loads(line)
            row['t'] = row['Timestamp'] / 1e9
            rows.append(row)
    return rows
source = load(pathlib.Path(f'{trace_prefix}.{sp}.bench.jsonl'))
target = load(pathlib.Path(f'{trace_prefix}.{tp}.bench.jsonl'))
def stat(values):
    values = sorted(values)
    if not values:
        return {'n': 0}
    def q(p):
        x = (len(values) - 1) * p
        low = int(x)
        high = min(low + 1, len(values) - 1)
        return round(values[low] + (values[high] - values[low]) * (x - low), 3)
    return {'n': len(values), 'p50': q(.5), 'p90': q(.9), 'mean': round(statistics.fmean(values), 3)}
line_re = re.compile(r'^\s*(.*?)\s+(\d+)/(\d+)\s+\[(\d+)\]\s+([0-9.]+):\s+(.*)$')
ent_re = re.compile(r'raw_syscalls:sys_enter: NR (\d+) \(([^)]*)\)')
ext_re = re.compile(r'raw_syscalls:sys_exit: NR (\d+) = (-?\d+)')
sw_re = re.compile(r'\s*(.*?):(\d+) \[[^]]+\] (\S+) ==> (.*?):(\d+) \[')
pending, calls, switches = {}, [], []
with (base / 'perf.script').open(errors='replace') as source_file:
    for line in source_file:
        match = line_re.match(line)
        if not match:
            continue
        comm, pid, tid, cpu, timestamp, event = match.groups()
        pid, tid, cpu, timestamp = int(pid), int(tid), int(cpu), float(timestamp)
        if pid in (sp, tp):
            match_enter = ent_re.search(event)
            if match_enter:
                pending[pid, tid] = (timestamp, int(match_enter.group(1)), match_enter.group(2), comm, cpu)
                continue
            match_exit = ext_re.search(event)
            if match_exit:
                number, result = int(match_exit.group(1)), int(match_exit.group(2))
                enter = pending.pop((pid, tid), None)
                if enter and enter[1] == number:
                    args = enter[2].split(',')
                    def hx(value):
                        value = value.strip()
                        return int(value, 16) if re.fullmatch(r'[0-9a-fA-F]+', value) else -1
                    calls.append({'pid': pid, 'tid': tid, 'nr': number, 'ret': result, 'start': enter[0], 'end': timestamp, 'fd': hx(args[0]) if args else -1, 'comm': enter[3], 'cpu': enter[4]})
                continue
        if 'sched:sched_switch:' in event:
            match_switch = sw_re.search(event.split('sched:sched_switch:', 1)[1])
            if match_switch:
                switches.append({'t': timestamp, 'cpu': cpu, 'old': int(match_switch.group(2)), 'oldcomm': match_switch.group(1), 'new': int(match_switch.group(5)), 'newcomm': match_switch.group(4), 'pid': pid})
# x86-64 raw syscall numbers: sendto=44, recvfrom=45.
def data(pid, number):
    rows = [call for call in calls if call['pid'] == pid and call['nr'] == number and 1000 <= call['ret'] <= 1600]
    return sorted(rows, key=(lambda x: x['start']) if number == 44 else (lambda x: x['end']))
S, Tr, T, R = data(sp, 44), data(tp, 45), data(tp, 44), data(sp, 45)
start = min((c['start'] for c in calls), default=0)
end = max((c['end'] for c in calls), default=0)
def trim(rows):
    return [r for r in rows if start <= r['t'] <= end]
def flow(rows, f, stage):
    return sorted((r for r in rows if r.get('Flow') == f and r.get('Stage') == stage), key=lambda r: r['t'])
B1, B4 = trim(flow(target, 'B', 1)), trim(flow(target, 'B', 4))
S1all, S4, P1 = trim(flow(source, 'S', 1)), trim(flow(source, 'S', 4)), trim(flow(source, 'P', 1))
# Keep only one S1 per serial if earlier diagnostic captures contain both the
# reply decode and RawBenchTransport.RequestAsync return markers.
S1byserial = {}
for marker in S1all:
    if marker['Serial'] not in S1byserial or marker['t'] > S1byserial[marker['Serial']]['t']:
        S1byserial[marker['Serial']] = marker
S1 = sorted(S1byserial.values(), key=lambda r: r['t'])
B1t = [r['t'] for r in B1]
B4by = {r['Serial']: r for r in B4}
Rend = [r['end'] for r in R]
Tend = [r['end'] for r in T]
S1t = [r['t'] for r in S1]
S4t = [r['t'] for r in S4]
Trt = [r['end'] for r in Tr]
Tst = [r['start'] for r in T]
Sst = [r['start'] for r in S]
def after(rows, times, t):
    i = bisect.bisect_right(times, t)
    return rows[i] if i < len(rows) else None
cycles = []
for send in S:
    recv_target = after(Tr, Trt, send['start'])
    if not recv_target: continue
    t1 = after(B1, B1t, recv_target['end'])
    if not t1: continue
    t4 = B4by.get(t1['Serial'])
    if not t4 or t4['t'] <= t1['t']: continue
    send_target = after(T, Tst, t4['t'])
    if not send_target: continue
    recv_source = after(R, Rend, send_target['end'])
    if not recv_source: continue
    s1 = after(S1, S1t, recv_source['end'])
    if not s1: continue
    s4 = after(S4, S4t, s1['t'])
    if not s4 or s4['Serial'] <= s1['Serial']: continue
    next_send = after(S, Sst, s4['t'])
    if not next_send: continue
    stamps = [send['start'], recv_target['end'], t1['t'], t4['t'], send_target['start'], recv_source['end'], s1['t'], s4['t'], next_send['start']]
    if all(stamps[i] < stamps[i+1] for i in range(len(stamps)-1)):
        cycles.append(([send, recv_target, t1, t4, send_target, recv_source, s1, s4, next_send], stamps))
print('COUNTS', {'pids': meta['pids'], 'perf_window': [start, end], 'syscalls': {'source_sendto': len(S), 'target_recvfrom': len(Tr), 'target_sendto': len(T), 'source_recvfrom': len(R)}, 'markers': {'T1': len(B1), 'T4': len(B4), 'S1': len(S1), 'S4': len(S4), 'poll_completion_return': len(P1)}, 'complete_cycles': len(cycles)})
labels = ['source sendto→target recvfrom', 'target recvfrom→T1', 'T1→T4', 'T4→target sendto', 'target sendto→source recvfrom', 'source recvfrom→S1', 'S1→S4', 'S4→next source sendto']
for i, label in enumerate(labels):
    values = [(stamps[i+1]-stamps[i])*1e6 for _, stamps in cycles]
    starts = collections.Counter()
    ends = collections.Counter()
    for seq, stamps in cycles:
        a, b = seq[i], seq[i+1]
        starts[(a.get('tid', a.get('ThreadId')), a.get('comm', a.get('ThreadName')))] += 1
        ends[(b.get('tid', b.get('comm', b.get('ThreadName'))), b.get('comm', b.get('ThreadName')))] += 1
    print('SEG', label, stat(values), 'start_threads', starts.most_common(4), 'end_threads', ends.most_common(4))
# Thread IDs that execute poller waits while a reply is returned to S1.
wait_syscalls = {7: 'poll', 232: 'epoll_wait', 271: 'ppoll', 281: 'epoll_pwait', 73: 'pselect6', 23: 'select'}
waiters = collections.Counter()
wait_details = collections.Counter()
wait_calls = sorted((c for c in calls if c['pid'] == sp and c['nr'] in wait_syscalls), key=lambda c: c['start'])
wait_starts = [c['start'] for c in wait_calls]
for seq, stamps in cycles:
    recv, s1 = seq[5], seq[6]
    lo, hi = bisect.bisect_left(wait_starts, recv['end']), bisect.bisect_left(wait_starts, s1['t'])
    for call in wait_calls[lo:hi]:
        waiters[(call['tid'], call['comm'], wait_syscalls[call['nr']])] += 1
print('COMPLETION_WAITERS_IN_RECV_TO_S1', waiters.most_common(20))
P1t = [r['t'] for r in P1]
same = different = missing = 0
completion_threads = collections.Counter()
for seq, stamps in cycles:
    recv, s1 = seq[5], seq[6]
    next_send = seq[8]
    lo, hi = bisect.bisect_left(P1t, recv['end']), bisect.bisect_right(P1t, next_send['start'])
    candidates = P1[lo:hi]
    p = min(candidates, key=lambda item: abs(item['t'] - s1['t'])) if candidates else None
    if not p:
        missing += 1
        continue
    completion_threads[(p['ThreadId'], p['ThreadName'])] += 1
    if (p['ThreadId'], p['ThreadName']) == (s1['ThreadId'], s1['ThreadName']):
        same += 1
    else:
        different += 1
print('COMPLETION_DRAIN_VS_S1_RESUME', {'same_thread': same, 'different_thread': different, 'missing_completion_poll_return': missing, 'drain_threads': completion_threads.most_common(12)})
for seq, stamps in cycles[:8]:
    recv, s1 = seq[5], seq[6]
    cs = [(c['tid'], c['comm'], c['nr'], c['ret']) for c in calls if c['pid'] == sp and recv['end'] <= c['start'] < s1['t']]
    print('SAMPLE', {'source_recv_tid': recv['tid'], 's1_managed_thread': s1['ThreadName'], 's1_managed_id': s1['ThreadId'], 'source_calls_recv_to_s1': cs})
print('SYSCALL_TIDS_SOURCE', collections.Counter((c['tid'], c['comm'], c['nr']) for c in calls if c['pid'] == sp).most_common(30))
# Infer native TIDs for managed markers from sched_switch intervals. The same
# managed thread ID should resolve to one native TID across the active window.
source_tids = {c['tid'] for c in calls if c['pid'] == sp}
by_cpu = collections.defaultdict(list)
for switch in switches:
    by_cpu[switch['cpu']].append(switch)
switch_times = {}
for cpu, rows in by_cpu.items():
    rows.sort(key=lambda row: row['t'])
    switch_times[cpu] = [row['t'] for row in rows]
managed_candidates = collections.defaultdict(collections.Counter)
managed_intersections = {}
for marker in S1:
    candidates = set()
    for cpu, rows in by_cpu.items():
        ix = bisect.bisect_right(switch_times[cpu], marker['t']) - 1
        if ix >= 0:
            row = rows[ix]
            if row['new'] in source_tids and row['newcomm'] == marker['ThreadName']:
                candidates.add(row['new'])
    managed_candidates[marker['ThreadId']].update(candidates)
    if marker['ThreadId'] not in managed_intersections:
        managed_intersections[marker['ThreadId']] = candidates
    else:
        managed_intersections[marker['ThreadId']] &= candidates
print('S1_NATIVE_TID_CANDIDATES', {mid: {'intersection': sorted(managed_intersections[mid]), 'counts': managed_candidates[mid].most_common(8)} for mid in managed_candidates})
