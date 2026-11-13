from pathlib import Path
import json,statistics,re,subprocess,hashlib
job=Path('/root/jobs/fx/cx/v21/s033/n4w');raw=job/'raw';valid=[]
for d in sorted(raw.glob('*-L4-*')):
 if not d.is_dir():continue
 get=lambda n:json.loads((d/n).read_text())
 a=get('active-source-stats.json');t=get('target-stats.json');w=get('warmup-source-stats.json');wt=get('warmup-target-stats.json');life=get('lifetime.json')
 assert a['submitted']==a['completed']==t['received'] and a['errors']==t['errors']==0
 assert w['submitted']==w['completed']==wt['allPhaseMessages'] and w['errors']==wt['errors']==0
 assert life['wall_seconds']<20 and all(v==0 for v in life['exit_codes'].values())
 sched=(d/'scheduling.txt').read_text();assert all(s.strip()=='6-7,14-15' for s in re.findall(r'Cpus_allowed_list:(.*)',sched));assert 'SCHED_IDLE' not in sched
 valid.append({'directory':d.name,'warmupMessages':w['completed'],'activeMessages':a['completed'],'errors':0,'threadAffinity':'6-7,14-15','threadPolicy':'SCHED_OTHER','lifetimeSeconds':life['wall_seconds'],'exitCodes':life['exit_codes']})
micros=[]
for d in sorted(raw.glob('micro-*')):
 if not d.is_dir() or not (d/'lifetime.json').exists():continue
 life=json.loads((d/'lifetime.json').read_text());assert life['wall_seconds']<20 and life['exit_code']==0
 micros.append({'directory':d.name,'lifetimeSeconds':life['wall_seconds'],'exitCode':0})
rows=json.loads((raw/'micro-summary.json').read_text());micro=[]
for shape in ('0','1024','4096','nested','dto'):
 for kind in ('before','writer'):
  rs=[r for r in rows if r['directory'].startswith(f'micro-utf8-{shape}-{kind}-')]
  assert len(rs)==3
  vals=[r['allocation_bytes_per_message']['all'] for r in rs];cpus=[r['cpuUsPerMessage'] for r in rs];median=statistics.median(cpus)
  micro.append({'shape':shape,'kind':kind,'heapBytesPerMessageMedian':statistics.median(vals),'heapBytesRange':[min(vals),max(vals)],'cpuUsPerMessageMedian':median,'cpuG5SpreadPercent':100*max(abs(x-median)/median for x in cpus),'cpuVerdict':'판정 불가','rawDirectories':[r['directory'] for r in rs]})
l4=[]
for kind in ('before','writer'):
 for row in json.loads((raw/(kind+'-summary.json')).read_text()):
  if row['role']!='source':continue
  d=raw/row['cell'];n=sum(json.loads((d/f'{p}-source-stats.json').read_text())['completed'] for p in ('warmup','active'))
  l4.append({'directory':d.name,'kind':kind,'normalizationMessages':n,'cpuSubtreePct':row['cpu_subtree_pct'],'allocationKiBPerMessage':{k:v/n/1024 for k,v in row['allocation_subtree_bytes'].items()},'wholeHeapKiBPerMessage':row['sampled_allocation_bytes']/n/1024})
(raw/'final-comparison.json').write_text(json.dumps({'microUtf8':micro,'l4Source':l4},indent=2,ensure_ascii=False))
check={'branch':subprocess.check_output(['git','branch','--show-current'],text=True).strip(),'head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'status':subprocess.check_output(['git','status','--porcelain'],text=True).strip(),'corpus':json.loads((raw/'corpus.json').read_text())['passed'],'l4':valid,'micro':micros,'writerSha256':hashlib.sha256((job/'tools/writer.cjs').read_bytes()).hexdigest()}
(raw/'validation-summary.json').write_text(json.dumps(check,indent=2));print({'branch':check['branch'],'head':check['head'],'status':check['status'],'corpus':check['corpus'],'l4Runs':len(valid),'microRuns':len(micros),'maxL4Lifetime':max(x['lifetimeSeconds'] for x in valid),'maxMicroLifetime':max(x['lifetimeSeconds'] for x in micros)})
