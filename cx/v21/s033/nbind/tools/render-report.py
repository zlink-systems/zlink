import csv,json,pathlib
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/nbind');R=J/'raw'
s=json.loads((R/'per-message-summary.json').read_text());assert len(s)==60
key=lambda x:(x['mode'],x['level'],x['pattern'],x['label'].split('-')[-1])
base_data={key(x):x for x in s if not x['label'].startswith('permit-')}
permit_data={key(x):x for x in s if x['label'].startswith('permit-')}
data=dict(base_data);data.update({k:v for k,v in permit_data.items() if k[1]=='L2'})
matched_data=dict(data);matched_data.update(permit_data)
(R/'run-selection.json').write_text(json.dumps({'primary':{str(k):v['label'] for k,v in data.items()},'permit_fixed':{str(k):v['label'] for k,v in matched_data.items()},'original_runner':{str(k):v['label'] for k,v in base_data.items()}},indent=2)+'\n')
roles=['source','target'];patterns=['send-saturation','request-backpressure'];levels=['L1','L2','L3'];reps=['r1','r2']
def get(mode,L,pat,rep,role):return data[mode,L,pat,rep]['roles'][role]
def two(v,fmt='.2f'):return '/'.join(format(x,fmt) for x in v)
normal={('send-saturation','source'):dict(L1=3,L2=3,L3=3),('send-saturation','target'):dict(L1=4,L2=1,L3=1),('request-backpressure','source'):dict(L1=4,L2=4,L3=4),('request-backpressure','target'):dict(L1=7,L2=2,L3=2)}
lines=[]
for pat in patterns:
 for L in levels:
  for role in roles:
   audit=[get('audit',L,pat,rep,role) for rep in reps];alloc=[get('alloc',L,pat,rep,role) for rep in reps];cpu=[get('cpu',L,pat,rep,role)['cpu'] for rep in reps]
   base=normal[pat,role][L];extra=two([a['total_calls_per_message']-base for a in audit]);b=audit[0]['bytes_per_message'];out=sum(v for k,v in b.items() if k.endswith(':JS_to_native'));inc=sum(v for k,v in b.items() if k.endswith(':native_to_JS'))
   heap=two([a['heap']['estimated_bytes_per_message'] for a in alloc],'.0f');native=two([a['native_per_message']['alloc_calls'] for a in alloc])
   cp=' ; '.join(','.join(f"{c['split'].get(k,{}).get('us_per_message',0):.1f}" for k in ['JS_runtime','addon','libzlink','kernel']) for c in cpu)
   lines.append(f"| {'send' if pat=='send-saturation' else 'BP'} {'S' if role=='source' else 'T'} | {L} | {base} + {extra} | {out:.0f}/{inc:.0f} | {heap} | {native} | {cp} |")
api=[]
for pat in patterns:
 for L in levels:
  vals=[]
  for role,k in [('source','submit'),('target','receive'),('target','reply_submit'),('source','completion_receive')]:
   vals.append(two([get('cpu',L,pat,rep,role)['cpu']['api_inclusive'].get(k,{}).get('us_per_message',0) for rep in reps]) if pat!='send-saturation' or k in ['submit','receive'] else '—')
  api.append(f"| {'send' if pat=='send-saturation' else 'BP'} {L} | "+' | '.join(vals)+' |')
