from pathlib import Path
p=Path('/root/jobs/fx/cx/v21/s033/nbind/tools/render-report.py');s=p.read_text()
s=s.replace("s=json.loads((R/'per-message-summary.json').read_text());data={(x['mode'],x['level'],x['pattern'],x['label'].split('-')[-1]):x for x in s}", "s=json.loads((R/'per-message-summary.json').read_text());assert len(s)==60\nkey=lambda x:(x['mode'],x['level'],x['pattern'],x['label'].split('-')[-1])\nbase_data={key(x):x for x in s if not x['label'].startswith('permit-')}\npermit_data={key(x):x for x in s if x['label'].startswith('permit-')}\ndata=dict(base_data);data.update({k:v for k,v in permit_data.items() if k[1]=='L2'})\nmatched_data=dict(data);matched_data.update(permit_data)\n(R/'run-selection.json').write_text(json.dumps({'primary':{str(k):v['label'] for k,v in data.items()},'permit_fixed':{str(k):v['label'] for k,v in matched_data.items()},'original_runner':{str(k):v['label'] for k,v in base_data.items()}},indent=2)+'\\n')")
needle="(R/'added-cpu.json').write_text(json.dumps(step_rows,indent=2)+'\\n')"
insert='''
matched_lines=[];matched_steps=[]
for pat in patterns:
 for lo,hi in [('L1','L2'),('L2','L3')]:
  vals={k:[] for k in ['total','JS_runtime','addon']}
  for rep in reps:
   a=matched_data['cpu',lo,pat,rep];b=matched_data['cpu',hi,pat,rep]
   vals['total'].append(sum(b['roles'][r]['cpu']['total_us_per_message']-a['roles'][r]['cpu']['total_us_per_message'] for r in roles))
   for k in ['JS_runtime','addon']:
    vals[k].append(sum(b['roles'][r]['cpu']['split'].get(k,{}).get('us_per_message',0)-a['roles'][r]['cpu']['split'].get(k,{}).get('us_per_message',0) for r in roles))
  matched_lines.append(f"| {'send' if pat=='send-saturation' else 'BP'} {lo}→{hi} | "+' | '.join(two(vals[k]) for k in ['JS_runtime','addon','total'])+' |')
  matched_steps.append(dict(pattern=pat,step=lo+'->'+hi,**vals))
(R/'permit-fixed-added-cpu.json').write_text(json.dumps(matched_steps,indent=2)+'\\n')
'''
s=s.replace(needle,needle+'\n'+insert)
s=s.replace('**조건과 단위.** 1K', '**조건과 단위.** 주 표본은 L1 raw+permit, 요청의 `+`를 적용한 L2 raw+permit+envelope, 명시된 L3 raw+service-wire다. 원래 runner의 L2(no permit)와 L3+permit을 대조군으로 별도 보존했다([선택한 run](raw/run-selection.json)). 1K')
s=s.replace('모든 36 cell/72 process', '주 표본36+대조군24, 총60 cell/120 process')
s=s.replace('HEAD `dc2eef3023`, branch', '`F=framework/languages/node/packages/framework/src`. HEAD `dc2eef3023`, branch')
s=s.replace('`F=framework/languages/node/packages/framework/src`. Caller 경계도', 'Caller 경계도')
# Insert a four-row sensitivity table into the existing multiline report expression.
s=s.replace("- **L1→L2:**", "**permit 고정 대조군.** L3에도 L1/L2와 같은 permit을 유지한 경우의 source+target Δµs다. Target loop 차이를 없애도 JS/V8 증가가 남는다.\\n\\n| 단계 | ΔJS/V8 r1/r2 | Δaddon r1/r2 | Δ전체 r1/r2 |\\n|---|---:|---:|---:|\\n'''+ '\\n'.join(matched_lines)+'''\\n\\n- **L1→L2:**")
s=s.replace('공유 CPU와 1s warmup의 tiering 영향 때문에 send L1→L2 r1은 총 CPU가 감소했다. 복사한 `run-cell.sh` 그대로 L1에만 permit flag가 있고 L2/L3에는 없어 target permit/빈 poll 비용도 다르다.', '공유 CPU와 1s warmup의 tiering 영향으로 반복 간 CPU 차이가 크다. 원래 runner의 L2는 permit을 승계하지 않아 이를 별도로 측정했고 주 표본 L2에는 permit을 유지했다. 주 표본 L3와 permit 고정 L3를 함께 비교해 target loop/빈 poll 차이를 분리했다.')
# Keep conclusions numerical by deriving all L2-dependent values from primary data.
s=s.replace("(J/'report.md').write_text(report)", '''
def pair_cpu(L,role,group):return two([get('cpu',L,'request-backpressure',rep,role)['cpu']['js_exclusive'].get(group,{}).get('us_per_message',0) for rep in reps])
def pair_heap(L,role):return two([get('alloc',L,'request-backpressure',rep,role)['heap']['groups_bytes_per_message'].get('envelope',0) for rep in reps],'.0f')
def bp_step(lo,hi,k):return two(next(x for x in step_rows if x['pattern']=='request-backpressure' and x['step']==lo+'->'+hi)[k])
report=report.replace('**100.20/60.62µs**','**'+bp_step('L1','L2','JS_runtime')+'µs**').replace('**−2.01/−6.95µs**','**'+bp_step('L1','L2','addon')+'µs**')
report=report.replace('**12.55/11.21**','**'+pair_cpu('L2','source','envelope')+'**').replace('**7.02/6.92µs**','**'+pair_cpu('L2','target','envelope')+'µs**')
report=report.replace('**832/868B**','**'+pair_heap('L2','source')+'B**').replace('**1012/1036B**','**'+pair_heap('L2','target')+'B**')
for pat,old in [('send-saturation','12.05/12.16'),('request-backpressure','27.07/27.36')]:
 report=report.replace(old+'→',two([get('alloc','L2',pat,rep,'source')['native_per_message']['alloc_calls'] for rep in reps])+'→')
(J/'report.md').write_text(report)''')
p.write_text(s)
