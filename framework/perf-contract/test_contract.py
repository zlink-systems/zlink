import bisect
import copy
import json
import tempfile
import unittest
from pathlib import Path
from results import *
from validator import validate_snapshot, validate_result, validate_schema


def histogram(samples):
    h = {'unit':'ms','ticksUnit':'ns','bucketSpec':'ns-1us-1pct-60s-v1','boundsNs':list(map(str, BOUNDS)),
         'counts':['0']*len(BOUNDS),'overflow':'0','count':str(len(samples)),'sumNs':str(sum(samples)),
         'maxNs':str(max(samples)) if samples else None,'percentileMethod':'nearest-rank-bucket-upper-bound'}
    for sample in samples:
        index = bisect.bisect_left(BOUNDS, sample)
        if index == len(BOUNDS): h['overflow']=str(int(h['overflow'])+1)
        else: h['counts'][index]=str(int(h['counts'][index])+1)
    return h


def snapshot(role='channel', instance=0, seconds=1, samples=(1000,), mode='request'):
    metrics = metric_defaults()
    nulls = {'/metrics/'+key:reason('NOT_APPLICABLE','Synthetic fixture has no such observation.') for key,value in metrics.items() if value is None}
    for key in OUTCOMES: metrics['messages.'+key]='0'
    metrics['messages.sent']=metrics['messages.completed']=str(len(samples))
    metrics['slo.eligible']=metrics['slo.met']=str(len(samples));metrics['slo.missed']='0'
    metrics['slo.missRatio']=0;metrics['slo.goodputOpsPerSec']=len(samples)/seconds
    metrics['load.lateWarmupMessages']='0'
    for family in ('applicationMessages','applicationPayloadBytes'):
        for direction in ('request','send','reply','event'): metrics[family+'.'+direction]='0'
    metrics['throughput.kops']=len(samples)/seconds/1000;metrics['throughput.messagesPerSec']=0;metrics['throughput.megabytesPerSec']=0
    histograms={key:None for key in CATALOG['histogramPrefixes']}
    for key in histograms: nulls['/histograms/'+key]=reason('NOT_APPLICABLE','No fixture interval.')
    histograms['latencyMs']=histogram(samples);histograms['settleLatencyMs']=histogram([])
    export_latency(histograms['latencyMs'],'latency','latencyMs',metrics,nulls)
    export_latency(histograms['settleLatencyMs'],'settle.latency','settleLatencyMs',metrics,nulls)
    for key,value in metrics.items():
        if value is not None: nulls.pop('/metrics/'+key,None)
    for key,value in histograms.items():
        if value is not None: nulls.pop('/histograms/'+key,None)
    clock={'source':'test-monotonic','nativeFrequencyHz':'1000000000','ticksUnit':'ns','clockDomainId':'fixture-'+str(instance),
           'scope':'process','alignmentMethod':None,'maxErrorNs':None,'validFromTicks':None,'validThroughTicks':None,'evidence':[]}
    for key,value in clock.items():
        if value is None: nulls['/clock/'+key]=reason('NOT_APPLICABLE','Process-local fixture clock.')
    nulls['/publicStatus']=reason('NOT_APPLICABLE','Fixture has no public host.')
    bins=[];remaining=seconds*1000;offset=0
    while remaining>1e-8:
        duration=min(100,remaining)
        bins.append({'offsetMs':offset,'durationMs':duration,'counts':{key:str(len(samples)) if offset==0 else '0' for key in ('messages.sent','messages.completed','slo.eligible')},'cpuPercent':None,
                     'nullReasons':{'/cpuPercent':reason('NO_SAMPLES','No actual sample starts in this fixture bin.')}})
        offset+=duration;remaining-=duration
    return {'schemaVersion':3,'runId':'test','cellId':'cell','resetSeq':'1','language':'dotnet','role':role,'roleInstance':instance,
            'configHash':'config','comparisonKey':'comparison','phase':'complete','window':{'startedAtUnixMs':'1','endedAtUnixMs':str(1+int(seconds*1000)),
            'startTicks':'100','endTicks':str(100+round(seconds*1e9)),'measuredSeconds':seconds,'settleSeconds':0},'clock':clock,
            'serializedMessageBytes':[],'metrics':metrics,'histograms':histograms,'nullReasons':nulls,'publicStatus':None,'publicMetrics':[],
            'timeSeries':bins,'runtimeMetrics':{'cpuSamples':{'name':'fixture actual CPU spans','unit':'observation','type':'array','value':[]}},'provenance':{'pid':123+instance}}


