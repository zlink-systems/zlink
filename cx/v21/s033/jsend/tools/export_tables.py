from pathlib import Path
import json,csv,collections
p=Path('/root/jobs/fx/cx/v21/s033/jsend');data=json.load(open(p/'syscall-summary.json'))
with open(p/'byte-histograms.csv','w') as f:
 w=csv.writer(f);w.writerow(['run','pid','tid','fd','syscall','kind','bucket_min_B','bucket_max_B_exclusive','calls','fraction'])
 for name,r in data.items():
  if 'preflight' in name:continue
  for t in r['rows']:
   if t['calls']<1000:continue
   for hist in ('requested_hist','returned_hist'):
    total=sum(t[hist].values())
    for i in range(33):w.writerow([name,t['pid'],t['tid'],t['fd'],'sendto' if t['op']==44 else 'recvfrom',hist,256*i,'' if i==32 else 256*(i+1),t[hist].get(str(i),0),t[hist].get(str(i),0)/total])
with open(p/'syscall-details.csv','w') as f:
 w=csv.writer(f);w.writerow(['run','rate','pid','tid','fd','syscall','calls_per_request','success_B_per_call','requested_B_per_call','short_calls','EAGAIN_calls','record_equivalents_per_success'])
 for name,r in data.items():
  if 'preflight' in name:continue
  for t in r['rows']:
   if t['calls']<1000:continue
   ini=json.load(open(p/'runs'/name/'capture/initial.json'));role='source' if t['pid']==ini['processes']['source']['pid'] else 'target';reply=(role=='target' and t['op']==44) or (role=='source' and t['op']==45)
   rec=(1443 if reply else 1435) if name.startswith('main') else 1252 if name.startswith('binding') else 1092
   w.writerow([name,r['rate'],t['pid'],t['tid'],t['fd'],'sendto' if t['op']==44 else 'recvfrom',t['calls_per_request'],t['bytes_per_positive'],t['requested_per_call'],t['short'],t['eagain'],t['bytes_per_positive']/rec])
print('exported')
