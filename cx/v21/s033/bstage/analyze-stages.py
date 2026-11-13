import argparse, collections, json, math, pathlib, re
ap=argparse.ArgumentParser();ap.add_argument('capture');a=ap.parse_args();p=pathlib.Path(a.capture)
events=[]
for line in (p/'stages.txt').read_text(errors='replace').splitlines():
 m=re.search(r'\s(\d+)\s+\[\d+\]\s+(\d+\.\d+):\s+bstage:(\w+):',line)
 if not m: continue
 event={'tid':int(m[1]),'time':float(m[2]),'kind':m[3]}
 event.update(dict(re.findall(r'(frame|queue)=(\d+)',line)));events.append(event)
events.sort(key=lambda e:e['time']);(p/'events.json').write_text(json.dumps(events,indent=2)+'\n')
http=[(i,e) for i,e in enumerate(events) if e['kind']=='http'];requests=[];excluded=collections.Counter()
for (i,start),(j,end) in zip(http,http[1:]):
 if start.get('frame')!=end.get('frame'): excluded['different HTTP frame']+=1;continue
 window=events[i:j+1]
 spots=[e for e in window if e['kind']=='spot'];runs=[e for e in window if e['kind']=='run'];schedules=[e for e in window if e['kind']=='schedule']
 if len(spots)!=1 or len(runs)!=1 or len(schedules)!=1:excluded['incomplete or ambiguous stage sequence']+=1;continue
 spot,run,schedule=spots[0],runs[0],schedules[0]
 if run['queue']!=schedule['queue'] or not start['time']<=schedule['time']<=run['time']<=spot['time']<=end['time']:excluded['queue or time mismatch']+=1;continue
 requests.append({'start':start['time'],'end':end['time'],'queue':run['queue'],'events':window,'before_schedule_us':(schedule['time']-start['time'])*1e6,'schedule_to_run_us':(run['time']-schedule['time'])*1e6,'run_to_spot_us':(spot['time']-run['time'])*1e6,'spot_to_http_us':(end['time']-spot['time'])*1e6})
(p/'request-stages.json').write_text(json.dumps(requests,indent=2)+'\n')
summary={'requests':len(requests),'events':len(events),'excluded_http_adjacencies':dict(excluded),'stages':{}}
for key in ['before_schedule_us','schedule_to_run_us','run_to_spot_us','spot_to_http_us']:
 values=sorted(r[key] for r in requests);summary['stages'][key]={f'p{q}':values[math.ceil(q/100*len(values))-1] for q in [50,99]}
(p/'stage-summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
