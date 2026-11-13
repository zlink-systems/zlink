"""Keep the report's arithmetic synchronized with paper_model.py, not product code."""
from pathlib import Path
import json

ROOT=Path(__file__).resolve().parents[1]
p=ROOT/'report.md'
s=p.read_text()
for old,new in {
 '0.12–0.24':'0.18–0.46',
 '1.22–3.04':'1.28–3.26',
 '0.74–1.34':'0.80–1.56',
 '1.59–2.89':'1.65–3.11',
 '2.36–4.04':'2.48–4.48',
 '0.08–0.16와 생성·완료·resume 0.04–0.08':'0.08–0.16와 생성·완료·resume 0.10–0.30',
 'Frame pool 한 쌍 0.08–0.16와 생성·재개·완료 0.04–0.08':'Frame pool 한 쌍 0.08–0.16와 생성·재개·완료 0.10–0.30',
 'Caller에만 생기는 추가분은 `p_s×0.50–0.90`이다.':'Caller 검토에는 frame 생성이 빠지지 않도록 pending 전체 비용 `p_s×0.80–1.56`을 보수적으로 배정한다. 일부 resume·take는 실제로 completion 자원에서 실행하므로 이 caller 배정은 thread별 실측값이 아니다.',
 'K=8 send 상단 초과 0.381':'K=8 send 상단 초과 0.628',
 'p_r=0에서 0.214, p_r=1에서 0.414':'p_r=0에서 0.434, p_r=1에서 0.634',
}.items():
    s=s.replace(old,new)

cases=json.loads((ROOT/'analysis/paper-sums.json').read_text())['cases']
budgets=json.loads((ROOT/'analysis/recomputed.json').read_text())['cells']
def get(pattern,size,k,pending):
    return next(r for r in cases if r['pattern']==pattern and r['size']==size and r['batch']==k and r['pending_fraction']==pending)
def rng(x): return f'{x[0]:.3f}–{x[1]:.3f}'
notes=[
 'Target 상단이 0.628 초과한다. 예산 미충족 범위가 있다.',
 '이 시나리오의 CPU 합계는 들어간다. 실제 I/O thread 상한은 미확정이다.',
 'Caller 상단이 0.434 초과한다. Process CPU만 통과해도 부족하다.',
 '이 시나리오의 CPU 합계는 들어간다.',
 'CPU는 들어간다. 왕복 대기를 별도로 제한해야 한다.',
 '절대 목표의 CPU 예산에도 들어간다. 왕복 대기는 미확정이다.']
labels=['send 1024','send 4096','backpressure 1024','backpressure 4096','serial 1024','serial 4096']
rows=[]
for b,label,note in zip(budgets,labels,notes):
    serial=b['pattern']=='request-serial'
    r=get(b['pattern'],b['size'],1 if serial else 8,1 if serial else 0)
    rows.append(f"| {label} | {rng(r['caller_extra_us'])} / {b['caller_effective_share_us']:.3f} | {rng(r['source_extra_us'])} / {b['source_cpu_effective_share_us']:.3f} | {rng(r['target_extra_us'])} / {b['target_cpu_effective_share_us']:.3f} | {note} |")
start=s.index('| 셀 | Caller 추가 합계 / 허용량 |')
end=s.index('\n\n조건을 바꾸면',start)
head='| 셀 | Caller 추가 합계 / 허용량 | Source 추가 CPU / 허용량 | Target 추가 CPU / 허용량 | 판정 |\n|---|---|---|---|---|\n'
s=s[:start]+head+'\n'.join(rows)+s[end:]

start=s.index('| 조건 | 다시 계산한 영향 |')
end=s.index('\n\n원시 profile에는',start)
sensitivity='''| 조건 | 다시 계산한 영향 |
|---|---|
| Send 1024, K=64·p_s=0 | Target 상단도 2.120938로 허용량 1.849425보다 0.271512 크다. Full batch만 가정해도 상단 예산이 닫히지 않는다. |
| Send 1024, K=8·p_s=1 | Source 전체 추가 1.610–3.080, caller 검토 배정 1.510–2.830이다. 두 상단 모두 초과한다. Caller 조건은 `1.27+1.56p_s≤1.431373`, 즉 이 상단 모델에서 `p_s≤0.103444`다. |
| Backpressure 1024, p_r=1 | Caller 상단 3.310, 허용량 초과 0.633963이다. Native admission을 다시 구현해서 이 비용을 없애는 방식은 제외한다. |
| Backpressure 4096, p_r=1·K=8 | Caller 3.502, source 5.492, target 5.0795로 이 CPU 모델 안에는 들어간다. |
| Serial 1024 | 추가 endpoint CPU를 모두 직렬 비용으로 더하는 시나리오는 7.340–14.790이다. Binding RTT에 상단을 더하면 76.735618 µs다. 남은 **26.507078 µs** 안에 추가 lane·scheduler 대기와 native 비용 증가가 모두 들어가야 103.242696을 만족한다. |
| Serial 4096 | 같은 계산은 7.724–15.174, 총 86.503140 µs다. 남은 **28.505486 µs**가 115.008626까지의 추가 대기·native 비용 허용량이다. |'''
s=s[:start]+sensitivity+s[end:]
s=s.replace('0.48+3.04/K','0.48+3.26/K')
s=s.replace('Frame pool 0.08–0.16 + 생성·재개·완료 0.04–0.08 = 0.18–0.46',
            'Frame pool 0.08–0.16 + 생성·재개·완료 0.10–0.30 = 0.18–0.46')
p.write_text(s)
