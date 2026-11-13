import collections,json,pathlib,math,re
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang')
def load(p):return json.loads(p.read_text())
S=load(P/'summary.json');PR=load(P/'profile-summary.json');rates=load(P/'rates.json')
c=collections.Counter(r['job']['mode'] for r in S);assert c=={'unpaced':72,'alloc':24,'paced':24,'profile':24,'handoff':24},c
assert len(PR)==24,len(PR)
for r in S:
 j=r['job'];assert r['messages']>0 and r['window_s']>0
 if j['mode']!='unpaced':assert abs(r['actual_rate']/rates[j['pattern']]-1)<.03,(j,r['actual_rate'])
 assert abs(r['total_us']-r['user_us']-r['kernel_us'])<1e-6
 assert abs(r['total_us']-sum(v['cpu_us'] for v in r['roles'].values()))<1e-6
 if j['mode'] in ('paced','profile'):
  assert r['tcp']['data_packets_per_msg']>0 and r['ss_data_segs_per_msg']>0,j
  log=(pathlib.Path(r['path'])/'tcpdump.log').read_text();assert '0 packets dropped by kernel' in log,(j,log)
  assert r['wakeups_per_msg'] is not None
 if j['mode']=='handoff':assert r['wakeups_per_msg']>0,(j,'zero wake starts')
 if j['mode']=='alloc':
  assert len(r['native_alloc'])==2 and all(v['count_per_msg']>0 and v['bytes_per_msg']>0 for v in r['native_alloc'].values()),j
  if j['lang']=='java':assert len(r['jvm_alloc'])==2 and all(v['count_per_msg']>0 for v in r['jvm_alloc'].values())
  if j['lang']=='dotnet':assert r['dotnet_alloc']['bytes_per_msg']>0 and r['dotnet_alloc'].get('count_per_msg_estimate',0)>0,(j,r.get('dotnet_alloc'))
  if j['lang']=='node':assert len(r['v8_alloc'])==2 and all(v['count_per_msg']>0 for v in r['v8_alloc'].values())
for r in PR:
 for side,v in r['roles'].items():assert v['native_samples']>0,(r['job'],side)
 if r['job']['lang']!='cpp':assert all(v['managed_samples']>0 for v in r['roles'].values()),r['job']
for p in (P/'runs').glob('*/*/meta.json'):
 m=load(p)
 if m.get('ok'):assert not any('error' in v for v in m['cleanup']),(p,m['cleanup'])
print('PASS: 72 unpaced + 24 allocation + 24 paced + 24 profile + 24 handoff; all common rates, CPU sums, packet drops, allocation domains, native/managed samples and cleanup checked.')
