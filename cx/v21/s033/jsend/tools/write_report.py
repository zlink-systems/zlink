from pathlib import Path
import json,statistics,collections
P=Path('/root/jobs/fx/cx/v21/s033/jsend')
def load(n):return json.load(open(P/(n+'.json')))
perf=load('perf-summary');ss=load('aligned-ss-summary');sysc=load('syscall-summary');net=load('net-summary');hand=load('handoff-summary');rcv=load('rcvbuf-summary');valid=load('validation');lines=[]
def para(s):lines.extend([s,''])
def table(head,rows):
 lines.append('| '+' | '.join(head)+' |');lines.append('| '+' | '.join(['---']*len(head))+' |')
 for row in rows:lines.append('| '+' | '.join(map(str,row))+' |')
 lines.append('')
def f(n,d=0):return f'{n:,.{d}f}'
def cell(name):return json.load(open(P/'runs'/name/'results.json'))['cells'][0]
def unpaced(name):
 vals=[cell(f'{name}-unpaced-{i}')['throughput_per_second'] for i in range(1,4)];return vals,statistics.median(vals)
def thread(name,role,io):return next(t for t in perf[name]['threads'].values() if t['role']==role and t['comm'].endswith('/'+str(io)))
def send(name):return thread(name,'target',1)['stage_cycles_per_request']['send']
def conn(name,reply):
 server=5247 if name.startswith('binding') else 5254
 return next(c for c in ss[name]['connections'] if (c['local']==server)==reply and c['per_request']['bytes_sent']>500)
def opposite(name,c):return next(x for x in ss[name]['connections'] if (x['local'],x['peer'])==(c['peer'],c['local']))
B=unpaced('binding')[1];rows=[]
for case,label in [('main','main Framework'),('prototype','descriptor prototype'),('binding','binding')]:
 vals,med=unpaced(case);rows.append([label,*[f(x,3) for x in vals],f(med,3),f(min(vals),3)+'–'+f(max(vals),3),f(med/B,4)])
table(['조건','1회 req/s','2회 req/s','3회 req/s','중앙값 req/s','범위 req/s','F÷B'],rows)
para('F÷B는 이번 세션에서 측정한 세 번의 처리량 중앙값을 binding 중앙값으로 나눈 값이다. 순서는 main → prototype → binding을 세 번 반복했다. 이전 보고서의 처리량을 섞지 않았다. Source CPU 0–7, target CPU 8–15의 condition B, request-backpressure, payload 1,024 B, request window 100, send concurrency 8, warmup 30초, active 60초를 유지했다. 위 9회에는 JFR·perf·BPF·active packet capture를 켜지 않았다.')
para('프로토타입은 framework/bench/grpc에서 `/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh`로 다시 빌드했다. Runtime JAR SHA-256은 `f2563a323bc368d0fbb67920c6ca88e14f79850eb145f71e011e9e5b0994b782`로 jenv의 프로토타입과 byte-identical하다. Main은 jenv/main-build를 재사용했다. Main의 50k source만 기존 main-counter-build의 pacing driver를 사용했다. Binding JAR은 세 조건 모두 1.23.0이며 SHA-256은 `077dc5df4c1a473365e03abe5605846ab8e7620d952bf48ec6bc41f976d9d714`다. 로드한 native library는 모든 capture에서 `6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01` SHA-256의 동일 libzlink.so였다. Build manifest와 command JSON을 보존했다.')
proto='prototype-50k-combined';bind='binding-50k-combined';main='main-50k-combined';pc=conn(proto,True);bc=conn(bind,True);cr=send(proto)/send(bind);pr=pc['per_request']['data_segs_out']/bc['per_request']['data_segs_out'];cp=send(proto)/pc['per_request']['data_segs_out'];cb=send(bind)/bc['per_request']['data_segs_out'];expl=(pc['per_request']['data_segs_out']-bc['per_request']['data_segs_out'])*cb/(send(proto)-send(bind))
para(f'추가 TCP 비용의 주원인은 reply를 더 작은 TCP data packet으로 나누어 보내는 것이다. Perf와 겹치는 ss 구간에서 prototype은 요청당 {pc["per_request"]["data_segs_out"]:.4f} packet, binding은 {bc["per_request"]["data_segs_out"]:.4f} packet을 보냈다. 비율은 {pr:.3f}배다. Target IO/1의 tcp_sendmsg 비용은 {f(send(proto))}/{f(send(bind))} cycles/request, 즉 {cr:.3f}배였다. Packet당 비용은 약 {f(cp)}/{f(cb)} cycles로 {cp/cb:.3f}배다. Binding의 packet당 비용을 고정해 계산하면 packet 수 증가가 추가 {f(send(proto)-send(bind))} cycles/request의 약 {expl*100:.1f}%를 설명한다. Framework에 별도의 비싼 byte-copy 경로가 필요하다는 설명보다 이 관측이 비용 차이를 잘 설명한다.')
rows=[]
for name,label,rec in [(main,'main',1443),(proto,'prototype',1092),(bind,'binding',1252)]:
 c=conn(name,True);o=opposite(name,c);s=send(name);rx=thread(name,'source',2)['stage_cycles_per_request']['recv'];rows.append([label,f(s),f(s/rec,3),f(c['per_request']['data_segs_out'],4),f(c['avg_data_seg_bytes'],1),f(1/c['per_request']['data_segs_out'],3),f(o['per_request']['acks_out'],4),f(s/c['per_request']['data_segs_out']),f(rx)])
