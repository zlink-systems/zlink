import json,pathlib,copy,re
j=pathlib.Path('/root/jobs/fx/cx/v21/s033/chand');raw=json.loads((j/'counts-raw.json').read_text());d=json.loads((j/'counts-summary.json').read_text())
def check(d):
 n=d['requests'];s=d['counts']['source'];r=s['2']['total'];w=s['3']['total']
 assert n>0 and r['rotation_start']==r['rotation_end']>0
 assert r['completion_post']==n,(r['completion_post'],n)
 assert r['completion_run']==0
 assert w['completion_run']>=n
 assert r['aligned']>=n
check(d)
assert raw['stats_after']['source']['submitted']==d['requests'] and raw['stats_after']['source']['errors']==0
assert raw['stats_after']['target']['allPhaseMessages']-raw['stats_before']['target']['allPhaseMessages']==d['requests']
bad=copy.deepcopy(d);bad['counts']['source']['2']['total']['completion_post']-=1
try:check(bad)
except AssertionError:negative=True
else:raise AssertionError('missing post not rejected')
log=(j/'harness-direct.log').read_text();assert log.endswith('PASS\n')
assert all(f'terminal_mode={i} deliveries=1' in log for i in range(5))
assert 'one_delivery=64 duplicate_terminals_rejected=64' in log
result={'valid_counts':'PASS','negative_missing_one_post':'REJECTED','source_target_count_agreement':d['requests'],'counter_calibration':'malloc=1 aligned_alloc=1 mutex_lock=1 cond_signal=1','contract_cases':['one delivery','timeout one terminal','cancel one terminal','shutdown one terminal','preparation exception one delivery','callback exception one delivery','independent callback progress'],'handoff_attempts':0,'handoff_successes':0,'handoff_zero_reason':'No handoff operation exists in current source; counters measure rotation denominator, not an absent assignment function.'}
(j/'quality.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
