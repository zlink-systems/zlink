#!/usr/bin/env python3
import pathlib,sys,statistics
from collections import Counter
p=pathlib.Path(sys.argv[1]); pidS=sys.argv[2]; pidT=sys.argv[3]; out=pathlib.Path(sys.argv[4]); fd_st,fd_tr,fd_tt,fd_sr=sys.argv[5:9]
roles=[]; counts=Counter()
for l in p.open():
 x=l.rstrip('\n').split('\t'); pid,tid,nr,fd,ent,ext,ret,length=x[:8]
 nr=int(nr); rv=int(ret); ln=int(length,16) if length else -1
 role=None
 if pid==pidS and nr==44 and fd==fd_st and 1000<=rv<=1500: role='S_tx'; counts[role]+=1
 elif pid==pidT and nr==45 and fd==fd_tr and 1000<=rv<=1500: role='T_rx'; counts[role]+=1
 elif pid==pidT and nr==44 and fd==fd_tt and 1000<=rv<=1500: role='T_tx'; counts[role]+=1
 elif pid==pidS and nr==45 and fd==fd_sr and 1000<=rv<=1500: role='S_rx'; counts[role]+=1
 if role: roles.append((float(ext),role,rv,int(tid)))
roles.sort()
stages=['S_tx','T_rx','T_tx','S_rx','S_tx']; cycles=[]; stage=0; cur=[]; broken=0; leading=0
for row in roles:
 t,r,n,tid=row
 if stage==0:
  if r=='S_tx': cur=[row]; stage=1
  else: leading+=1
  continue
 want=stages[stage]
 ok=r==want
 if ok and stage==2: ok=(cur[1][2] >= 1000 and cur[1][2] <= 1500)
 if ok and stage==3: ok=(n==cur[2][2])
 if ok and stage==1: ok=(n==cur[0][2])
 if ok:
  cur.append(row); stage+=1
  if stage==5:
   cyc=cur
   cycles.append({'times':[x[0] for x in cyc], 'tids':[x[3] for x in cyc], 'bytes':[x[2] for x in cyc]})
   cur=[]; stage=0
 else:
  broken+=1
  cur=[]; stage=0
  if r=='S_tx': cur=[row]; stage=1
if cur: broken+=1

def q(v,p):
 a=sorted(v);
 if not a: return float('nan')
 z=(len(a)-1)*p; lo=int(z); hi=min(lo+1,len(a)-1); return a[lo]+(a[hi]-a[lo])*(z-lo)
segments=['A','B','C','D']; vals={k:[] for k in segments}
for c in cycles:
 ts=c['times']
 for i,k in enumerate(segments): vals[k].append((ts[i+1]-ts[i])*1e6)
summary={k:{'n':len(v),'p50':q(v,.5),'p90':q(v,.9),'mean':statistics.fmean(v) if v else float('nan')} for k,v in vals.items()}
out.write_text('\n'.join(f"{','.join(map(str,c['times']))}\t{','.join(map(str,c['tids']))}\t{','.join(map(str,c['bytes']))}" for c in cycles))
print('valid_calls',dict(counts),'leading',leading,'broken',broken,'cycles',len(cycles),'summary',summary,'cycle_mean_us',sum(sum(v) for v in vals.values())/len(cycles) if cycles else None,'1/mean-cycle throughput us from calls',sum(c['times'][-1]-c['times'][0] for c in cycles)/len(cycles)*1e6 if cycles else None,'cycle_file',out)
