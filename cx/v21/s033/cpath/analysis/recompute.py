"""Read supplied text/JSON only. No benchmark, build, test, or runtime probe."""
import json
import re
import statistics as st
import ast
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INPUT = ROOT / 'input'
OUT = ROOT / 'analysis'

def read(path):
    return json.loads(path.read_text())

def median(values):
    return st.median(values)

def language_table(name, patterns=None):
    rows = []
    for line in (INPUT / name).read_text().splitlines():
        c = [x.strip() for x in line.split('|')[1:-1]]
        if len(c) < 5 or not c[2].isdigit():
            continue
        if patterns and c[1] not in patterns:
            continue
        if c[0] == 'cpp':
            continue
        rows.append(dict(language=c[0], pattern=c[1], size=int(c[2]),
                         rate=float(c[3].replace(',', '')), input=name))
    return rows

other = (language_table('ratio053java__table.md')
         + language_table('ratio054net__table.md')
         + language_table('ratio040__ratio040-24cells.md'))
cells = []
raw_runs = []
for pattern in ['send-saturation', 'request-backpressure', 'request-serial']:
    for size in [1024, 4096]:
        variants = {}
        for impl in ['zlink-cpp', 'zlink-framework-cpp']:
            runs = []
            for run in range(1, 6):
                path = (INPUT / 'ratio051pr2/ratio051pr2/log/after/cpp'
                        / f'run{run}' / f'{impl}-{pattern}-{size}/results.json')
                c = read(path)['cells'][0]
                rate = c['throughput_per_second']
                seconds = c['active_elapsed_ms'] / 1000
                # CPU snapshots use active window, throughput uses target receive
                # count for send and source completion count for request.
                denom = (c['server_received_at_close'] if pattern == 'send-saturation'
                         else c['completed_at_close'])
                source_us = c['client_cores'] * seconds * 1e6 / denom
                target_us = (c['server_cpu_percent'] / 100 * c['logical_cores']
                             * seconds * 1e6 / c['server_received_at_close'])
                caller_us = c['submit_thread_cores'] * seconds * 1e6 / denom
                row = dict(input=str(path.relative_to(INPUT)), rate=rate,
                           source_us=source_us, target_us=target_us, caller_us=caller_us,
                           caller_cores=c['submit_thread_cores'],
                           source_cores=c['client_cores'],
                           target_cores=c['server_cpu_percent']/100*c['logical_cores'],
                           errors=c['errors'], abandoned=c['abandoned'],
                           contaminated=c['contaminated'], drain_bound_hit=c['drain_bound_hit'])
                runs.append(row)
                raw_runs.append(row)
            variants[impl] = {k: median([r[k] for r in runs]) for k in
                              ['rate', 'source_us', 'target_us', 'caller_us',
                               'caller_cores', 'source_cores', 'target_cores']}
        b, f = variants['zlink-cpp'], variants['zlink-framework-cpp']
        fastest = max([x for x in other if x['pattern'] == pattern and x['size'] == size],
                      key=lambda x: x['rate'])
        q60 = .6 * b['rate']
        goal = max(q60, fastest['rate'])
        t60, tg = 1e6/q60, 1e6/goal
        row = dict(pattern=pattern, size=size, binding=b, framework=f,
                   ratio=f['rate']/b['rate'], q60=q60, fastest_earlier=fastest,
                   goal=goal, tighter='0.6' if q60 >= fastest['rate'] else 'absolute',
                   time60_us=t60, effective_time_us=tg,
                   caller_share60_us=t60-b['caller_us'],
                   wall_share60_us=t60-1e6/b['rate'],
                   caller_effective_share_us=tg-b['caller_us'],
                   wall_effective_share_us=tg-1e6/b['rate'])
        for role in ['source', 'target']:
            baseline = b[role+'_us']
            row[role+'_cpu_budget60_us'] = baseline/.6
            row[role+'_cpu_share60_us'] = baseline*(1/.6-1)
            row[role+'_cpu_effective_budget_us'] = baseline*b['rate']/goal
            row[role+'_cpu_effective_share_us'] = baseline*(b['rate']/goal-1)
        cells.append(row)

