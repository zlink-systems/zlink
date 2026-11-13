from pathlib import Path
import collections,sys
ns={'__name__':'inspect'}
src=Path('/root/jobs/fx/cx/v21/s033/salloc/analyze.py').read_text();exec(src[:src.index('\ncell=')],ns)
cell=Path(sys.argv[1])/'run1'/'zlink-framework-cpp-request-backpressure-1024'
samples=ns['parse_samples'](cell/'capture/source.samples.bin')
maps,sym=ns['symbolize'](cell/'source.maps',samples)
samples=ns['classify'](samples,maps,sym)
import json
j=json.loads((cell/'capture/source.alloc-delta.json').read_text());tid=max(j['slots'],key=lambda k:sum(j['slots'][k][c] for c in ('malloc','calloc','realloc')))
for key in ('submit_request_reply_message_erased','raw_route_port_t::request','request_task_access_t::receive'):
 rows=[s for s in samples if s['tid']==int(tid) and key in s['site']]
 sizes=collections.Counter(s['size'] for s in rows);print('\nSITE',key,'sizes',sizes)
 for size,n in sizes.most_common(6):
  r=next(s for s in rows if s['size']==size);print('BYTES',size,'SAMPLES',n)
  for ip in r['ips']:
   m=next((m for m in maps if m[0]<=ip<m[1]),None)
   if m:
    fn,loc=sym.get((m[3],ip-m[0]+m[2]),('??','??'));print(' ',fn[:220],loc[:100])
