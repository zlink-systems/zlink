# Node binding 메시지당 비용: L1/L2/L3

**“메시지당 N-API 경계 비용이 Node L2/L3 손실의 단일 원인”이라는 가설을 기각한다.** Send source의 addon 호출은 세 단계 모두 정확히 **3회/message**다. BP도 정상 submit·completion 경로는 **4회/message**다. L2는 target의 정상 addon 호출을 줄인다. 추가 CPU의 주된 관측 영역은 **JS/V8의 header 처리·frame 표현·객체 생성**이며, native Buffer view와 copy 비용도 별도로 존재한다.

HEAD `dc2eef3023`, branch `exp/4350-node-binding-cost`. 지정 build 두 단계 rc0, dist stale 0, addon SHA256 `ed260e4bb579…`. [빌드 지문](raw/build-fingerprint.json). Production·binding·bench tracked 변경 0. [호출 경로와 모든 경계](call-shape.md).

**조건과 단위.** 1K send-saturation/BP(request-backpressure), warmup 1s + active 제출 5s + drain. 계수·CPU·할당을 각각 별도 process에서 **2회** 수집했다. CPU는 wrapper 없이 `perf stat task-clock` + `perf record cpu-clock 399Hz/dwarf` + Node `--perf-basic-prof`; 네 collector가 disabled 상태에서 준비된 뒤 함께 enable한다. Heap은 collected-object sampling 8KiB, native는 LD_PRELOAD malloc/calloc/realloc/free counter다. 모든 36 cell/72 process에서 오류0·제출=완료=수신, 전체 thread CPU `6–7,14–15`, SCHED_OTHER, lifetime ≤14.35s. [모든 개별 수량·정책](raw/profile-audit.json).

**message당 전체 비용.** S=source, T=target. 각 `/`는 r1/r2이며 payload B의 `/`만 JS→native/native→JS다. NAPI는 **정상 message 경계 수 + 빈 poll·poller/event 조회의 추가 호출 r1/r2**다. CPU는 `JS/V8, addon, libzlink, kernel` µs 순서이며 `;` 앞뒤가 r1/r2다. Native A는 전체 process의 malloc+calloc+realloc 호출 수이며 addon 전용이나 live 객체 수가 아니다. JS heap은 누적 allocation의 sampling 추정 B다.

| 셀·역할 | 단계 | NAPI/message | payload copy B out/in | JS heap B r1/r2 | native A r1/r2 | CPU µs J,A,C,K: r1 ; r2 |
|---|---|---:|---:|---:|---:|---|
| send S | L1 | 3 + 0.00/0.00 | 1228/0 | 2790/2771 | 12.17/11.83 | 19.6,6.0,6.0,6.7 ; 10.8,3.9,6.4,6.5 |
| send T | L1 | 4 + 1.17/0.53 | 0/1027 | 3172/3709 | 15.60/15.90 | 12.4,15.4,4.1,2.9 ; 6.8,9.8,2.6,3.2 |
| send S | L2 | 3 + 0.00/0.00 | 1247/0 | 2979/2914 | 12.05/12.16 | 15.5,5.6,5.8,10.5 ; 16.5,5.6,6.3,14.2 |
| send T | L2 | 1 + 4.49/3.03 | 0/1247 | 3118/2579 | 3.97/3.59 | 13.8,5.8,4.8,5.1 ; 15.7,5.1,5.4,10.7 |
| send S | L3 | 3 + 0.00/0.00 | 2360/0 | 3361/3309 | 18.97/18.90 | 24.1,11.1,8.2,16.4 ; 27.3,10.9,7.6,13.9 |
| send T | L3 | 1 + 6.55/7.58 | 0/1333 | 3871/4030 | 4.96/4.95 | 27.1,6.2,7.5,9.4 ; 36.7,6.3,10.1,7.7 |
| BP S | L1 | 4 + 1.63/2.31 | 1228/1228 | 7340/7354 | 23.91/24.25 | 35.2,20.4,18.0,28.1 ; 34.2,20.3,23.6,42.8 |
| BP T | L1 | 7 + 3.30/5.20 | 1228/1027 | 10510/19232 | 30.43/36.99 | 31.8,22.6,16.8,24.9 ; 34.4,22.6,19.1,43.0 |
| BP S | L2 | 4 + 2.07/1.58 | 1299/1240 | 8378/8412 | 27.07/27.36 | 80.4,26.0,22.6,30.3 ; 64.4,22.3,20.9,32.1 |
| BP T | L2 | 2 + 12.09/12.96 | 1240/1299 | 6770/7073 | 10.54/10.71 | 86.9,15.0,34.1,35.2 ; 64.9,13.7,25.6,34.2 |
| BP S | L3 | 4 + 1.70/2.18 | 2420/1336 | 9671/9565 | 36.65/36.69 | 135.6,33.1,26.7,36.6 ; 68.0,27.7,25.7,40.8 |
| BP T | L3 | 2 + 14.56/14.31 | 1336/1393 | 9713/9608 | 13.13/13.00 | 118.9,15.9,27.9,32.9 ; 73.4,11.1,28.7,39.9 |

