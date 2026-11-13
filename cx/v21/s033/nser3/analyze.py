import bisect, collections, json, pathlib, re, statistics
base = pathlib.Path('/root/jobs/fx/cx/v21/s033/nser3/traced-path')
line_re = re.compile(r'^\s*(.*?)\s+(\d+)/(\d+)\s+\[(\d+)\]\s+([0-9.]+):\s+(.*)$')
ent_re = re.compile(r'syscalls:sys_enter_(sendto|recvfrom):')
ext_re = re.compile(r'syscalls:sys_exit_(sendto|recvfrom): (-?(?:0x[0-9a-fA-F]+|\d+))')
sw_re = re.compile(r'\s*(.*?):(\d+) \[[^]]+\] (\S+) ==> (.*?):(\d+) \[')

def stat(values):
    a=sorted(values)
    if not a:return {'n':0}
    def q(p):
        x=(len(a)-1)*p;lo=int(x);hi=min(lo+1,len(a)-1)
        return round(a[lo]+(a[hi]-a[lo])*(x-lo),3)
    return {'n':len(a),'p50':q(.5),'p90':q(.9),'mean':round(statistics.fmean(a),3)}
def read_jsonl(path):
    if not path.exists(): return []
    rows=[]
    for l in path.read_text().splitlines():
        r=json.loads(l)
        if 'Timestamp' in r:r['t']=r['Timestamp']/1e9
        rows.append(r)
    return rows
def callmark(c,key):
    return {'t':c[key], 'ThreadId':c['tid'], 'ThreadName':c['comm']}