table(['50k perf 구간','target send cycles/req','send cycles/B','reply data seg/req','평균 data seg B','record/seg','reply ACK/req','send cycles/seg','source receive cycles/req'],rows)
para('Reply record는 main 1,443 B, prototype 1,092 B, binding 1,252 B다. Record 크기는 TCP/IP header를 제외한다. 위 packet·ACK 값은 perf 수집 안에 들어가는 약 17초의 ss delta를 사용했다. Source IO/2 수신 비용도 prototype/binding에서 '+f(thread(proto,'source',2)['stage_cycles_per_request']['recv']/thread(bind,'source',2)['stage_cycles_per_request']['recv'],3)+'배로 늘었다. Reply packet 증가와 같은 방향이며 packet당 receive 비용은 오히려 prototype이 조금 낮았다.')
if 'original-50k-repeat' in perf and 'binding-50k-repeat' in perf:
 p2='original-50k-repeat';b2='binding-50k-repeat';c2=conn(p2,True);d2=conn(b2,True)
 para(f'별도 50k 대조에서도 prototype/binding은 {f(send(p2))}/{f(send(b2))} cycles/request, {c2["per_request"]["data_segs_out"]:.4f}/{d2["per_request"]["data_segs_out"]:.4f} packet/request였다. Packet당 비용은 {f(send(p2)/c2["per_request"]["data_segs_out"])}/{f(send(b2)/d2["per_request"]["data_segs_out"])} cycles다. Binding의 packet 수가 바뀔 때 request당 비용도 같이 바뀌었고 packet당 비용은 비슷했다. 처음 비교의 cycles/B 비율은 {cr*1252/1092:.3f}배, 대조 비교는 {(send(p2)/1092)/(send(b2)/1252):.3f}배다. 이전 보고서의 18.95/6.32 cycles/B라는 정확한 3.00배 수치는 재현하지 못했다.')