steps=[];step_rows=[]
for pat in patterns:
 for lo,hi in [('L1','L2'),('L2','L3')]:
  vals={k:[] for k in ['total','JS_runtime','addon','libzlink','kernel','copy','napi']}
  for rep in reps:
   a=[get('cpu',lo,pat,rep,r)['cpu'] for r in roles];b=[get('cpu',hi,pat,rep,r)['cpu'] for r in roles]
   vals['total'].append(sum(c['total_us_per_message'] for c in b)-sum(c['total_us_per_message'] for c in a))
   for k in ['JS_runtime','addon','libzlink','kernel']:vals[k].append(sum(c['split'].get(k,{}).get('us_per_message',0) for c in b)-sum(c['split'].get(k,{}).get('us_per_message',0) for c in a))
   for k,select in [('copy',lambda key:key.startswith('copy_leaf_')),('napi',lambda key:key=='napi_primitive_leaf')]:
    vals[k].append(sum(v['us_per_message'] for c in b for key,v in c['js_exclusive'].items() if select(key))-sum(v['us_per_message'] for c in a for key,v in c['js_exclusive'].items() if select(key)))
  steps.append(f"| {'send' if pat=='send-saturation' else 'BP'} {lo}→{hi} | "+' | '.join(two(vals[k]) for k in ['napi','JS_runtime','addon','copy','total'])+' |')
  step_rows.append(dict(pattern=pat,step=lo+'->'+hi,**vals))
(R/'added-cpu.json').write_text(json.dumps(step_rows,indent=2)+'\n')

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
(R/'permit-fixed-added-cpu.json').write_text(json.dumps(matched_steps,indent=2)+'\n')

with (R/'export-counts.tsv').open('w') as f:
 w=csv.writer(f,delimiter='\t');w.writerow(['run','role','messages','export','calls','calls_per_message','callbacks_per_message','outcomes_per_message'])
 for x in s:
  if x['mode']!='audit':continue
  for role,r in x['roles'].items():
   inst=json.loads((R/x['label']/f'{role}-instrument.json').read_text())
   for name,c in inst['counts'].items():w.writerow([x['label'],role,x['messages'],name,c,c/x['messages'],json.dumps({k:v for k,v in r['callbacks_per_message'].items() if k.startswith(name+':')}),json.dumps({k:v for k,v in r['outcomes_per_message'].items() if k.startswith(name+':')})])
with (R/'allocation-calls.tsv').open('w') as f:
 w=csv.writer(f,delimiter='\t');w.writerow(['run','role','messages','malloc/msg','calloc/msg','realloc/msg','free/msg','requested_native_B/msg','estimated_JS_B/msg'])
 for x in s:
  if x['mode']!='alloc':continue
  for role,r in x['roles'].items():w.writerow([x['label'],role,x['messages'],*[r['native_per_message'][k] for k in ['malloc','calloc','realloc','free','bytes']],r['heap']['estimated_bytes_per_message']])
# Include small routing-id stack copies in an explicit, source-derived companion record.
routing=[]
for x in s:
 if x['mode']!='audit':continue
 for role,r in x['roles'].items():
  if role=='source':n=24*(r['calls_per_message'].get('socketSubmitSend',0)+r['calls_per_message'].get('socketSubmitRequest',0))
  else:
   pid=next(e['pid'] for e in x['exits'] if e['role']=='source');n=len(f'bench-source-request-{pid}')*r['calls_per_message'].get('socketReply',0)
  routing.append(dict(run=x['label'],role=role,JS_to_native_routing_bytes_per_message=n,basis='addon_message_parts.h:17-33; client/main.js:194-199,208-213; actual source PID'))
