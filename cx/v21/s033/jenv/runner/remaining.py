import json,pathlib,subprocess
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/jenv');rate=round(json.load(open(P/'runs/main/results.json'))['cells'][0]['throughput_per_second'])
for case,pace,smoke in [('prototype-smoke',0,True),('prototype-unpaced',0,False),('prototype',rate,False),('binding',rate,False)]:
 cmd=['python3',str(P/'runner/run.py'),case]
 if pace:cmd+=['--rate',str(pace)]
 if smoke:cmd+=['--smoke']
 print('NEXT',case,'rate',pace,flush=True);subprocess.run(cmd,check=True)
# Process captures before releasing this matrix's lock, so another long run need not delay analysis.
for case in ('main','prototype-unpaced','prototype','binding'):
 subprocess.run(['python3',str(P/'tools/wire_pcap.py'),str(P/'runs'/case/'wire.pcap')],check=True)
for tool,log in [('summarize_perf.py','perf-summary.log'),('summarize_bpf.py','bpf-summary.log'),('summarize_extra.py','extra-summary.log')]:
 with (P/log).open('w') as f:subprocess.run(['python3',str(P/'tools'/tool)],stdout=f,stderr=subprocess.STDOUT,check=True)
print('capture processing finished',flush=True)