para('BPF syscall 수와 perf 비용을 서로 다른 창에서 나누면 이 원인을 놓친다. Binding reply data seg/request는 perf 구간 '+f(bc['per_request']['data_segs_out'],4)+', packet BPF 구간 '+f(next(v/net[bind]['messages'] for k,v in net[bind]['maps']['@skb'].items() if k.startswith('5247,') and k.endswith(',1')),4)+'였다. 두 창 모두 약 50k req/s였다. 완료 요청 rate가 같아도 syscall/packet 계측이 write와 수신의 도착 간격 및 묶음을 바꾼다. 기존 BPF의 호출 비율 1.22배를 perf의 3배 비용에 직접 적용할 수 없다. 이 보고서의 비용 원인 표는 perf와 시간상 겹친 ss를 사용했다.')
para('Framework는 mesh 수신 뒤 요청마다 virtual handler turn을 실행하고 여러 carrier가 reply를 제출한다. Raw binding bench는 한 thread의 recv→reply loop에서 연속 제출한다. 이 생산 간격 차이가 Core write batch의 충전률을 낮추는 것으로 해석한다. 어느 dispatch 경계가 얼마를 분산하는지는 record별 gap으로 분리하지 않았다. TCP 경계에서 확정한 기전은 NODELAY=1에서 더 많은 작은 packet을 내보내고, 각 packet에 같은 network/loopback 작업을 반복한다는 것이다.')
pt=thread(proto,'target',1);bt=thread(bind,'target',1);groups=[('TX_network_without_RX_softirq','TX network 경로'),('TCP_receive_without_wakeup','송신 안의 TCP receive'),('RX_softirq_other','나머지 RX softirq'),('wakeup','깨우기 경로'),('send_other','tcp_sendmsg 나머지')]
table(['tcp_sendmsg 아래의 배타 분류','prototype cycles/req','binding cycles/req','차이'],[[label,f(pt['send_groups_cycles_per_request'].get(k,0)),f(bt['send_groups_cycles_per_request'].get(k,0)),f(pt['send_groups_cycles_per_request'].get(k,0)-bt['send_groups_cycles_per_request'].get(k,0))] for k,label in groups])
para('각 표본은 깨우기 → TCP receive → 나머지 RX softirq → TX network → 나머지 순서로 한 범주에만 배정했다. Loopback 송신은 local delivery·netfilter·TCP receive를 송신 syscall 안에서 진행할 수 있다. 이 작업이 packet마다 반복된다. 아래는 tcp_sendmsg stack을 가진 표본의 kernel self 비용이다. Inclusive 비용을 서로 더하지 않았다.')
ps=dict(pt['send_self_cycles_per_request']);bs=dict(bt['send_self_cycles_per_request']);funcs=['nft_do_chain','__tcp_transmit_skb','nf_hook_slow','_copy_from_iter','tcp_sendmsg_locked','nf_conntrack_tcp_packet','tcp_v4_rcv','tcp_rcv_established','kmem_cache_alloc_node','try_to_wake_up']
table(['kernel function self','prototype cycles/req','binding cycles/req'],[[k,f(ps.get(k,0)),f(bs.get(k,0))] for k in funcs])
para('Kernel copy의 `_copy_from_iter`는 약 '+f(ps.get('_copy_from_iter',0))+'/'+f(bs.get('_copy_from_iter',0))+' cycles/request로 거의 같았다. 할당·깨우기 하나가 차이를 지배하지 않았다. I/O thread 전체 kernel IPC는 prototype/binding target IO/1에서 '+f(pt['ipc'],3)+'/'+f(bt['ipc'],3)+'였다. cycles는 thread별 cycles:k hardware counter에 call-chain 표본 period 비율을 적용했다. Sample rate는 249 Hz이며 counter 수집은 처음 비교 20초, 대조 25초다. 모든 수치를 추정치로 해석한다.')
rows=[]
for name,label in [(main,'main'),(proto,'prototype'),(bind,'binding')]:
 for reply,kind in [(False,'request'),(True,'reply')]:
  c=conn(name,reply);o=opposite(name,c);rows.append([label,kind,f(c['per_request']['data_segs_out'],4),f(o['per_request']['acks_out'],4),f(c['avg_data_seg_bytes'],1),f(c['mss']),int(c['delta']['total_retrans'])])