(R/'routing-copy-bytes.json').write_text(json.dumps(routing,indent=2)+'\n')
report='''# Node binding 메시지당 비용: L1/L2/L3

**“메시지당 N-API 경계 비용이 Node L2/L3 손실의 단일 원인”이라는 가설을 기각한다.** Send source의 addon 호출은 세 단계 모두 정확히 **3회/message**다. BP도 정상 submit·completion 경로는 **4회/message**다. L2는 target의 정상 addon 호출을 줄인다. 반복해서 확인한 추가 protocol 비용은 **JS/V8의 header 처리·frame 표현·객체 생성**이며, native Buffer view와 copy 비용도 존재한다. 전체 CPU 차이에는 kernel 변동도 크게 포함된다.

`F=framework/languages/node/packages/framework/src`. HEAD `dc2eef3023`, branch `exp/4350-node-binding-cost`. 지정 build 두 단계 rc0, dist stale 0, addon SHA256 `ed260e4bb579…`. [빌드 지문](raw/build-fingerprint.json). Production·binding·bench tracked 변경 0. [호출 경로와 모든 경계](call-shape.md).

**조건과 단위.** 주 표본은 L1 raw+permit, 요청의 `+`를 적용한 L2 raw+permit+envelope, 명시된 L3 raw+service-wire다. 원래 runner의 L2(no permit)와 L3+permit을 대조군으로 별도 보존했다([선택한 run](raw/run-selection.json)). 1K send-saturation/BP(request-backpressure), warmup 1s + active 제출 5s + drain. 계수·CPU·할당을 각각 별도 process에서 **2회** 수집했다. CPU는 wrapper 없이 `perf stat task-clock` + `perf record cpu-clock 399Hz/dwarf` + Node `--perf-basic-prof`; 네 collector가 disabled 상태에서 준비된 뒤 함께 enable한다. Heap은 collected-object sampling 8KiB, native는 LD_PRELOAD malloc/calloc/realloc/free counter다. 주 표본36+대조군24, 총60 cell/120 process에서 오류0·제출=완료=수신, 전체 thread CPU `6–7,14–15`, SCHED_OTHER, lifetime ≤14.35s. [모든 개별 수량·정책](raw/profile-audit.json).

**message당 전체 비용.** S=source, T=target. 각 `/`는 r1/r2이며 payload B의 `/`만 JS→native/native→JS다. NAPI는 **정상 message 경계 수 + 빈 poll·poller/event 조회의 추가 호출 r1/r2**다. CPU는 `JS/V8, addon, libzlink, kernel` µs 순서이며 `;` 앞뒤가 r1/r2다. Native A는 전체 process의 malloc+calloc+realloc 호출 수이며 addon 전용이나 live 객체 수가 아니다. JS heap은 누적 allocation의 sampling 추정 B다. CPU는 kernel leaf를 우선하고 나머지는 가장 가까운 JS/addon/Core stack으로 귀속한 period 가중 비율에 task-clock을 곱했다. Process CPU delta와의 차이는 −0.3~+3.6%다.

| 셀·역할 | 단계 | NAPI/message | payload copy B out/in | JS heap B r1/r2 | native A r1/r2 | CPU µs J,A,C,K: r1 ; r2 |
|---|---|---:|---:|---:|---:|---|
'''+ '\n'.join(lines)+'''

Routing copy는 위 payload와 별도다. Source 제출마다 JS→native **24B**, BP target reply마다 **28B**, source reply completion마다 native→JS **24B**가 더해진다. [routing 근거](raw/routing-copy-bytes.json). Target routingId는 warmup 뒤 재사용된다. Export별 원 횟수·callback·빈 결과는 [export-counts.tsv](raw/export-counts.tsv), malloc/calloc/realloc/free·native 요청 bytes는 [allocation-calls.tsv](raw/allocation-calls.tsv), 모든 추정치는 [per-message-summary.json](raw/per-message-summary.json)에 있다.

정상 native→JS allocator callback은 source send 0, source BP 2, target L1 0/L2·L3 2회다. Native가 만드는 객체·Buffer·bigint의 반환은 각 export 경계 안에 있으며 별도의 addon 호출로 중복 계수하지 않았다. L3 source는 `messageFrameClose` 1회를 `messageFrameData` 1회로 바꾼다. **호출 수가 같아도 반환 값·native allocation 비용은 같지 않다.**

**한 번의 native 제출·수신에 해당하는 CPU.** 아래는 해당 addon entry stack의 inclusive on-CPU µs/input-message다. Core의 동기 작업과 N-API 값 생성을 포함하고, JS codec·다른 IO thread는 제외한다. Receive/completion에는 해당 window의 빈 시도도 포함하므로 성공 호출 하나의 순수 latency가 아니다. L1 target `.data()`의 별도 copyData CPU는 send **3.61/2.35**, BP **3.47/4.08µs**이며 receive entry 열에 합치지 않았다.

| 1K 셀·단계 | source submit r1/r2 | target receive r1/r2 | target reply submit r1/r2 | source completion receive r1/r2 |
|---|---:|---:|---:|---:|
'''+ '\n'.join(api)+'''

**단계 추가 CPU: source+target 합계 µs/input-message.** N-API self는 leaf가 `napi_*`/N-API trampoline인 표본, copy self는 memcpy/memmove 계열 leaf 표본이다. 각각 경계 전체나 모든 inlined copy의 전수 비용은 아니다. J/addon 열에 이미 포함되는 부분이므로 아래 열들을 더하지 않는다.

| 단계 | ΔN-API self r1/r2 | ΔJS/V8 r1/r2 | Δaddon r1/r2 | Δcopy self r1/r2 | Δ전체 r1/r2 |
|---|---:|---:|---:|---:|---:|
'''+ '\n'.join(steps)+'''

**permit 고정 대조군.** L3에도 L1/L2와 같은 permit을 유지한 경우의 source+target Δµs다. Target loop 차이를 없애도 L2→L3의 두 셀에서 JS/V8 증가가 남는다.\n\n| 단계 | ΔJS/V8 r1/r2 | Δaddon r1/r2 | Δ전체 r1/r2 |\n|---|---:|---:|---:|\n'''+ '\n'.join(matched_lines)+'''\n\n- **L1→L2:** 추가 정상 JS→addon 호출 S **0**, T send **−3**/BP **−5**; target allocator callback은 **+2**다. BP의 JS/V8 증가 **100.20/60.62µs**와 addon 감소 **−2.01/−6.95µs**가 단일 경계 원인설과 맞지 않는다. 직접 식별한 envelope JS CPU만 source **12.55/11.21**, target **7.02/6.92µs**이며 L1은0이다. 원인은 `F/runtime/channels/channel-envelope.ts:124,471,521,658,886,960,1052`: correlation/deadline, template copy, reader·문자열·header 객체다. Codec 아래 sampled heap은 source **832/868B**, target **1012/1036B**이며 target에는 protobuf reply encode도 포함된다.
- **L2→L3:** 정상 호출·allocator callback 추가 **0**이다. JS pack copy는 send **1247B**, BP request **1299B**·reply **1240B**씩 추가된다. Boundary payload copy 증가는 send S out **1113B**/T in **86B**, BP S out/in **1121/96B**, T out/in **96/94B**다. 원인은 `F/runtime/foundation/service-wire-m6a-codec.ts:216,256,274,337,409`: frame 작성, 길이·text 검사, part view·wrapper 생성이다. L3 BP의 service-wire JS CPU는 S **5.92/4.49**, T **8.69/3.88µs**이며 L2는0이다. Source native allocation도 send **12.05/12.16→18.97/18.90**, BP **27.07/27.36→36.65/36.69회/message**로 늘었다.
- **경계의 실제 잔여 비용:** L3 source `messageFrameData` entry는 send **3.84/3.80**, BP **5.52/5.21µs**다. `bindings/node/native/src/addon_core.cc:1113`→`addon_message_values.h:125`의 external Buffer 생성·수명 관리 비용이다. `Message.allocate+data`가 copy를 줄이면서도 호출·값·owner 비용을 더할 수 있다는 기존 n2hdr/#4415 결과와 일치한다. 이 비용이 L2/L3 전체 추가 CPU를 설명하지는 않는다.

Caller 경계도 남아 있다. L3 source ladder는 native protobuf body를 먼저 만든 뒤 `.data()`로 pack에 사용한다(`framework/bench/grpc/node/shared/raw-wire.js:174`, `client/main.js:259,304`). 이 추가 view와 GC 수명 비용 전체를 production codec에 귀속하지 않는다.

**정밀 귀속의 한계.** CPU의 JS/V8 열에는 JIT·GC·libuv·확인되지 않은 native helper도 포함한다. Unresolved stack은 최대0.94%이며 codec 수치에 넣지 않았다. 공유 CPU와 1s warmup의 tiering 영향으로 반복 간 CPU 차이가 크다. 원래 runner의 L2는 permit을 승계하지 않아 이를 별도로 측정했고 주 표본 L2에는 permit을 유지했다. 주 표본 L3와 permit 고정 L3를 함께 비교해 target loop/빈 poll 차이를 분리했다. 따라서 위 관측 차이를 세 원인의 정확한 인과 합계나 공식 throughput 손실률로 바꾸지 않는다. **고정 경계 증가 0, 직접 관측한 JS codec CPU·heap 증가, copy byte 증가, addon CPU 감소/증가의 분리**까지가 확인된 결론이다.

**.NET 대조.** [d2l2](../d2l2/report.md)의 codec-only managed allocation은 source encode216+reply decode280=**496B**, target request decode328+reply encode216=**544B**이고 inbound header byte copy는0이다. Node의 위 target codec sampling에는 protobuf도 있어 객체 수의 직접 배율 비교는 하지 않는다. [d3l3](../d3l3/report.md)의 send1K CPU는 source submit12.55→14.61µs, finalizer0.20→0.86µs, target4.58→4.60µs였다. 해당 .NET ladder의 head 중복·미해제와 completion 표현 차이도 보고돼 있어 이 수치나 기존 2–5%를 Node의 순수 N-API 계수로 환산할 수 없다.

검증: known 8×1KiB 입력에서 from/data/copy/close 각 횟수와 양방향8192B 확인, 잘못된 Buffer 인자 거부·예외 계수 확인([raw](raw/preload-check/check.log)). Native counter는 알려진 malloc1001/calloc1000/realloc1000/free2000을 일치시켰다([raw](raw/counter-check-correct/summary.json)). 초기 collector·준비 실패와 제외 이유는 [excluded-runs.json](raw/excluded-runs.json)에 보존했다. 남은 유효 run의 오류0. Production/binding 수정·commit·push·GitHub·sub-agent·internal-docs·measure.lock 취득·SIGCONT 사용0. 판정 규칙 수 변경0, 성능·중복·수명 점검의 관측 사항은 위 call-shape에 기록했으며 구현 변경0이다.
'''