# Supplied /proc capture snapshots: HZ is recorded as 100 by collection metadata.
captures = []
for base in ['csrc__raw/raw/paced', 'csrc__raw/raw/profiles',
             'ctrecv__raw/raw/final/paced', 'ctrecv__raw/raw/final/unpaced',
             'ctrecv__raw/raw/baseline/paced']:
    for before_path in sorted((INPUT/base).glob('*/capture/before.json')):
        after_path = before_path.with_name('after.json')
        if not after_path.exists():
            continue
        before, after = read(before_path), read(after_path)
        seconds = after['monotonic'] - before['monotonic']
        roles = {}
        for role in ['source', 'target']:
            count_key = 'completed' if role == 'source' else 'received'
            n = after['stats'][role][count_key] - before['stats'][role][count_key]
            a,b = after['processes'][role], before['processes'][role]
            us = {mode: (a[key]-b[key])*.01*1e6/n
                  for mode,key in [('user','utime_ticks'),('kernel','stime_ticks')]}
            threads = []
            for tid,t in a['threads'].items():
                old = b['threads'].get(tid)
                if not old:
                    continue
                runtime = (t['runtime_ns']-old['runtime_ns'])/1e9
                threads.append(dict(tid=tid,comm=t['comm'],cpu_us=runtime*1e6/n,
                                    cores=runtime/seconds,
                                    user_us=(t['utime_ticks']-old['utime_ticks'])*1e4/n,
                                    kernel_us=(t['stime_ticks']-old['stime_ticks'])*1e4/n))
            roles[role]=dict(n=n,**us,total=us['user']+us['kernel'],
                             top_threads=sorted(threads,key=lambda t:t['cpu_us'],reverse=True)[:6])
        captures.append(dict(input=str(before_path.parent.relative_to(INPUT)),seconds=seconds,
                             roles=roles))

summary = dict(cells=cells, runs=raw_runs, captures=captures,
               notes=['CPU process values are user+kernel.',
                      'Per-run CPU/count ratios are reduced by median, not ratios of medians.',
                      'Serial reciprocal throughput is RTT, not one saturated-thread CPU cost.',
                      'No target per-thread counters in ratio051pr2; no inferred saturation proof.'])
(OUT/'recomputed.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')

# Reuse only the regex literals of the supplied attribution definitions. Never
# import/run their collectors: those scripts have historical output locations.
def regex_literals(path, wanted):
    result = {}
    for node in ast.parse(path.read_text()).body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id in wanted):
            call = node.value
            flags = re.I if len(call.args) > 1 else 0
            result[node.targets[0].id] = re.compile(ast.literal_eval(call.args[0]), flags)
    return result

patterns = regex_literals(INPUT/'ccall2/ccall2/scripts/analyze.py', {'ALLOC','SYNC'})
patterns.update(regex_literals(INPUT/'ccall2/ccall2/scripts/summarize.py',
                               {'TASK','CODEC','BINDING','REG','DRIVER_RESULT'}))

def caller_stage(frames):
    for symbol, dso in frames:
        if patterns['ALLOC'].search(symbol) or patterns['SYNC'].search(symbol):
            continue
        for pattern, stage in [('CODEC','encode'), ('TASK','result observation')]:
            if patterns[pattern].search(symbol):
                return stage
        if patterns['BINDING'].search(symbol) or '/libzlink.so' in dso:
            return 'binding submit'
        if patterns['REG'].search(symbol) or 'zlink::framework::' in symbol or '/libzlink_framework' in dso:
            return 'registration'
        if any(s in symbol for s in ['framework_driver_t::','binding_driver_t::','request_loop']):
            return 'result observation' if patterns['DRIVER_RESULT'].search(symbol) else 'other'
    return 'other'

ccap=INPUT/'ccall2/ccall2/out-profile/run1/zlink-framework-cpp-request-backpressure-1024/capture'
cb, ca = read(ccap/'before.json'),read(ccap/'after.json')
cn=ca['stats']['source']['completed']-cb['stats']['source']['completed']
cthreads=ca['processes']['source']['threads']
ctid=max(cthreads, key=lambda tid: cthreads[tid]['runtime_ns']-
          cb['processes']['source']['threads'].get(tid,{}).get('runtime_ns',0))
ccpu=(cthreads[ctid]['runtime_ns']-cb['processes']['source']['threads'][ctid]['runtime_ns'])/1000/cn
csamples=[x for x in read(ccap/'source.samples.json') if x['tid']==ctid]
cperiod=sum(x['period'] for x in csamples)
groups=Counter()
leaves=Counter()
for sample in csamples:
    groups[caller_stage(sample['frames'])]+=sample['period']
    if sample['frames']:
        leaves[sample['frames'][0][0]]+=sample['period']