def capture(cell):
    folder=base/cell
    meta=json.loads((folder/'capture.json').read_text()); sp,tp=meta['pids']['source'],meta['pids']['target']
    sb=read_jsonl(folder/f'trace.{sp}.bench.jsonl')
    tb=read_jsonl(folder/f'trace.{tp}.bench.jsonl')
    sr=read_jsonl(folder/f'trace.{sp}.jsonl')
    tr=read_jsonl(folder/f'trace.{tp}.jsonl')
    impl='zlink-framework-dotnet' if cell.startswith('zlink-framework') else 'zlink-dotnet'
    payload=int(cell.rsplit('-',1)[1]); framework=impl.startswith('zlink-framework')
    # syscalls and scheduling switches during the perf window
    pending={};calls=[];switches=[]
    for line in (folder/'perf.script').read_text(errors='replace').splitlines():
        if 'syscalls:sys_enter_' not in line and 'syscalls:sys_exit_' not in line: continue
        m=line_re.match(line)
        if not m:continue
        comm,pid,tid,cpu,ts,event=m.groups();pid=int(pid);tid=int(tid);cpu=int(cpu);ts=float(ts)
        if pid not in (sp,tp):continue
        m=ent_re.search(event)
        if m:
            name=m.group(1);pending[pid,tid,name]=(ts,comm,cpu);continue
        m=ext_re.search(event)
        if m:
            name,ret=m.group(1),int(m.group(2),0);p=pending.pop((pid,tid,name),None)
            if p:
                calls.append({'pid':pid,'tid':tid,'nr':44 if name=='sendto' else 45,'ret':ret,'start':p[0],'end':ts,'comm':p[1]})
            continue
    # x86-64 syscall numbers: sendto 44 and recvfrom 45; payload frames are above 1 KiB.
    _times_cache={}
    def after(rows, field, t):
        key=(id(rows),field)
        times=_times_cache.get(key)
        if times is None:
            times=[r[field] for r in rows];_times_cache[key]=times
        i=bisect.bisect_right(times,t)
        return rows[i] if i<len(rows) else None
    sends={pid:sorted([c for c in calls if c['pid']==pid and c['nr']==44 and c['ret']>=payload],key=lambda x:x['start']) for pid in (sp,tp)}
    recvs={pid:sorted([c for c in calls if c['pid']==pid and c['nr']==45 and c['ret']>=payload],key=lambda x:x['end']) for pid in (sp,tp)}
    def groups(rows,flow=None,stage=None):
        return sorted([r for r in rows if (flow is None or r.get('Flow')==flow) and (stage is None or r.get('Stage')==stage)],key=lambda r:r['t'])
    if framework:
        B={s:{r['Serial']:r for r in groups(tr,'B',s)} for s in range(1,5)}
        B1times=sorted(B[1].values(),key=lambda r:r['t'])
        D1times=sorted(groups(sr,'D',1),key=lambda r:r['t'])
        D={s:{r['Serial']:r for r in groups(sr,'D',s)} for s in range(1,4)}
        C={s:groups(sr,'C',s) for s in range(1,6)}
        d4=groups(sb,'D',4)
        cycles=[]
        for i,start in enumerate(d4[:-1]):
            end=d4[i+1]
            send=after(sends[sp],'start',start['t'])
            if not send or send['start']>=end['t']:continue
            recv=after(recvs[tp],'end',send['start'])
            if not recv:continue
            b1=after(B1times,'t',recv['end'])
            if not b1:continue
            serial=b1['Serial'];b2=B[2].get(serial);b3=B[3].get(serial);b4=B[4].get(serial)
            if not all((b2,b3,b4)):continue
            tsend=after(sends[tp],'start',b4['t'])
            if not tsend:continue
            srecv=after(recvs[sp],'end',tsend['end'])
            if not srecv:continue
            d1=after(D1times,'t',srecv['end'])
            if not d1:continue
            ds=d1['Serial'];d2=D[2].get(ds);d3=D[3].get(ds)
            if not all((d2,d3)) or end['t']<=d3['t']:continue
            cseq={s:after(C[s],'t',start['t']) for s in range(1,6)}
            cseq={s:(r if r and r['t']<send['start'] else None) for s,r in cseq.items()}
            # C markers occur on the same request. C5 may be after C4 and before the matching send.
            seq=[start,cseq[1],cseq[2],cseq[3],cseq[4],cseq[5],callmark(send,'start'),callmark(recv,'end'),b1,b2,b3,b4,callmark(tsend,'start'),callmark(srecv,'end'),d1,d2,d3,end]
            if any(x is None for x in seq):continue
            if not all(seq[j]['t']<seq[j+1]['t'] for j in range(17)):continue
            cycles.append(seq)
        labels=['bench→Framework call','Framework call→typed encode done','typed encode→service wire encode done','service wire encode→native submit','native submit→source sendto','source sendto→target recvfrom','target recvfrom→ProcessReceived','receive parse→mailbox enqueue','mailbox enqueue→Node route dispatch','Node dispatch/handler/reply encode','reply submit→target sendto','target sendto→source recvfrom','source recvfrom→physical reply observed','reply decode→completion queue','completion queue→worker execute','completion execute→caller/next request']
        idx=[(0,1),(1,2),(2,3),(3,4),(4,6),(6,7),(7,8),(8,9),(9,10),(10,11),(11,12),(12,13),(13,14),(14,15),(15,16),(16,17)]
    else:
        B={s:{r['Serial']:r for r in groups(tb,'B',s)} for s in (1,4)}
        B1times=sorted(B[1].values(),key=lambda r:r['t'])
        S={s:groups(sb,'S',s) for s in (1,2,3,4)}
        d4=groups(sb,'D',4);cycles=[]
        for i,start in enumerate(d4[:-1]):
            end=d4[i+1]
            s2=after(S[2],'t',start['t'])
            s3=after(S[3],'t',s2['t']) if s2 else None
            s4=after(S[4],'t',s3['t']) if s3 else None
            send=after(sends[sp],'start',start['t'])
            recv=after(recvs[tp],'end',send['start']) if send else None
            b1=after(B1times,'t',recv['end']) if recv else None
            b4=B[4].get(b1['Serial']) if b1 else None
            tsend=after(sends[tp],'start',b4['t']) if b4 else None
            srecv=after(recvs[sp],'end',tsend['end']) if tsend else None
            s1=after(S[1],'t',srecv['end']) if srecv else None
            if not all((s2,s3,s4,send,recv,b1,b4,tsend,srecv,s1)) or end['t']<=s1['t']:continue
            seq=[start,s2,s3,s4,callmark(send,'start'),callmark(recv,'end'),b1,b4,callmark(tsend,'start'),callmark(srecv,'end'),s1,end]
            if all(seq[j]['t']<seq[j+1]['t'] for j in range(len(seq)-1)):cycles.append(seq)
        labels=['bench→raw request setup','raw setup→body encode done','body encode→binding submit','binding submit→source sendto','source sendto→target recvfrom','target recvfrom→handler start','binding handler decode/echo→reply call','reply call→target sendto','target sendto→source recvfrom','source recvfrom→binding completion/typed decode','completion→caller/next request']
        idx=[(0,1),(1,2),(2,3),(3,4),(4,5),(5,6),(6,7),(7,8),(8,9),(9,10),(10,11)]
    segments={}
    for name,(a,b) in zip(labels,idx):
        vals=[(x[b]['t']-x[a]['t'])*1e6 for x in cycles]
        alloc=[x[b]['AllocatedBytes']-x[a]['AllocatedBytes'] for x in cycles if 'AllocatedBytes' in x[a] and 'AllocatedBytes' in x[b] and x[a].get('ThreadId') == x[b].get('ThreadId')]
        handoffs=sum(1 for x in cycles if x[a].get('ThreadId') is not None and x[b].get('ThreadId') is not None and x[a]['ThreadId']!=x[b]['ThreadId'])
        segments[name]={'time_us':stat(vals),'alloc_bytes':stat(alloc),'thread_handoff_count':handoffs,'n':len(vals)}
    sums=[]
    for x in cycles:
        sums.append((x[-1]['t']-x[0]['t'])*1e6)
    # From syscall data, report managed TIDs at send/receive points for executor mapping.
    summary={'cell':cell,'implementation':impl,'payload':payload,'complete_cycles':len(cycles),'cycle_wall_us':stat(sums),'segments':segments,
             'syscalls':{f'source_sendto':len(sends[sp]),'target_recvfrom':len(recvs[tp]),'target_sendto':len(sends[tp]),'source_recvfrom':len(recvs[sp])},
             'markers':{'source_bench':len(sb),'target_bench':len(tb),'source_runtime':len(sr),'target_runtime':len(tr)},
             'alloc_method':'GC.GetAllocatedBytesForCurrentThread snapshots; segment delta is reported only when endpoint markers run on the same managed thread.',
             'handoff_method':'different managed ThreadId at segment endpoint markers; syscall segments report TIDs separately in report.'}
    return summary,cycles,calls,switches

all_summaries=[]
for impl in ('zlink-framework-dotnet','zlink-dotnet'):
    for payload in (1024,4096):
        cell=f'{impl}-request-serial-{payload}'
        s,cycles,calls,switches=capture(cell);all_summaries.append(s)
        print('\n',cell,'cycles',s['complete_cycles'],'cycle wall',s['cycle_wall_us'],'syscalls',s['syscalls'])
        for k,v in s['segments'].items():print(k,v)
(base/'segment-summary.json').write_text(json.dumps(all_summaries,indent=2)+'\n')
