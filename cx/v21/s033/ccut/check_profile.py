import csv, json, re
from collections import defaultdict
from pathlib import Path
OUT=Path('/root/jobs/fx/cx/v21/s033/ccut')
ROOT=Path('/root/jobs/fx/cx/v21/s033/ccall')
p=json.loads((OUT/'attribution.json').read_text())
def group(s):
    if 'init_size' in s: return 'native message allocation'
    if 'epoll_ctl' in s: return 'epoll syscall samples'
    if 'async()' in s and 'request_submit' in s: return 'request async bundle'
    if 'insert_entry_locked' in s: return 'binding pending insertion'
    if 'router_socket_t::request' in s: return 'binding builder factory'
    if 'submit_request_attempt' in s: return 'binding attempt'
    if 'submit_raw_request_state' in s: return 'binding native array submit'
    if 'completion_owner_t::drain' in s or any(x in s for x in ['zlink_completion','complete_reply_from_transport','settle_if_joined','capture(zlink_completion']): return 'completion consume/drain'
    return 'other binding/Core/kernel samples'
r={}
for impl,v in p.items():
    g=defaultdict(float)
    for s,stacks in v['binding_stacks'].items():g[group(s)]+=sum(stacks.values())
    r[impl]={'binding_groups':dict(g),'normalized_cpu_us':v['cpu_us'],'stages':{s:sum(vs.values()) for s,vs in v['stages'].items()}}
    assert abs(sum(g.values())-r[impl]['stages']['binding submit'])<1e-8
    assert abs(sum(r[impl]['stages'].values())-v['cpu_us'])<1e-8
summary=json.loads((ROOT/'profile-summary.json').read_text())
for impl in r:
    r[impl]['samples']=summary[impl]['critical']['samples']
    r[impl]['profile_cpu_us']=summary[impl]['critical']['cpu_us']
    rs=list(csv.DictReader((OUT/f'{impl}-caller-functions.csv').open()))
    sums=defaultdict(float)
    for row in rs:
        s=row['symbol'];w=float(row['self_us'])
        if re.search(r'(^|::)(malloc|cfree|calloc|realloc|_int_malloc|_int_free|_int_free_merge_chunk|_int_free_create_chunk|malloc_consolidate|unlink_chunk)',s):sums['allocator_self']+=w
        if 'pthread_mutex_' in s:sums['pthread_mutex_self']+=w
        if '_Sp_counted' in s or '__shared_count' in s:sums['shared_count_self']+=w
    r[impl]['exclusive_leaf_sums']=dict(sums)
r['budget']={
 'baseline':{'registration':4.4235,'result observation':4.2523,'encode':2.1547,'binding submit':5.5365,'other':1.113},
 'pr1':{'registration':2.10,'result observation':1.20,'encode':2.1547,'binding submit':5.5365,'other':1.113},
 'pr2':{'registration':1.35,'result observation':1.20,'encode':.65,'binding submit':4.0,'other':1.113},
 'pr3_target':{'registration':.65,'result observation':.80,'encode':.65,'binding submit':3.3,'other':1.113},
 'conservative_scenario':{'registration':1.50,'result observation':1.50,'encode':1.0,'binding submit':4.0,'other':1.113}}
for k,b in r['budget'].items(): print(k,round(sum(b.values()),4))
(OUT/'profile-check.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print('Profile totals and disjoint binding groups: PASS')
print({k:v['exclusive_leaf_sums'] for k,v in r.items() if 'exclusive_leaf_sums' in v})
print('MemAvailable:', next(s for s in Path('/proc/meminfo').read_text().splitlines() if s.startswith('MemAvailable:')))

# Validate the accounting in both directions without running the benchmark.
def validate_totals(data):
    for impl in ('zlink-framework-cpp', 'zlink-cpp'):
        value = data[impl]
        assert abs(sum(value['stages'].values()) - value['normalized_cpu_us']) < 1e-8
        assert abs(sum(value['binding_groups'].values()) - value['stages']['binding submit']) < 1e-8
validate_totals(r)
import copy
checks = {'original_totals': 'accepted'}
for label, corrupt in [('changed_stage', 'stage'), ('changed_binding_group', 'group')]:
    candidate = copy.deepcopy(r)
    if corrupt == 'stage':
        candidate['zlink-framework-cpp']['stages']['registration'] += 0.01
    else:
        candidate['zlink-framework-cpp']['binding_groups']['binding attempt'] += 0.01
    try:
        validate_totals(candidate)
    except AssertionError:
        checks[label] = 'rejected'
    else:
        raise AssertionError('corrupted accounting was accepted')
(OUT/'profile-check-validation.json').write_text(json.dumps(checks, indent=2)+'\n')
print('Positive/negative accounting checks:', checks)