unprofiled=read(INPUT/'ccall2/ccall2/out-unprofiled/run1/zlink-framework-cpp-request-backpressure-1024/results.json')['cells'][0]
uncpu=unprofiled['submit_thread_cores']*1e6/unprofiled['throughput_per_second']
ccall=dict(messages=cn,caller_tid=ctid,caller_us=ccpu,period=cperiod,
           stages_period=dict(groups),stages_us={k:v/cperiod*ccpu for k,v in groups.items()},
           unprofiled_caller_us=uncpu,
           stages_normalized_unprofiled_us={k:v/cperiod*uncpu for k,v in groups.items()},
           leaf_period=dict(leaves),leaf_us={k:v/cperiod*ccpu for k,v in leaves.items()})

receive_profiles=[]
for p in sorted((INPUT/'ctrecv__raw/raw').glob('*/profiles/*/capture/target.attribution.json')):
    a=read(p)
    cap=p.parent
    if not (cap/'before.json').exists():
        continue
    before,after=read(cap/'before.json'),read(cap/'after.json')
    n=after['stats']['target']['received']-before['stats']['target']['received']
    us=(after['processes']['target']['utime_ticks']-before['processes']['target']['utime_ticks'])*1e4/n
    # Fixed-period cpu-clock samples in these captures. Check denominator.
    assert sum(a['parts_samples'].values())==a['user_samples']
    receive_profiles.append(dict(input=str(p.relative_to(INPUT)),variant=a['variant'],
       n=n,user_us=us,user_samples=a['user_samples'],parts_samples=a['parts_samples'],
       parts_us={k:v/a['user_samples']*us for k,v in a['parts_samples'].items()}))
source_profiles=[]
for variant in ['main','change']:
    a=read(INPUT/'csrc__raw/raw/fresh-source-attribution.json')[variant]
    cap=INPUT/f'csrc__raw/raw/profiles/{variant}-2/capture'
    before,after=read(cap/'before.json'),read(cap/'after.json')
    n=after['stats']['source']['completed']-before['stats']['source']['completed']
    us=(after['processes']['source']['utime_ticks']-before['processes']['source']['utime_ticks'])*1e4/n
    source_profiles.append(dict(variant=variant,n=n,user_us=us,
       # Group shares are supplied exclusive attribution, not new profiling.
       groups_us={k:v/a['profile_user_us']*us for k,v in a['groups_us'].items()}))
historical={}
for name in ['cdrain','cdrain2']:
    rows=[]
    for line in (INPUT/f'{name}__report.md').read_text().splitlines():
        fields=[x.strip() for x in line.split('|')[1:-1]]
        if len(fields)==6 and fields[0] in ['send-saturation 1K','request-backpressure 1K'] and fields[1].isdigit():
            rows.append(dict(cell=fields[0],run=int(fields[1]),variant=fields[2],
                             b=float(fields[3].replace(',','')),f=float(fields[4].replace(',',''))))
    medians={}
    for cell in ['send-saturation 1K','request-backpressure 1K']:
        medians[cell]={}
        for variant in ['main','branch']:
            rr=[x for x in rows if x['cell']==cell and x['variant']==variant]
            if rr:
                b,f=median([x['b'] for x in rr]),median([x['f'] for x in rr])
                medians[cell][variant]=dict(b=b,f=f,ratio=f/b)
        if all(v in medians[cell] for v in ['main','branch']):
            medians[cell]['throughput_change_percent']=(medians[cell]['branch']['f']/medians[cell]['main']['f']-1)*100
    historical[name]=dict(supplied_report_rows=rows,medians=medians)
evidence=dict(ccall=ccall,receive_profiles=receive_profiles,source_profiles=source_profiles,
              historical=historical)
(OUT/'evidence.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2)+'\n')
for c in cells:
    print(c['pattern'],c['size'],
          'B/F',round(c['binding']['rate'],3),round(c['framework']['rate'],3),
          'goal',round(c['goal'],3),'t',round(c['effective_time_us'],3),
          'caller B/F',round(c['binding']['caller_us'],3),round(c['framework']['caller_us'],3),
          'caller share',round(c['caller_effective_share_us'],3),
          'CPU B src/tgt',round(c['binding']['source_us'],3),round(c['binding']['target_us'],3),
          'CPU share src/tgt',round(c['source_cpu_effective_share_us'],3),round(c['target_cpu_effective_share_us'],3))
