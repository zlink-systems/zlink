import json,pathlib
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang')
def valid(c,t,pattern):
 assert c.get('errors',0)==c.get('abandoned',0)==t.get('errors',0)==0
 assert c['submitted']==c['completed']
 assert c['completed']==t['received']+((t.get('rejected') or 0) if pattern=='send-saturation' else 0)
 assert c['throughput_per_second']>0
for pattern in ['send-saturation','request-backpressure','request-serial']:
 good={'submitted':100,'completed':100,'throughput_per_second':50,'errors':0,'abandoned':0};target={'received':100,'errors':0}
 valid(good,target,pattern)
 try:valid({**good,'completed':99},target,pattern)
 except AssertionError:pass
 else:raise RuntimeError('bad completion accepted')
 try:valid(good,{**target,'errors':1},pattern)
 except AssertionError:pass
 else:raise RuntimeError('bad target accepted')
print('positive=3 negative=6 all expected results')
for p in sorted((P/'runs').glob('*/*/meta.json')):
 m=json.loads(p.read_text())
 if not m.get('ok') or (p.parent/'clock-invalid.txt').exists():continue
 d=p.parent;c=json.loads((d/'results.json').read_text())['cells'][0];t=json.loads((d/'target-stats.json').read_text());valid(c,t,m['job']['pattern'])
print('saved cells checked')