table(['perf 구간','연결','data seg/req','반대 방향 ACK/req','평균 data seg B','sender MSS B','재전송 delta'],rows)
para('최초 세 조건의 net_dev_start_xmit에서 모든 관측 skb는 gso_segs=1, gso_size=0이었다. MSS보다 큰 write를 여러 TSO segment로 쪼갠 차이가 없었다. Active의 retransmit는 ss와 tcp_retransmit_skb에서 모두 0이었다. tcp_send_ack 시점의 quick>0인 ACK는 0이었다. Delayed-ACK scheduling과 실제 timer 발화를 구분해 세었으며 상세 값은 net-summary.json의 함수 ID 0=send_ack, 1=send_delayed_ack, 2=delack_timer_handler에 있다. ACK 상당수는 source IO/2의 tcp_recvmsg/cleanup 경로에서 생성됐다. Reply sender socket의 tcp_ack가 target IO/1에서 실행된 횟수는 net BPF 창에서 prototype 약 0.241/request, binding 약 0.170/request였다. 이 BPF 계수는 위 perf 비용과 직접 합산하지 않았다.')
rows=[]
for name,label in [(main,'main'),(proto,'prototype'),(bind,'binding')]:
 nr=net[name];m=nr['maps'];n=nr['messages']
 for reply,kind in [(False,'request ACK'),(True,'reply ACK')]:
  c=conn(name,reply);sp,dp=c['peer'],c['local'];suffix=str(sp)+','+str(dp)
  rows.append([label,kind,f(m.get('@events',{}).get('0,'+suffix,0)/n,4),f(m.get('@events',{}).get('1,'+suffix,0)/n,4),m.get('@events',{}).get('2,'+suffix,0),m.get('@ackmode',{}).get(suffix+',1',0)])
table(['packet BPF 창','연결의 ACK','send_ack/req','delayed-ACK schedule/req','실제 delayed timer 호출','quick>0 ACK 수'],rows)
rows=[]
for name,label in [(main,'main'),(proto,'prototype'),(bind,'binding')]:
 for reply,kind in [(False,'request 수신'),(True,'reply 수신')]:
  c=conn(name,reply);o=opposite(name,c);key=str(o['local'])+'->'+str(o['peer']);v=rcv[name][key];rows.append([label,kind,f(v['first']),f(v['last']),f(v['max'])])
table(['조건','수신 측','첫 warmup ss rcvbuf B','마지막 active ss B','최대 B'],rows)
para('ss -tinmeo를 warmup과 active에서 약 2초 간격으로 기록했다. Active의 rcvbuf는 각 연결에서 안정적이었다. Prototype의 reply 수신 buffer가 binding보다 작아서 생긴 3배 차이라는 설명은 이번 실행에서 지지되지 않았다. 원본 tcp_info/ss의 rcv_space, rcv_ssthresh, RTT, rcvmss도 각 capture/ss.json과 warmup-ss.json에 보존했다.')
rows=[]
for name,label in [(main,'main'),(proto,'prototype'),(bind,'binding')]+([('nagle-50k-combined','nagle')] if 'nagle-50k-combined' in sysc else []):
 ini=json.load(open(P/'runs'/name/'capture/initial.json'))
 for t in sysc[name]['rows']:
  if t['calls']<1000:continue
  role='source' if t['pid']==ini['processes']['source']['pid'] else 'target';comm=ini['processes'][role]['threads'][str(t['tid'])]['comm'];reply=(role=='target' and t['op']==44) or (role=='source' and t['op']==45);record=(1443 if reply else 1435) if label=='main' else 1252 if label=='binding' else 1092
  rows.append([label,role+' '+comm.rsplit('/',1)[-1], 'sendto' if t['op']==44 else 'recvfrom',f(t['calls_per_request'],3),f(t['requested_per_call'],1),f(t['bytes_per_positive'],1),f(t['bytes_per_positive']/record,3),t['short'],t['eagain']])
