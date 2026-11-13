from pathlib import Path
import subprocess
P=Path('/root/jobs/fx/cx/v21/s033/jenv')
s=(P/'runner/run-counter.py').read_text()
s=s.replace("('main','binding','main-counters','binding-counters')", "('main','binding','main-counters','binding-counters','main-causes','binding-causes')")
s=s.replace("a.case=='main-counters'", "a.case in ('main-counters','main-causes')")
s=s.replace("str(out/'capture-bpf')", "str(out/'capture')")
s=s.replace("collect('bpf')", "collect('perf')")
(P/'runner/run-cause.py').write_text(s)
for c in ('main-causes','prototype-causes','binding-causes'):
 print('CAUSE',c,'rate 50000',flush=True)
 subprocess.run(['python3',str(P/'runner/run-cause.py'),c,'--rate','50000'],check=True)
for c in ('main-causes','prototype-causes','binding-causes'):
 subprocess.run(['python3',str(P/'tools/wire_pcap.py'),str(P/'runs'/c/'wire.pcap')],check=True)
s=(P/'tools/summarize_perf.py').read_text().replace("('main','prototype','prototype-unpaced','binding')", "('main-causes','prototype-causes','binding-causes')").replace("J/'perf-summary.json'", "J/'cause-perf-summary.json'")
(P/'tools/summarize_cause_perf.py').write_text(s)
subprocess.run(['python3',str(P/'tools/summarize_cause_perf.py')],check=True)
subprocess.run(['python3',str(P/'tools/finish_causes.py')],check=True)