Routing copy는 위 payload와 별도다. Source 제출마다 JS→native **24B**, BP target reply마다 **28B**, source reply completion마다 native→JS **24B**가 더해진다. [routing 근거](raw/routing-copy-bytes.json). Target routingId는 warmup 뒤 재사용된다. Export별 원 횟수·callback·빈 결과는 [export-counts.tsv](raw/export-counts.tsv), malloc/calloc/realloc/free·native 요청 bytes는 [allocation-calls.tsv](raw/allocation-calls.tsv), 모든 추정치는 [per-message-summary.json](raw/per-message-summary.json)에 있다.

정상 native→JS allocator callback은 source send 0, source BP 2, target L1 0/L2·L3 2회다. Native가 만드는 객체·Buffer·bigint의 반환은 각 export 경계 안에 있으며 별도의 addon 호출로 중복 계수하지 않았다. L3 source는 `messageFrameClose` 1회를 `messageFrameData` 1회로 바꾼다. **호출 수가 같아도 반환 값·native allocation 비용은 같지 않다.**

**한 번의 native 제출·수신에 해당하는 CPU.** 아래는 해당 addon entry stack의 inclusive on-CPU µs/input-message다. Core의 동기 작업과 N-API 값 생성을 포함하고, JS codec·다른 IO thread는 제외한다. Receive/completion에는 해당 window의 빈 시도도 포함하므로 성공 호출 하나의 순수 latency가 아니다. L1 target `.data()`의 별도 copyData CPU는 send **3.61/2.35**, BP **3.47/4.08µs**이며 receive entry 열에 합치지 않았다.

| 1K 셀·단계 | source submit r1/r2 | target receive r1/r2 | target reply submit r1/r2 | source completion receive r1/r2 |
|---|---:|---:|---:|---:|
| send L1 | 6.59/4.14 | 12.99/7.86 | — | — |
| send L2 | 4.80/4.96 | 9.15/8.23 | — | — |
| send L3 | 7.25/8.65 | 11.30/14.06 | — | — |
| BP L1 | 12.75/13.16 | 16.95/16.92 | 8.70/8.27 | 11.06/12.78 |
| BP L2 | 18.48/15.40 | 29.44/21.48 | 14.80/13.64 | 14.61/13.13 |
| BP L3 | 24.37/17.31 | 24.87/21.85 | 13.64/9.93 | 12.25/11.87 |

**단계 추가 CPU: source+target 합계 µs/input-message.** N-API self는 leaf가 `napi_*`/N-API trampoline인 표본, copy self는 memcpy/memmove 계열 leaf 표본이다. 각각 경계 전체나 모든 inlined copy의 전수 비용은 아니다. J/addon 열에 이미 포함되는 부분이므로 아래 열들을 더하지 않는다.

| 단계 | ΔN-API self r1/r2 | ΔJS/V8 r1/r2 | Δaddon r1/r2 | Δcopy self r1/r2 | Δ전체 r1/r2 |
|---|---:|---:|---:|---:|---:|
| send L1→L2 | -1.80/-0.40 | -2.75/14.58 | -9.96/-2.97 | -0.01/0.12 | -6.10/29.49 |
| send L2→L3 | 0.97/0.95 | 21.95/31.83 | 5.93/6.46 | 0.48/0.88 | 43.10/41.01 |
| BP L1→L2 | 0.39/-0.49 | 100.20/60.62 | -2.01/-6.95 | 1.12/1.44 | 132.39/38.15 |
| BP L2→L3 | 2.07/0.43 | 87.21/12.10 | 8.01/2.84 | 1.56/0.52 | 97.24/37.10 |