table(['BPF 창','role IO index','syscall','calls/req','요청 B/call','반환 B/성공 call','record 환산/성공 call','short calls','EAGAIN calls'],rows)
para('모든 sendto의 반환 B는 요청 B와 같았고 partial write와 send EAGAIN은 0이었다. Recvfrom의 short는 요청 buffer보다 적게 읽은 정상 수신이며 partial write로 세지 않았다. Record 환산은 반환 B/record B이고 call 경계에서 잘린 record의 개별 경계를 센 값은 아니다. 요청·반환 byte의 256 B bucket 0–8 KiB와 ≥8 KiB bucket 전체를 byte-histograms.csv에 적었다. Syscall별 원계수와 계측 창 rate는 syscall-details.csv 및 syscall-summary.json에 있다. BPF START/STOP의 정확한 시간을 사용하고, 같은 창 전후 stats의 완료 rate로 request 수를 보정했다. Stats 조회와 gate 경계 차이 때문에 작은 비율은 정밀한 정수 계수로 해석하지 않는다.')
para('Prototype reply는 virtual handler turn에서 생성했고 native 제출은 관측한 세 ForkJoinPool carrier worker에서 실행됐다. Binding은 bench-raw-request platform thread에서 reply 생성과 native 제출을 수행했다. Prototype JFR의 descriptor encode 표본도 virtual handler turn에 있었다. 실제 sendto는 두 조건 모두 별도의 target IO/1이다. Source receive는 IO/2다. Target의 producer와 IO thread는 CPU 8–15 안에서 이동했고 source I/O는 0–7 안에서 이동했다. TID·CPU별 전체 분포는 capture/handoff.log의 @reply·@runtime, syscall.log의 @callcpu에 있다. CPU별 caller→sender의 record 단위 인과관계는 추적하지 않았다.')
rows=[]
for name,label in [('prototype-50k-handoff','prototype'),('binding-50k-handoff','binding')]:
 h=hand[name]['handoff'];m=h['maps'];ini=json.load(open(P/'runs'/name/'capture/initial.json'));prod=collections.Counter();tids=set()
 for k,v in m.get('@reply',{}).items():
  tid,cpu,comm=k.split(',',2);tids.add(tid);prod[int(cpu)]+=v
 mx=prod.most_common(1)[0];rows.append([label,'reply native submit','/'.join(sorted(tids,key=int)),','.join(map(str,sorted(prod))),str(mx[0])+' ('+f(mx[1]/sum(prod.values())*100,1)+'%)'])
 for role,index in [('target',1),('source',2)]:
  tid=next(t for t,v in ini['processes'][role]['threads'].items() if v['comm'].endswith('/'+str(index)));cpus=collections.Counter()
  for k,v in m.get('@runtime',{}).items():
   t,cpu,comm=k.split(',',2)
   if t==tid:cpus[int(cpu)]+=v
  mx=cpus.most_common(1)[0];rows.append([label,role+' IO/'+str(index),tid,','.join(map(str,sorted(cpus))),str(mx[0])+' ('+f(mx[1]/sum(cpus.values())*100,1)+'%)'])
table(['handoff 창','실행 위치','TID','관측 CPU','최다 CPU 비중'],rows)
para('Producer CPU 비중은 native reply 호출 수, I/O CPU 비중은 sched_switch로 센 실행 시간 기준이다. Virtual thread는 대기 후 다른 carrier에서 재개할 수 있다. 생성·제출·TCP 송신의 record별 CPU 연결은 미계수다.')
rows=[]
for name,label in [('prototype-50k-handoff','prototype'),('binding-50k-handoff','binding'),('direct-50k-handoff','direct')]:
 h=hand[name]['handoff'];ini=json.load(open(P/'runs'/name/'capture/initial.json'));row=[label]
 for role,index in [('source',0),('source',2),('target',0),('target',1)]:
  tid=next(t for t,v in ini['processes'][role]['threads'].items() if v['comm'].endswith('/'+str(index)));row.append(f(h['io'][tid]['wakeup_per_request'],3))
 rows.append(row)
table(['sched 창 wakeups/request','source IO/0','source IO/2','target IO/0','target IO/1'],rows)
rows=[]
for name,label in [('prototype-50k-handoff','prototype'),('binding-50k-handoff','binding'),('direct-50k-handoff','direct')]:
 if name not in hand:continue
 h=hand[name]['handoff'];co=hand[name]['completion'];ini=json.load(open(P/'runs'/name/'capture/initial.json'));t=next(t for t,v in ini['processes']['target']['threads'].items() if v['comm'].endswith('/1'));m=co['maps'].get('@completion',{});pid=ini['processes']['target']['pid'];rows.append([label,f(h['native_reply_per_request'],3),f(h['io'][t]['wakeup_per_request'],3),m.get('0,'+str(pid)+','+t,0),m.get('1,'+str(pid)+','+t,0),m.get('2,'+str(pid)+','+t,0)])
