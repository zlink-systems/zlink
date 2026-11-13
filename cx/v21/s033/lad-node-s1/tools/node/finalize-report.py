#!/usr/bin/env python3
import datetime,json,pathlib
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/lad-node-m1')
data=json.loads((job/'raw/aggregates/session.json').read_text())
lines=['\n## 기본 hold 결과\n','| 시나리오 | 크기 | binding msg/s | L0 msg/s | L0R msg/s | L1 msg/s | L0R 손실 | L1/L0R 손실 | L1/L0 손실 | 판정 | raw 결과 |','|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|']
daily={}
for row in data['rows']:
  vals={v:row['cells'][v]['median'] for v in ('binding','L0','L0R','L1')}
  raw=', '.join(f"{v}: `{row['cells'][v]['runs'][0]['path']}`" for v in ('binding','L0','L0R','L1'))
  lines.append(f"| {row['pattern']} | {row['payload']} | {vals['binding']:.1f} | {vals['L0']:.1f} | {vals['L0R']:.1f} | {vals['L1']:.1f} | {row['module_cost_l0r_vs_l0']:.2%} | {row['l1_loss_l1_vs_l0r']:.2%} | {row['l1_loss_l1_vs_l0']:.2%} | L0/binding {row['l0_verdict']}; L1 step {row['l1_verdict']} | {raw} |")
  daily[f"{row['pattern']}-{row['payload']}"]={'msg_s':vals['binding'],'raw':row['cells']['binding']['runs'][0]['path']}
lines.append('\n각 1회 측정의 G5는 적용하지 않는다. 셀 오류 0, submitted=completed=received 검사를 통과했다. L1 단계 판정은 L1/L0 손실 7% 이하 기준이다.\n')
confirm=[(r['pattern'],r['payload'],pair) for r in data['rows'] for pair in r['confirm_pairs']]
if confirm:
  lines.append('경계 5–9% 셀은 짧은 후속 hold에서 해당 비교 pair를 각 3회 측정해야 한다. 현재 필요 셀: '+', '.join(f'{p}-{n} {pair}' for p,n,pair in confirm)+'.\n')
with open(job/'report.md','a') as f: f.write('\n'.join(lines))
if not (job/'raw/aggregates/session.json').exists(): raise SystemExit('no aggregate')
path=pathlib.Path('/root/jobs/fx/cx/v21/s033/binding-daily/node-2026-11-13.json')
path.write_text(json.dumps({'date_utc':'2026-11-13','cells':daily},indent=2,ensure_ascii=False)+'\n')
print(job/'report.md'); print(path)