def one_way_snapshot(role='channel', instance=0, seconds=1, publisher=False, source=False):
    value=snapshot(role,instance,seconds)
    for key in ('messages.sent','messages.completed','messages.settleCompleted','slo.eligible','slo.met','slo.missed','slo.missRatio','slo.goodputOpsPerSec','throughput.kops'):
        value['metrics'][key]=None
        value['nullReasons']['/metrics/'+key]=reason('CLOCK_DOMAIN_UNVERIFIED' if key.startswith('slo.') else 'NOT_APPLICABLE','No roundtrip observation.')
    for hist,prefix in (('latencyMs','latency'),('settleLatencyMs','settle.latency')):
        value['histograms'][hist]=None;value['nullReasons']['/histograms/'+hist]=reason('NOT_APPLICABLE','No roundtrip.')
        for suffix in CATALOG['latencySuffixes']:
            value['metrics'][prefix+'.'+suffix]=None;value['nullReasons']['/metrics/'+prefix+'.'+suffix]=reason('NOT_APPLICABLE','No roundtrip.')
    if source:
        value['metrics']['messages.sent']='4';value['nullReasons'].pop('/metrics/messages.sent',None)
        value['timeSeries'][0]['counts']['messages.sent']='4'
        value['metrics']['messages.published' if publisher else 'messages.admitted']='4'
        if publisher:
            value['metrics']['messages.publishedInWindow']='3';value['metrics']['messages.settlePublished']='1'
    for key,item in value['metrics'].items():
        if item is not None:value['nullReasons'].pop('/metrics/'+key,None)
    return value