def pair_cpu(L,role,group):return two([get('cpu',L,'request-backpressure',rep,role)['cpu']['js_exclusive'].get(group,{}).get('us_per_message',0) for rep in reps])
def pair_heap(L,role):return two([get('alloc',L,'request-backpressure',rep,role)['heap']['groups_bytes_per_message'].get('envelope',0) for rep in reps],'.0f')
def bp_step(lo,hi,k):return two(next(x for x in step_rows if x['pattern']=='request-backpressure' and x['step']==lo+'->'+hi)[k])
report=report.replace('**100.20/60.62µs**','**'+bp_step('L1','L2','JS_runtime')+'µs**').replace('**−2.01/−6.95µs**','**'+bp_step('L1','L2','addon')+'µs**')
report=report.replace('**12.55/11.21**','**'+pair_cpu('L2','source','envelope')+'**').replace('**7.02/6.92µs**','**'+pair_cpu('L2','target','envelope')+'µs**')
report=report.replace('**832/868B**','**'+pair_heap('L2','source')+'B**').replace('**1012/1036B**','**'+pair_heap('L2','target')+'B**')
for pat,old in [('send-saturation','12.05/12.16'),('request-backpressure','27.07/27.36')]:
 report=report.replace(old+'→',two([get('alloc','L2',pat,rep,'source')['native_per_message']['alloc_calls'] for rep in reps])+'→')
(J/'report.md').write_text(report)
print('report lines',len(report.splitlines()),'bytes',len(report.encode()))