table(['5초 uprobe/sched 창','native reply/req','target IO/1 wakeups/req','sync write calls','async write starts','write completion callbacks'],rows)
para('Reply producerからIO/1へのlogical handoff는 두 조건 모두 native reply 하나당 1회다. Framework의 receive→mesh→virtual handler→IO/1 경로에는 raw receive thread→IO/1보다 application dispatch 경계가 하나 더 있다. 이 비용을 tcp_sendmsg 비용에 더하지 않았다. Sync write fast path는 관측되지 않았고 async 시작과 write completion callback 수는 창 경계의 1건 차이 외에는 일치했다. API reply의 one-shot 동기 submit과 transport의 비동기 write를 구분해야 한다. Direct의 sched_waking에서는 target IO/1 깨우기 대부분이 다른 CPU에서 시작됐다. Native API 제출 경계가 Framework에만 추가된 것은 아니며 kernel wakeup self 차이는 전체 send 차이의 작은 부분이다.')
para('시험한 첫 변경은 ZLinkJavaRawServicePort.replyMessages에서 inStateLane/awaitTurn과 port 소유 map 확인을 없애고 기존 router.reply(target, token).message(...).submit()을 직접 호출한 것이다. Token owner 확인, whole-message admission, 연결 선택과 close admission은 binding/Core를 사용한다. 기존 reply-winner와 message close를 유지했다. State lane은 caller가 자신의 turn을 실행하는 구현이므로 원래도 native zlink_reply가 mesh thread로 항상 이동하는 것은 아니었다.')
if all((P/'runs'/f'direct-unpaced-{i}'/'results.json').exists() for i in range(1,4)):
 vals,med=unpaced('direct');c=conn('direct-50k-combined',True)
 table(['직접 제출 변경','1회 req/s','2회 req/s','3회 req/s','중앙값 req/s','범위 req/s','F÷B'],[['direct',*[f(x,3) for x in vals],f(med,3),f(min(vals),3)+'–'+f(max(vals),3),f(med/B,4)]])
 para(f'기존 prototype 중앙값보다 {100*(med/unpaced("prototype")[1]-1):.1f}% 높았다. 50k에서 target send는 {f(send("direct-50k-combined"))} cycles/request, reply data packet은 {c["per_request"]["data_segs_out"]:.4f}/request로 줄었다. Binding의 비용까지 내려가지는 않았다. 추가 lane 경합을 제거하는 것은 유효하지만 더 작은 reply packet을 만드는 원인을 전부 없애지는 않는다.')
# The option-only experiment is appended after the matrix finishes.
if (P/'runs/nagle-50k-combined/results.json').exists():
 c=cell('nagle-50k-combined');para('둘째 변경은 direct 위에 기존 binding 옵션 `router.options().tcpNoDelay(false)` 한 줄을 적용한 것이다. Framework가 별도 batch queue나 timer를 만들지 않고 TCP가 write를 합치도록 하는 최소 옵션 변경이다. Source와 target의 이 port에서 만드는 ROUTER 모두에 적용했다. TCP batching 정책은 Core/kernel이 소유한다. 결과는 다음과 같다.')
 vals,med=unpaced('nagle');table(['tcpNoDelay(false)','1회 req/s','2회 req/s','3회 req/s','중앙값 req/s','범위 req/s','F÷B'],[['nagle',*[f(x,3) for x in vals],f(med,3),f(min(vals),3)+'–'+f(max(vals),3),f(med/B,4)]])
 para(f'50k 목표 실행의 실제 전체 rate는 {f(c["throughput_per_second"],3)} req/s였다. Latency 표본 mean/p95/p99는 {c["latency_mean_ms"]:.3f}/{c["latency_p95_ms"]:.3f}/{c["latency_p99_ms"]:.3f} ms다. 기존 prototype의 p95/p99는 {cell(proto)["latency_p95_ms"]:.3f}/{cell(proto)["latency_p99_ms"]:.3f} ms, direct는 {cell("direct-50k-combined")["latency_p95_ms"]:.3f}/{cell("direct-50k-combined")["latency_p99_ms"]:.3f} ms다.')
 if 'nagle-50k-combined' in ss:
  x=conn('nagle-50k-combined',True);para(f'그 실행의 target send는 {f(send("nagle-50k-combined"))} cycles/request, reply data packet은 {x["per_request"]["data_segs_out"]:.4f}/request, 평균 data segment는 {f(x["avg_data_seg_bytes"],1)} B였다. ')
