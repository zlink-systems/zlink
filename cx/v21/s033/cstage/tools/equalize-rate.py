from pathlib import Path
import json,shutil,subprocess,sys
j=Path('/root/jobs/fx/cx/v21/s033/cstage');runner=j/'tools/measure.py'
initial=json.loads((j/'summary.json').read_text());reference=initial['cpu']['staged']['full_60s_rate'];input_rate=round(105000*reference/initial['cpu']['main']['full_60s_rate'])
shutil.copyfile(j/'summary.json',j/'summary-initial-rate-mismatch.json');shutil.copyfile(j/'common-rate.json',j/'common-rate-initial.json')
old=j/'runs/cpu-initial-mismatch/main';old.parent.mkdir(parents=True,exist_ok=True);assert not old.exists();shutil.move(j/'runs/cpu/main',old)
records=[]
def invoke(tag,active,cpu):
 cmd=['python3',str(runner),'--variants','main','--runs','1','--warmup','30','--active',str(active),'--tag',tag,'--rate',str(input_rate)]
 if cpu:cmd+=['--cpu']
 subprocess.run(cmd,check=True)
 p=j/'runs'/tag/'main/run1/results.json';c=json.loads(p.read_text())['cells'][0]
 actual=c['throughput_per_second'];record={'tag':tag,'input_rate':input_rate,'actual_rate':actual,'reference_staged_rate':reference,'relative_difference':abs(actual-reference)/reference};records.append(record)
 (j/'rate-calibration.json').write_text(json.dumps(records,indent=2));print(record,flush=True);return actual
for n in range(1,4):
 actual=invoke('rate-calibration-'+str(n),5,False)
 if abs(actual-reference)/reference<.003:break
 input_rate=round(input_rate*reference/actual)
else:raise RuntimeError('main pacing calibration did not converge')
for n in range(1,4):
 actual=invoke('cpu',60,True)
 subprocess.run(['python3',str(j/'tools/summarize.py')],check=True)
 x=json.loads((j/'summary.json').read_text());central=x['same_rate']['relative_rate_difference'];full=abs(actual-reference)/max(actual,reference)
 if central<.005 and full<.005:
  policy=json.loads((j/'common-rate-initial.json').read_text());policy.update(common_rate=round(reference),main_input_rate=input_rate,staged_input_rate=105000,main_actual_rate=actual,staged_actual_rate=reference,relative_actual_difference=full,policy='main pacing input calibrated to the already measured staged 60s actual rate; both full and central-window rates differ by less than 0.5%; initial equal-input pair excluded')
  (j/'common-rate.json').write_text(json.dumps(policy,indent=2));print('SAME_RATE_VERIFIED',policy,flush=True);break
 bad=j/'runs'/('cpu-main-rate-mismatch-'+str(n))/'main';bad.parent.mkdir(parents=True,exist_ok=True);shutil.move(j/'runs/cpu/main',bad)
 shutil.copyfile(j/'summary.json',j/('summary-rate-mismatch-'+str(n)+'.json'))
 input_rate=round(input_rate*reference/actual)
else:raise RuntimeError('60s main/staged actual rates did not match within 0.5%')
