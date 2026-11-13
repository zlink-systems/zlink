import sys,pathlib,json,math,statistics,collections
from counter_io import quantile
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/xbp')
def load(path):return json.loads(path.read_text())
def percentile(v,p):
    return sorted(v)[max(0,math.ceil(len(v)*p)-1)] if v else None

def validate(row):
    for key in ['underflow','overflow','unexpected_retry','completion_error']:
        assert row[key]==0,(key,row[key])
    assert row['requests']==row['initial']+row['resubmits']
    assert row['bp']==row['initial_bp']+row['retry_bp']
    assert row['resubmits']==row['retry_ok']+row['retry_bp']+row['retry_other']
    assert row['wakes']==row['wake_progress']+row['wake_no_progress']+row['wake_terminal']+row['wake_unmatched'],('wakes',row['wakes'],row['wake_progress'],row['wake_no_progress'],row['wake_terminal'],row['wake_unmatched'])
    assert sum(row['delay'].values())==row['delay_n']
    assert sum(row['wake_delay'].values())==row['wake_delay_n']
    assert row['snd_rc']==row['rcv_rc']==row['monitor_rc']==0 and row['hwm_reads']>0 and row['monitor_abi']==4 and row['monitor_detail']&16
    assert row['completed']>0

rows=[]
for d in sorted((J/'raw').glob('*-r[123]')):
    if not (d/'validation.json').exists():continue
    lang,kind,payload,mode,run=d.name.split('-');row={'language':lang,'kind':kind,'payload':int(payload),'mode':mode,'run':int(run[1:]),'directory':str(d)}
    c=load(d/'results.json')['cells'][0];row['rate']=c['throughput_per_second'];row['cell']=c;row['validation']=load(d/'validation.json');row['window']=load(d/'window.json')
    if mode=='count':
        data=load(d/'source-counter.json');source=max(data['stats'],key=lambda x:x['requests']);target=max(load(d/'target-counter.json')['stats'],key=lambda x:x['reply_calls'])
        validate(source)
        assert target['hwm_reads']>0 and target['monitor_abi']==4 and target['monitor_detail']&16 and target['monitor_rc']==0,target
        samples=[json.loads(line) for line in (d/'samples.jsonl').read_text().splitlines()];app=[x['stats'].get('currentInFlight',x['stats'].get('inFlight')) for x in samples]
        native=load(d/'native-samples.json');logical=[];admitted=[]
        for x in native:
            for z in x['stats']:
                if z['socket']==source['socket']:logical.append(z['logical']);admitted.append(z['admitted_live'])
        row.update(counter=source,targetCounter=target,appSamples=app,nativeLogicalSamples=logical,nativeAdmittedSamples=admitted,sampleIntervalsUs=[(b['ns']-a['ns'])/1000 for a,b in zip(samples,samples[1:])])
        row['counterCoverage']=source['completed']/row['validation']['windowBenchmarkCompleted']
        assert .95<=row['counterCoverage']<=1.05,('native/bench completion coverage',row['counterCoverage'],d)
    rows.append(row)
(J/'summary-runs.json').write_text(json.dumps(rows,indent=2)+'\n')
agg=[]
for language in ['cpp','dotnet','java']:
    for kind in ['framework','binding']:
        for payload in [1024,4096]:
            cells=[r for r in rows if (r['language'],r['kind'],r['payload'])==(language,kind,payload)]
            plain=[r for r in cells if r['mode']=='plain'];count=[r for r in cells if r['mode']=='count']
            if len(plain)!=3 or len(count)!=3:continue
            completed=sum(r['counter']['completed'] for r in count)
            counters={k:sum(r['counter'][k] for r in count) for k in ['requests','initial','bp','initial_bp','admitted','resubmits','retry_ok','retry_bp','retry_other','wakes','wake_progress','wake_no_progress','wake_terminal','wake_unmatched','completed','delay_n','wake_delay_n']}
            per1k={k:v/completed*1000 for k,v in counters.items()}
            h=collections.Counter();w=collections.Counter()
            for r in count:h.update(r['counter']['delay']);w.update(r['counter']['wake_delay'])
            app=sum((r['appSamples'] for r in count),[]);native=sum((r['nativeLogicalSamples'] for r in count),[]);admitted=sum((r['nativeAdmittedSamples'] for r in count),[])
            entry={'language':language,'kind':kind,'payload':payload,'plainMedian':statistics.median(r['rate'] for r in plain),'countMedian':statistics.median(r['rate'] for r in count),'completed':completed,'counts':counters,'per1000':per1k,'bpDelayP50Us':quantile(h,.5),'bpDelayP99Us':quantile(h,.99),'wakeDelayP50Us':quantile(w,.5),'wakeDelayP99Us':quantile(w,.99),'appFlight':[percentile(app,.5),percentile(app,.99),max(app)],'nativeFlight':[percentile(native,.5),percentile(native,.99),max(native)],'nativeAdmittedFlight':[percentile(admitted,.5),percentile(admitted,.99),max(admitted)],'samples':len(app),'hwm':[[r['counter']['sndhwm'],r['counter']['rcvhwm'],r['targetCounter']['sndhwm'],r['targetCounter']['rcvhwm']] for r in count],'coverage':[r['counterCoverage'] for r in count]}
            entry['hwmRanges']=[[min(r[side][lo] for r in count),max(r[side][hi] for r in count)] for side,lo,hi in [('counter','hwm_min_snd','hwm_max_snd'),('counter','hwm_min_rcv','hwm_max_rcv'),('targetCounter','hwm_min_snd','hwm_max_snd'),('targetCounter','hwm_min_rcv','hwm_max_rcv')]]
            entry['countVsPlainPercent']=(entry['countMedian']/entry['plainMedian']-1)*100
            agg.append(entry)
(J/'summary.json').write_text(json.dumps(agg,indent=2)+'\n')
for a in agg:print(a['language'],a['kind'],a['payload'],'plain',round(a['plainMedian']),'count',round(a['countMedian']),'BP/1k',round(a['per1000']['bp'],3),'flight',a['appFlight'],'HWM',a['hwm'])
if '--require-all' in sys.argv:assert len(agg)==12 and len(rows)==72,(len(agg),len(rows))