- **L1→L2:** 추가 정상 JS→addon 호출 S **0**, T send **−3**/BP **−5**; target allocator callback은 **+2**다. BP의 JS/V8 증가 **100.20/60.62µs**와 addon 감소 **−2.01/−6.95µs**가 단일 경계 원인설과 맞지 않는다. 직접 식별한 envelope JS CPU만 source **12.55/11.21**, target **7.02/6.92µs**이며 L1은0이다. 원인은 `F/runtime/channels/channel-envelope.ts:124,471,521,658,886,960,1052`: correlation/deadline, template copy, reader·문자열·header 객체다. Codec 아래 sampled heap은 source **832/868B**, target **1012/1036B**이며 target에는 protobuf reply encode도 포함된다.
- **L2→L3:** 정상 호출·allocator callback 추가 **0**이다. JS pack copy는 send **1247B**, BP request **1299B**·reply **1240B**씩 추가된다. Boundary payload copy 증가는 send S out **1113B**/T in **86B**, BP S out/in **1121/96B**, T out/in **96/94B**다. 원인은 `F/runtime/foundation/service-wire-m6a-codec.ts:216,256,274,337,409`: frame 작성, 길이·text 검사, part view·wrapper 생성이다. L3 BP의 service-wire JS CPU는 S **5.92/4.49**, T **8.69/3.88µs**이며 L2는0이다. Source native allocation도 send **12.05/12.16→18.97/18.90**, BP **27.07/27.36→36.65/36.69회/message**로 늘었다.
- **경계의 실제 잔여 비용:** L3 source `messageFrameData` entry는 send **3.84/3.80**, BP **5.52/5.21µs**다. `bindings/node/native/src/addon_core.cc:1113`→`addon_message_values.h:125`의 external Buffer 생성·수명 관리 비용이다. `Message.allocate+data`가 copy를 줄이면서도 호출·값·owner 비용을 더할 수 있다는 기존 n2hdr/#4415 결과와 일치한다. 이 비용이 L2/L3 전체 추가 CPU를 설명하지는 않는다.

`F=framework/languages/node/packages/framework/src`. Caller 경계도 남아 있다. L3 source ladder는 native protobuf body를 먼저 만든 뒤 `.data()`로 pack에 사용한다(`framework/bench/grpc/node/shared/raw-wire.js:174`, `client/main.js:259,304`). 이 추가 view와 GC 수명 비용 전체를 production codec에 귀속하지 않는다.

**정밀 귀속의 한계.** CPU의 JS/V8 열에는 JIT·GC·libuv·확인되지 않은 native helper도 포함한다. Unresolved stack은 최대0.94%이며 codec 수치에 넣지 않았다. 공유 CPU와 1s warmup의 tiering 영향 때문에 send L1→L2 r1은 총 CPU가 감소했다. 복사한 `run-cell.sh` 그대로 L1에만 permit flag가 있고 L2/L3에는 없어 target permit/빈 poll 비용도 다르다. 따라서 위 관측 차이를 세 원인의 정확한 인과 합계나 공식 throughput 손실률로 바꾸지 않는다. **고정 경계 증가 0, 직접 관측한 JS codec CPU·heap 증가, copy byte 증가, addon CPU 감소/증가의 분리**까지가 확인된 결론이다.

**.NET 대조.** [d2l2](../d2l2/report.md)의 codec-only managed allocation은 source encode216+reply decode280=**496B**, target request decode328+reply encode216=**544B**이고 inbound header byte copy는0이다. Node의 위 target codec sampling에는 protobuf도 있어 객체 수의 직접 배율 비교는 하지 않는다. [d3l3](../d3l3/report.md)의 send1K CPU는 source submit12.55→14.61µs, finalizer0.20→0.86µs, target4.58→4.60µs였다. 해당 .NET ladder의 head 중복·미해제와 completion 표현 차이도 보고돼 있어 이 수치나 기존 2–5%를 Node의 순수 N-API 계수로 환산할 수 없다.

검증: known 8×1KiB 입력에서 from/data/copy/close 각 횟수와 양방향8192B 확인, 잘못된 Buffer 인자 거부·예외 계수 확인([raw](raw/preload-check/check.log)). Native counter는 알려진 malloc1001/calloc1000/realloc1000/free2000을 일치시켰다([raw](raw/counter-check-correct/summary.json)). 초기 collector·준비 실패와 제외 이유는 [excluded-runs.json](raw/excluded-runs.json)에 보존했다. 남은 유효 run의 오류0. Production/binding 수정·commit·push·GitHub·sub-agent·internal-docs·measure.lock 취득·SIGCONT 사용0. 판정 규칙 수 변경0, 성능·중복·수명 점검의 관측 사항은 위 call-shape에 기록했으며 구현 변경0이다.