if (P/'runs/nagle-50k-combined/results.json').exists():
 n='nagle-50k-combined';v=cell(n);x=conn(n,True);dn='direct-50k-combined'
 if v['throughput_per_second']>=49500:
  para(f'이 실행은 50k를 유지했다. Direct에서 옵션 한 줄을 바꿔 reply packet 수를 {100*(1-x["per_request"]["data_segs_out"]/conn(dn,True)["per_request"]["data_segs_out"]):.1f}%, target send 비용을 {100*(1-send(n)/send(dn)):.1f}% 줄였다. 송신 비용은 {send(n)/1092:.3f} cycles/B로 baseline binding {send(bind)/1252:.3f}보다 낮다. Packet당 비용은 {f(send(n)/x["per_request"]["data_segs_out"])} cycles로 거의 같다. 이것이 packet 수가 비용의 원인이라는 개입 실험이다. Native descriptor와 payload, request window, rate, timeout, binding/native library는 바꾸지 않았다.')
  para('기존 binding의 tcpNoDelay(false)는 이 backpressure 조건에서 추가 TCP 비용을 제거할 수 있는 최소 Framework-side 변경이다. Core/kernel의 기존 coalescing 기능을 사용한다. Nagle 사본의 BPF에서도 partial write·send EAGAIN은 0, nonagle=0, SO_SNDBUF=4,194,304 B, GSO는 모든 관측 skb에서 1 segment/size 0이었다. Sender 평균 write는 약 1,895 B지만 TCP data segment 평균은 약 13,116 B여서 여러 작은 write가 합쳐졌다. 새 queue·timer·lock을 만들지 않았다.')
  para(f'평균 latency는 direct보다 {v["latency_mean_ms"]/cell(dn)["latency_mean_ms"]:.2f}배, p95는 {v["latency_p95_ms"]/cell(dn)["latency_p95_ms"]:.2f}배 높았다. 이 값은 request window 100 조건의 결과다. 모든 요청 패턴에 이 옵션을 기본으로 켜야 한다는 결론은 내리지 않는다. 이 사본의 F÷B는 TCP_NODELAY=1인 baseline binding과의 측정값이며, binding도 같은 옵션으로 바꾼 동일 TCP 정책 비교는 아니다.')
 else:
  para('옵션 시험은 목표 50k를 유지하지 못했으므로 비용 제거 방안으로 채택하지 않는다. 실제 rate의 수치만 기록했다.')
table(['실패에 관여한 결정','현재 판정 위치','spec의 소유','하나로 모을 소유자'],[
 ['Reply record 제출 직렬화','Java ZLinkJavaRawServicePort.java:524(수정 전); C++ raw_route_port.cpp:501; .NET ZLinkRawRouterServicePort.cs:237; Node node-raw-binding-port.ts:253','Core socket §2 + binding Multipart 제출','whole-message Core/binding admission'],
 ['Reply token/route 선택','Java NativeRouterSocket.java:90 및 네 언어의 public reply submit','binding Java Raw reply + Core request/reply','Core/binding token과 현재 Completion connection'],
 ['TCP packet 합치기','네 언어가 공용 libzlink/TCP 사용; 시험은 Java ZLinkJavaRawServicePort.java:180의 tcpNoDelay 옵션만 선택','기존 public binding TCP option','Core/kernel TCP']])
