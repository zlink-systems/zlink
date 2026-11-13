import subprocess, sys, json, time
from pathlib import Path
root=Path(__file__).parent
records=[]
for pattern in ['PAIR','DEALER_DEALER']:
    for pair in range(1,7):
        order=['main','after'] if pair%2 else ['after','main']
        for variant in order:
            label=f'perf-{pattern.lower()}-pair{pair:02d}-{variant}'
            command=['flock','-o','/root/jobs/fx/cx/measure.lock','python3',str(root/'pin_benchmark.py'),variant,pattern,label]
            result=subprocess.run(command)
            records.append({'pattern':pattern,'pair':pair,'variant':variant,'label':label,'exit_code':result.returncode})
            (root/'raw'/'pairs-manifest.json').write_text(json.dumps(records,indent=2))
            if result.returncode:
                sys.exit(result.returncode)