class ContractTests(unittest.TestCase):
    def test_histogram_fixture_and_big_sum(self):
        fixture=json.loads((HERE/'fixtures/aggregation.json').read_text())
        h=histogram(list(map(int,fixture['histogramSamplesNs'])))
        self.assertEqual(h['overflow'],fixture['expectedOverflow'])
        merged=histogram_merge([h,h]);self.assertEqual(merged['count'],'12')
        self.assertEqual(int(merged['sumNs']),2*int(h['sumNs']))
        metrics={};nulls={};export_latency(h,'latency','latencyMs',metrics,nulls)
        self.assertIsNone(metrics['latency.p999Ms']);self.assertEqual(nulls['/metrics/latency.p999Ms']['code'],'INSUFFICIENT_SAMPLES')
        self.assertIsNone(metrics['latency.p99Ms']);self.assertEqual(nulls['/metrics/latency.p99Ms']['lowerBoundMs'],60000)
        h['sumNs']='18446744073709551616';h['count']='1000000';h['overflow']='999995'
        h['maxNs']=str(MAX_U64)
        self.assertGreater(int(histogram_merge([h])['sumNs']),MAX_U64)

    def test_histogram_rejects_contract_and_counts(self):
        for mutate in (lambda h:h['counts'].pop(),lambda h:h.update(bucketSpec='old'),lambda h:h.update(count='2'),lambda h:h.update(maxNs=None)):
            h=histogram([1]);mutate(h)
            with self.assertRaises(ValueError): histogram_merge([h])

    def test_range_fixture_and_u64_edge(self):
        f=json.loads((HERE/'fixtures/aggregation.json').read_text())['ranges']
        self.assertEqual(str(intersection_count(f['source'],f['receiverWindow'])),f['expectedWindow'])
        self.assertEqual(str(intersection_count(f['source'],f['receiverSettle'])),f['expectedSettle'])
        self.assertEqual(range_count([{'first':str(MAX_U64),'last':str(MAX_U64)}]),1)
        for invalid in ([{'first':'01','last':'2'}],[{'first':'2','last':'1'}],[{'first':'1','last':'2'},{'first':'3','last':'4'}], [['1','2']]):
            with self.assertRaises(ValueError): ranges(invalid)

    def test_schema_valid_and_required_field_rejection(self):
        value=snapshot(seconds=1.000000001)
        validate_snapshot(value)
        for field in list(value):
            broken=copy.deepcopy(value);del broken[field]
            with self.subTest(field=field), self.assertRaises(ValueError): validate_snapshot(broken)
        broken=copy.deepcopy(value);broken['unexpected']=1
        with self.assertRaises(ValueError): validate_snapshot(broken)

    def test_schema_dto_payload(self):
        payload=json.loads((HERE/'fixtures/payload-64.json').read_text())
        request={'runId':'test','cellId':'cell','resetSeq':'1','phase':'measured','clientId':0,'sequence':str(MAX_U64),
                 'correlationId':'x','scheduledTicks':None,'sentTicks':'1','clockDomainId':'test','returnSpotId':None,'returnChannel':None,'payload':payload['payload']}
        validate_schema(request,'PerfEchoRequest')
        del request['scheduledTicks']
        with self.assertRaises(ValueError): validate_schema(request,'PerfEchoRequest')

    def test_signed_clock_epochs_and_exact_dto_bounds(self):
        from validator import i64
        self.assertEqual(i64(str(-(1<<63))), -(1<<63))
        self.assertEqual(i64(str((1<<63)-1)), (1<<63)-1)
        for text in ('-0', '+1', '01', str(1<<63), str(-(1<<63)-1)):
            with self.assertRaises(ValueError):i64(text)
        value=snapshot();value['window']['startTicks']='-1000000000';value['window']['endTicks']='0'
        validate_snapshot(value)
        value['clock']['validFromTicks']=str(-(1<<63)-1)
        with self.assertRaises(ValueError):validate_snapshot(value)

    def test_time_series_count_sum_and_counter_overflow(self):
        value=snapshot();value['timeSeries'][0]['counts']['messages.sent']='0'
        with self.assertRaises(ValueError):validate_snapshot(value)
        h=histogram([1000]);h['count']=h['counts'][0]=str(MAX_U64);h['sumNs']=str(MAX_U64*1000)
        with self.assertRaises(ValueError):histogram_merge([h,h])

    def test_null_reason_window_and_dense_bins(self):
        for mutation in (lambda s:s['nullReasons'].pop('/clock/maxErrorNs'),lambda s:s['window'].update(endTicks='101'),
                         lambda s:s['timeSeries'].pop(1),lambda s:s['metrics'].update(**{'messages.sent':str(MAX_U64+1)})):
            s=snapshot();mutation(s)
            with self.assertRaises((ValueError,KeyError)): validate_snapshot(s)

    def test_multi_owner_weighted_histogram_and_rates(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder);a=snapshot('client',0,1,[1000]);b=snapshot('client',1,2,[1000000,1000000])
            write_json(p/'client-0.json',a);write_json(p/'client-1.json',b)
            config={'runId':'test','cellId':'cell','configHash':'config','comparisonKey':'comparison','language':'dotnet','scenario':'session-echo-only',
                    'mode':'request','workload':{'subscriberCount':8},'metricOwners':['client-0.json','client-1.json']}
            result=aggregate(p,config,['client-0.json','client-1.json'],[],[])
            self.assertEqual(result['status'],'valid',result['reasons']);validate_result(result)
            self.assertEqual(result['metrics']['throughput.kops'],.002)
            self.assertEqual(result['histograms']['latencyMs']['count'],'3')
            self.assertAlmostEqual(result['metrics']['latency.meanMs'],.667)
            self.assertIsNone(result['measuredSeconds'])
            with self.assertRaises(FileExistsError): write_json(p/'result.json',{})

    def test_one_way_receipt_intersection_uses_receiver_window(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder);source=one_way_snapshot(source=True);receiver=one_way_snapshot(instance=1,seconds=2)
            source['runtimeMetrics']['sourceEvidence']={'value':[{'clientId':0,'attemptedRanges':[{'first':'1','last':'4'}],
                'windowAdmissionRanges':[{'first':'1','last':'3'}],'settleAdmissionRanges':[{'first':'4','last':'4'}]}]}
            receiver['runtimeMetrics']['deliveryEvidence']={'value':[{'clientId':0,'windowRanges':[{'first':'1','last':'2'}],
                'settleRanges':[{'first':'3','last':'4'}],'duplicateCount':'0'}]}
            write_json(p/'source.json',source);write_json(p/'receiver.json',receiver)
            config={'runId':'test','cellId':'cell','configHash':'config','comparisonKey':'comparison','language':'dotnet','scenario':'s2s-channel-to-spot-send',
                'mode':'send','workload':{'subscriberCount':8},'metricOwners':['source.json'],'receiverFiles':['receiver.json']}
            result=aggregate(p,config,[],['source.json','receiver.json'],[])
            self.assertEqual(result['status'],'valid',result['reasons']);validate_result(result)
            self.assertEqual(result['metrics']['send.deliveryRatio'],1)
            self.assertEqual(result['metrics']['send.admissionOpsPerSec'],3)
            self.assertEqual(result['metrics']['send.deliveryOpsPerSec'],1)
            self.assertIsNone(result['metrics']['messages.completed'])

    def test_fanout_ratio_is_minimum_and_settle_publish_excluded(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder);source=one_way_snapshot('publisher',source=True,publisher=True)
            source['runtimeMetrics']['publisherSequences']={'value':{'runId':'test','cellId':'cell','resetSeq':'1','phase':'measured',
                'attemptedRanges':[{'first':'1','last':'4'}],'windowSuccessRanges':[{'first':'1','last':'3'}],'settleSuccessRanges':[{'first':'4','last':'4'}]}}
            files=['publisher.json'];write_json(p/files[0],source)
            receipts=[([{'first':'1','last':'2'}],[{'first':'3','last':'4'}]),([{'first':'1','last':'1'}],[{'first':'2','last':'2'}])]
            for index,(window,settle) in enumerate(receipts):
                receiver=one_way_snapshot('subscriber',index+1)
                receiver['runtimeMetrics']['subscriberSequences']={'value':{'runId':'test','cellId':'cell','resetSeq':'1','phase':'measured','subscriberId':index,
                    'windowRanges':window,'settleRanges':settle,'duplicateEvents':'0','timingEvidence':None,'nullReasons':{'/timingEvidence':reason('CLOCK_DOMAIN_UNVERIFIED','No alignment.')}}}
                files.append('subscriber-'+str(index)+'.json');write_json(p/files[-1],receiver)
            config={'runId':'test','cellId':'cell','configHash':'config','comparisonKey':'comparison','language':'dotnet','scenario':'pubsub-fanout-echo',
                'mode':'publish','workload':{'subscriberCount':2},'metricOwners':[files[0]],'receiverFiles':files[1:],'minDeliveryRatio':.9}
            result=aggregate(p,config,[],files,[])
            self.assertEqual(result['status'],'valid',result['reasons']);validate_result(result)
            self.assertAlmostEqual(result['metrics']['fanout.deliveryRatio'],2/3)
            self.assertEqual(result['metrics']['fanout.uniqueDelivered'],'5')
            self.assertEqual(result['metrics']['fanout.outOfCohortEvents'],'1')
            self.assertFalse(result['baselineEligible'])

    def test_cpu_actual_span_assignment_jitter_empty_partial_and_weighting(self):
        fixture=json.loads((Path(__file__).parent/'fixtures/cpu-samples.json').read_text())
        value=snapshot(seconds=.35)
        value['runtimeMetrics']['cpuSamples']['value']=fixture['samples']
        for item, percent in zip(value['timeSeries'],fixture['expectedCpuPercent']):
            item['cpuPercent']=percent
            if percent is not None:item['nullReasons']={}
        validate_snapshot(value)
        for alter in ('assignment','negative','interpolation','duration','weighted'):
            broken=copy.deepcopy(value)
            if alter=='assignment':broken['runtimeMetrics']['cpuSamples']['value'][2]['binIndex']=0
            elif alter=='negative':broken['runtimeMetrics']['cpuSamples']['value'][0]['cpuDeltaNs']='-1'
            elif alter=='interpolation':broken['timeSeries'][2]['cpuPercent']=25
            elif alter=='duration':broken['runtimeMetrics']['cpuSamples']['value'][0]['observedDurationNs']='0'
            else:broken['timeSeries'][0]['cpuPercent']=25  # unweighted mean of 50% and 0% is incorrect
            with self.assertRaises(ValueError,msg=alter):validate_snapshot(broken)

    def test_complete_window_cannot_be_inferred_from_planned_end_ticks(self):
        broken=snapshot();broken['window']['endedAtUnixMs']=None
        broken['nullReasons']['/window/endedAtUnixMs']=reason('PHASE_NOT_STARTED','Planned deadline is not actual completion.')
        with self.assertRaises(ValueError):validate_snapshot(broken)

    def test_late_warmup_invalid_and_owner_errors_only(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder);a=snapshot();a['metrics']['load.lateWarmupMessages']='1'
            write_json(p/'server-source.json',a)
            config={'runId':'test','cellId':'cell','configHash':'config','comparisonKey':'comparison','language':'dotnet','scenario':'channel-echo-only',
                    'mode':'request','metricOwners':['server-source.json'],'workload':{'subscriberCount':8}}
            r=aggregate(p,config,[],['server-source.json'],[])
            self.assertEqual(r['status'],'invalid');self.assertFalse(r['baselineEligible'])

if __name__=='__main__': unittest.main()