para('결정 소유를 대조한 위치는 Java ZLinkJavaRawServicePort.java:524 및 NativeRouterSocket.java:90, C++ raw_route_port.cpp:501, .NET ZLinkRawRouterServicePort.cs:237, Node node-raw-binding-port.ts:253이다. Java의 추가 reply state-lane 판정과 binding token/admission 판정이 겹쳤다. C++는 socket lifetime mutex 안에서 같은 public binding submit을 호출하고 .NET·Node는 직접 submit한다.')
para('소유 계층은 Core/binding의 same-handle record admission·reply token·연결 라우팅·close gate다. Framework는 typed handler, logical reply winner, message lifetime을 유지한다.')
para('기준 spec은 core/doc/spec/core/socket/README.ko.md §2, bindings/doc/spec/README.ko.md의 Multipart 제출, bindings/doc/spec/java/README.ko.md의 Raw reply one-shot, bindings/doc/spec/async-execution-model.ko.md §5다. Framework 실행 자원 재량은 handler-turn-and-execution-gate §12를 따른다.')
para('교차언어 대조에서는 네 언어 모두 public binding reply를 한 번 제출한다는 결과 계약이 같았다. Java state-lane 경합을 제거했으며 다른 언어 구현·spec을 수정하지 않았다. 정식 cross-language, close 경합, 전체 contract gate는 이 측정에서 실행하지 않았다.')
para('변경 분류는 A, 기존 binding의 whole-message concurrent admission과 one-shot reply 계약에 대한 Framework 적용이다. 기존 descriptor wire 자체는 사용자 승인 범위의 측정용 프로토타입이며 배포·commit 대상이 아니다.')
para('리팩토링 점검에서 발견 1항목, 수정 1항목, 범위 밖 이관 0항목이다. 중복 reply gate/port-map 판정을 제거했다. 추가한 Framework queue·lock·cache·persistent state는 0이다. Reply 제출 판정 위치는 Framework port+Core/binding의 2곳에서 Core/binding 1곳으로 줄었다. 다른 호출의 state lane은 유지했다. Nagle 시험은 기존 TCP 옵션 한 줄만 바꾸며 새 상태를 추가하지 않았다. 변경 파일은 framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java 하나다. 최종 diff는 change.diff이고 직접 제출과 옵션 시험은 direct.diff·nagle.diff에도 분리했다.')
para('남은 미해명 항목은 원래 단일 실행의 정확한 3.00배 값, Framework의 어떤 dispatch 타이밍이 reply 묶음을 얼마나 분산하는지, record별 producer CPU→sender CPU/cache 이동 비용이다. 관측한 50k 비교에서는 packet 수가 send 비용 차이의 약 92–93%를 설명했지만 kernel packet당 잔여 차이를 특정 cache·SMT 기전으로 확정하지 않았다. Receive GRO merge/split과 record 경계별 syscall 분포도 직접 세지 않았다. Source request-send 방향의 packet당 비용 차이도 완전히 분리하지 않았다. Binding 기능만으로 packet 합치기가 가능하다는 것과, 모든 요청 패턴에 유리한 Framework 기본값이라는 것은 별도로 검증해야 한다.')
para(f'유효 실행 25회는 모두 submitted=completed=target received이고 errors=abandoned=0이었다. Validator는 정상 결과를 통과시키고 count 누락·error·abandoned를 각각 넣은 음성 fixture 3개를 모두 거부했다. 관측 최소 MemAvailable은 {valid["min_memavailable_kb"]/1048576:.2f} GiB다. 측정은 모두 `flock -o /root/jobs/fx/cx/measure.lock`의 배타 lock 안에서 실행했고 공유 빌드는 지정 wrapper를 사용했다. 종료는 SIGTERM/SIGINT를 사용했다. 두 계측기 준비 실패 실행은 preflight로 보존하고 결과 표에서 제외했다. 남은 실행 실패는 없다. Git은 status/diff만 사용했고 Core·binding·spec·GitHub를 수정하지 않았다.')
para('재현 자료는 runs/*/results.json, runner-meta.json, command JSON, wire.pcap, capture/*.perf.data, *.cycles.csv, ss.json, 각 BPF program.bt·log·window.json이다. latency는 active 시작 뒤 처음 최대 200,000개 완료 표본의 값이며 60초 전체의 균일 표본이 아니다. 요약은 throughput-summary.json, perf-summary.json, aligned-ss-summary.json, net-summary.json, syscall-details.csv, byte-histograms.csv, handoff-summary.json, validation.json에 있다. 실행·빌드 도구는 runner/와 tools/에 보존했다.')
# Correct a script-local non-Korean fragment before writing.
text='\n'.join(lines).replace('Reply producerからIO/1へのlogical handoff','Reply producer에서 IO/1로 넘기는 logical handoff')
(P/'report.md').write_text(text)
print('report lines',len(lines))
