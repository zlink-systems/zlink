import pathlib,json,collections,statistics,csv
j=pathlib.Path(__file__).parent;output=[]
for row in map(json.loads,(j/'summary.jsonl').read_text().splitlines()):
 path=pathlib.Path(row['raw']+'.proc.jsonl');ss=[json.loads(x) for x in path.read_text().splitlines()];mm=[(x['t'],x['memory']) for x in ss if x.get('memory') and .15<x['t']<4.8];depth=[(m['backlog_lo']+m['backlog_hi'])/2 for t,m in mm if 'backlog_lo' in m and m['backlog_hi']>=m['backlog_lo'] and -50<m['backlog_lo']<100000]
 counters=collections.Counter(c['addr'] for t,m in mm for c in m.get('seq_counter_candidates',[]));deadlines=collections.Counter(c['addr'] for t,m in mm for c in m.get('deadline_candidates',[]));counter=counters.most_common(1)[0][0] if counters else None;deadline=deadlines.most_common(1)[0][0] if deadlines else None
 shares=[]
 if len(row['roles'])==2:
  r,s=map(str,[row['roles']['receiver'],row['roles']['sender']])
  for snap in ss:
   if r in snap['threads'] and s in snap['threads']:
    shares.append(snap['threads'][r]['cpu']//2==snap['threads'][s]['cpu']//2)
 modes=[]
 for lower,upper in [(0,.3),(.3,.6),(.6,1),(1,2),(2,3),(3,4),(4,4.8)]:
  v=[m for t,m in mm if lower<=t<upper];d=[(m['backlog_lo']+m['backlog_hi'])/2 for m in v if 'backlog_lo' in m];modes.append({'window':[lower,upper],'backlog_avg':statistics.mean(d) if d else None})
 out={'label':row['label'],'run':row['run'],**row['values'],'backlog_avg':statistics.mean(depth) if depth else None,'backlog_max':max(depth) if depth else None,'memory_samples':len(mm),'sender_counter_addr':hex(counter) if counter else None,'deadline_addr':hex(deadline) if deadline else None,'shared_cacheline':counter//64==deadline//64 if counter and deadline else None,'same_physical_core_fraction':statistics.mean(shares) if shares else None,'time_windows':modes,'cpu_histogram':row['cpu_histogram'],'raw':row['raw']};output.append(out)
(j/'raw/observed-analysis.json').write_text(json.dumps(output,indent=2)+'\n')
for r in output:print(r['label'],r['run'],'latency',r.get('latency'),'backlog',round(r['backlog_avg'],1) if r['backlog_avg'] is not None else None,'counter',r['sender_counter_addr'],'deadline',r['deadline_addr'],'shared',r['shared_cacheline'],'smt_fraction',round(r['same_physical_core_fraction'],2) if r['same_physical_core_fraction'] is not None else None)
