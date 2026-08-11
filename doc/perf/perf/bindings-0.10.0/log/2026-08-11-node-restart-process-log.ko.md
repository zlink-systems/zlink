# Node 재측정 작업 log

## 진행 원칙

Node는 inventory gate부터 Single, Multi 순서로 다시 측정한다. 각 transport와 pattern은 C를
먼저 단독 실행하고 종료한 뒤 Node를 단독 실행한다. 자체 성능 개선과 POSDDD 리팩터링을 함께
검토하고, 의미 있는 구조 변경 후보는 Sol 리뷰를 거친다. Sol 리뷰는 저위험 변경만 고르는
절차가 아니다. Public interface를 유지하면서 hot path, 경계 호출, allocation과 ownership
구조에서 측정치를 유의미하게 높일 후보를 찾고, 변경 위험은 측정과 test로 판단한다. Public
interface와 Message ownership, C perf의 측정 의미는 변경하지 않는다.

## Single PAIR / tcp

최초 paired 측정에서 Node/C throughput 산술평균은 `66.375%`, latency ratio 중앙값은
`31.349x`였다. Sol 리뷰는 ROUTER에만 있던 bounded native receive를 일반 MessageSocket으로
확장하는 후보를 제안했다.

일반 receive는 N-API 호출 한 번에 준비된 message를 최대 16개, 1MiB, 1ms 범위에서 받고
public `recv(Received, flags)`는 기존처럼 message 하나씩 반환한다. ROUTER와 일반 socket이
중복해서 관리하던 pending queue, poll readiness와 close cleanup은 `BufferedReceiveQueue`가
소유하도록 통합했다. 이 리팩터링은 batching과 lifecycle 지식을 한 내부 module에 숨긴다.

Node 전체 test와 sample이 통과했고 PAIR batch order와 buffered poll readiness test를 추가했다.
최종 Node/C throughput ratio는 `12.871% / 25.751% / 44.534% / 143.393% / 101.823% /
80.618%`, 산술평균 `68.165%`다. 최초 Node baseline 대비 throughput 산술평균은 `102.724%`다.
Latency ratio 중앙값은 `30.271x`로 상한 `5x`를 넘는다. Native receive batching과 공통 queue
구조를 적용한 뒤에도 single JS event loop의 per-message materialization과 backlog가 남으며,
public batch interface 없이 추가로 경계 호출 수를 줄일 의미 있는 후보가 없어 `보류`한다.

- C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_174312_node-restart-pair-tcp-c.txt`
- Node baseline: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_174355_node-restart-pair-tcp.txt`
- Node batching A/B: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_174919_node-restart-pair-tcp-batched-recv.txt`
- Node 최종: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_175116_node-restart-pair-tcp-batched-posddd-final.txt`

## Single PUBSUB / tcp

Baseline Node/C throughput 산술평균은 `77.060%`, latency ratio 중앙값은 `1.780x`로 기준을
충족했다. 통과 뒤에도 개선과 POSDDD 구조를 검토했으며, Sol은 Subscriber의 message별 N-API
호출을 bounded native prefetch로 바꾸고 공통 `BufferedReceiveQueue`를 사용하는 후보를 GO로
판정했다.

Topic, source routing-id와 multipart frame은 다음 Core receive 전에 native envelope에 고정한다.
Topic buffer가 부족한 `EMSGSIZE` 경로는 frame을 유실하지 않고 buffer를 늘려 다시 받는다.
Public `subscribe`는 기존처럼 topic message 하나만 반환하고, pending queue와 poll readiness,
socket close cleanup은 일반 socket과 ROUTER가 사용하는 같은 내부 module이 담당한다.

전체 Node test와 sample이 통과했다. 최종 Node/C throughput ratio는 `27.966% / 45.947% /
62.436% / 135.835% / 109.622% / 85.076%`, 산술평균 `77.814%`다. Baseline 대비 throughput
산술평균은 `101.059%`, latency ratio 중앙값은 `1.919x`다. 성능 기준을 충족하고 공통 receive
lifecycle로 구조를 단순화하므로 후보를 유지한다.

- C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_175308_node-restart-pubsub-tcp-c.txt`
- Node baseline: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_175349_node-restart-pubsub-tcp.txt`
- Node 최종: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_175620_node-restart-pubsub-tcp-batched-final.txt`

## Single DEALER_DEALER / tcp

PAIR에서 적용한 일반 socket bounded native receive와 공통 buffered receive queue를 같은 public
recv hot path에서 사용했다. Node/C throughput ratio는 `10.233% / 23.094% / 43.175% /
143.583% / 105.659% / 94.226%`, 산술평균 `69.995%`다. Latency ratio 중앙값은
`3.848x`다. 처리량 목표 `60%`와 latency 상한 `5x`를 모두 충족한다. Pattern 전용 구조를
추가하지 않고 공통 receive module을 유지한다.

- C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_175750_node-restart-dealer-dealer-tcp-c.txt`
- Node: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_175817_node-restart-dealer-dealer-tcp-batched.txt`

## Single DEALER_ROUTER / tcp

최초 Node/C throughput 산술평균은 `26.392%`, latency ratio 중앙값은 `5.011x`였다. 자체
hot path 검토에서 재사용 `Received`마다 routed send/reply context와 closure를 다시 만들던
비용을 제거했고, ROUTER prefetch 상한을 이전 A/B 근거가 있는 64개로 조정했다. 최종 throughput
산술평균은 `30.740%`, latency ratio 중앙값은 `4.938x`다. 최초값 대비 throughput은
`116.48%`다.

Sol은 변경 위험을 제한하지 않고 allocation과 ownership 구조를 검토해 routed metadata lazy
materialization을 제안했다. Public property와 ownership을 유지하는 후보를 적용했지만 throughput
평균 `29.268%`, latency 중앙값 `6.104x`로 하락해 제거했다. 자체 개선과 의미 있는 Sol 구조
후보를 모두 측정했으나 routed one-way 최소 `33%`에 미달하므로 최종 후보를 유지하고 보류한다.

- C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_175859_node-restart-dealer-router-tcp-c.txt`
- Node baseline: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_175927_node-restart-dealer-router-tcp-batched.txt`
- context 재사용: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_180856_node-restart-dealer-router-tcp-context-cache.txt`
- 최종: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_181227_node-restart-dealer-router-tcp-context-cache-batch64.txt`
- 제거한 Sol 후보: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_181449_node-restart-dealer-router-tcp-lazy-route.txt`

## Single DEALER_ROUTER_REQREP / tcp

최초 Node/C throughput 산술평균은 `23.177%`, latency ratio 중앙값은 `4.322x`였다. 자체
검토에서 request builder마다 만들던 socket-capturing invoker를 socket별 고정 함수로 바꿔
`23.402%`까지 개선했다.

Sol 리뷰는 저위험 변경으로 범위를 제한하지 않고 실제 framework의 concurrent request에도
적용되는 completion 경계를 검토했다. Native request마다 TSFN을 만들던 구조를 socket-scoped
dispatcher로 바꾸고, 대기 없이 이미 준비된 completion을 최대 64개까지 한 JS 진입에서
처리하도록 구현했다. 최종 throughput 산술평균은 `24.710%`, latency ratio 중앙값은
`4.026x`이며 최초 대비 throughput은 `107.557%`다. Reply를 native frame으로 직접 넘기는
후보는 `64B`와 전체 latency가 악화되어 제거했다. 자체 후보와 Sol 구조 후보를 적용한 뒤에도
socket request/reply 최소 `30%`에 미달해 보류한다.

- C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_181716_node-restart-dealer-router-reqrep-tcp-c.txt`
- Node baseline: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_181737_node-restart-dealer-router-reqrep-tcp-context64.txt`
- 자체 invoker: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_181833_node-restart-dealer-router-reqrep-tcp-dealer-invoker.txt`
- socket dispatcher: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_182455_node-restart-dealer-router-reqrep-tcp-coalesced-dispatcher.txt`
- 제거한 native-frame 후보: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_182631_node-restart-dealer-router-reqrep-tcp-native-frame-completion.txt`
- 최종: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_182735_node-restart-dealer-router-reqrep-tcp-dispatcher-final.txt`

## Single ROUTER_ROUTER / tcp

기준 C와 현재 Node의 paired 측정에서 Node/C throughput 산술평균은 `29.233%`였고,
latency 중앙값은 `78.686x`였다. 자체 hot path 검토로 routed send builder가 매 submit 때
만들던 socket-capturing closure와 route normalization을 builder 생성 시점으로 옮겼다. 재사용
`Received`는 같은 routing bytes에 대해 immutable `RoutingId` facade를 재사용한다. 이 후보는
throughput 산술평균 `31.437%`, latency 중앙값 `69.553x`로 개선됐다.

Sol은 64개를 받는 native batch가 각 message의 JS raw object, routing Buffer와 BigInt를 모두
만든 뒤 첫 `recv`를 반환하는 receive backlog를 성능 병목으로 확인했다. public interface를
변경하지 않는 범위에서 batch 크기를 `4`, `8`, `16`, `32`로 각각 단독 측정했다. throughput
산술평균은 각각 `31.102%`, `30.624%`, `30.369%`, `31.873%`였다. `32`가 가장 높고 64개 후보보다
개선됐으므로 채택한다. 최종 latency 중앙값은 `60.398x`로 개선됐지만 Node 상한 `5x`를 넘는다.

자체 builder/ownership 개선과 Sol이 지적한 receive backlog 후보를 모두 수치로 비교했다. 더 큰
native compact cursor 재설계는 현재 후보 대비 효과를 보장하지 않는 내부 구조 확대가 되므로 이
pattern에서는 채택하지 않는다. routed one-way 최소 `33%`에도 미달하므로 `보류`한다. public
interface, routing ownership, message 순서와 C perf 의미는 변경하지 않았다. 전체 Node test와
sample은 released Core `0.10.1` package 조건에서 통과했다.

- C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_182912_node-restart-router-router-tcp-c.txt`
- Node baseline: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_182942_node-restart-router-router-tcp-current.txt`
- 자체 후보: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_183136_node-restart-router-router-tcp-routed-builder.txt`
- Sol A/B: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_183740_node-restart-router-router-tcp-batch4.txt`, `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_183819_node-restart-router-router-tcp-batch8.txt`, `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_183847_node-restart-router-router-tcp-batch16.txt`
- 최종: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_183913_node-restart-router-router-tcp-batch32.txt`

## Single ROUTER_ROUTER_REQREP / tcp

C paired 기준에서 현재 Node/C throughput 산술평균은 `24.710%`였고 latency 중앙값은
`3.595x`였다. 자체 candidate인 Router request route normalization의 builder-time 고정은 전체
throughput을 낮춰 제거했다.

Sol은 payload 크기와 무관하게 반복되는 약 1.1ms latency를 reply 생성과 request completion의
N-API 경계 비용으로 분리했다. 첫 후보는 `Received.reply(Buffer)`가 Buffer를 Message로 만든 뒤
다시 native frame으로 보내던 중간 단계를 없앤 것이다. public reply builder는 Buffer 또는 Message를
그대로 보관하고 native submit 직전에 한 번만 normalize하며, 성공 시 Message만 consume한다. 이
후보는 throughput 산술평균 `27.302%`를 기록했다.

두 번째 후보는 socket별 token completion sink다. native request state는 request마다 callback
reference 대신 token만 보관하고, 최대 64개 completion을 한 JS handler 호출로 전달한다. TypeScript
dispatcher가 token별 callback과 progress release를 소유한다. submit failure에서는 token을 즉시
제거하고, public callback signature·순서·Message ownership은 유지한다. 최종 평균은 `27.131%`,
latency 중앙값은 `3.679x`다. 두 구조 후보 모두 실제 request/reply 경계 비용을 줄이지만 socket
request/reply 최소 throughput `30%`에는 미달하므로 보류한다. 전체 Node test와 sample은 released
Core `0.10.1` package 조건에서 통과했다.

- C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_184251_node-restart-router-router-reqrep-tcp-c.txt`
- Node baseline: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_184314_node-restart-router-router-reqrep-tcp-current.txt`
- 제거한 자체 후보: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_184422_node-restart-router-router-reqrep-tcp-invoker.txt`
- direct reply: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_184942_node-restart-router-router-reqrep-tcp-direct-reply.txt`
- 최종: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_185740_node-restart-router-router-reqrep-tcp-token-completion-final.txt`

## Single PAIR / ws

C와 Node를 순차 paired 측정했다. Node/C throughput ratio는 `13.519% / 28.536% / 65.495% /
135.263% / 130.454% / 106.890%`, 산술평균 `79.858%`이며 latency ratio 중앙값은 `1.211x`다.
tcp에서 검토·적용한 bounded native receive와 공통 `BufferedReceiveQueue`가 같은 public recv
hot path에 적용된다. 단순 one-way 최소 `35%`, 평균 목표 `60%`, latency 상한 `5x`를 충족하므로
추가 pattern 전용 변경 없이 통과한다.

- C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260811_185936_node-restart-pair-ws-c.txt`
- Node: `/home/hep7hep7/project/zlink/bindings/node/perf/results/single/report/perf_node_single_linux_20260811_190003_node-restart-pair-ws-current.txt`

## Single ws·wss 최신 paired 측정

모든 항목은 C를 먼저 실행하고 Node를 뒤이어 단독 실행했다. 실행 조건은 1초·1회, Auto-HWM, 64·256·1024·65536·131072·262144B다.

| Transport | Pattern | throughput 평균 | latency 중앙값 | 판정 | C / Node report |
|-----------|---------|----------------:|---------------:|------|-----------------|
| `ws` | `PUBSUB` | 89.219% | 0.890x | 통과 | `190109_node-restart-pubsub-ws-c` / `190159_node-restart-pubsub-ws-current` |
| `ws` | `DEALER_DEALER` | 80.538% | 1.090x | 통과 | `190215_node-restart-dealer-dealer-ws-c` / `190242_node-restart-dealer-dealer-ws-current` |
| `ws` | `DEALER_ROUTER` | 44.919% | 1.940x | 통과 | `190253_node-restart-dealer-router-ws-c` / `190320_node-restart-dealer-router-ws-current` |
| `ws` | `DEALER_ROUTER_REQREP` | 41.765% | 1.930x | 통과 | `190335_node-restart-dealer-router-reqrep-ws-c` / `190355_node-restart-dealer-router-reqrep-ws-current` |
| `ws` | `ROUTER_ROUTER` | 47.747% | 1.960x | 통과 | `190406_node-restart-router-router-ws-c` / `190434_node-restart-router-router-ws-current` |
| `ws` | `ROUTER_ROUTER_REQREP` | 41.495% | 1.890x | 통과 | `190446_node-restart-router-router-reqrep-ws-c` / `190507_node-restart-router-router-reqrep-ws-current` |
| `wss` | `PUBSUB` | 116.722% | 0.707x | 통과 | `190604_node-restart-pubsub-wss-c` / `190806_node-restart-pubsub-wss-current` |
| `wss` | `DEALER_DEALER` | 103.527% | 0.840x | 통과 | `190812_node-restart-dealer-dealer-wss-c` / `190840_node-restart-dealer-dealer-wss-current` |
| `wss` | `DEALER_ROUTER` | 75.252% | 1.234x | 통과 | `190845_node-restart-dealer-router-wss-c` / `190911_node-restart-dealer-router-wss-current` |
| `wss` | `DEALER_ROUTER_REQREP` | 68.236% | 1.122x | 통과 | `190916_node-restart-dealer-router-reqrep-wss-c` / `190936_node-restart-dealer-router-reqrep-wss-current` |
| `wss` | `ROUTER_ROUTER` | 76.697% | 0.910x | 통과 | `190941_node-restart-router-router-wss-c` / `191008_node-restart-router-router-wss-current` |
| `wss` | `ROUTER_ROUTER_REQREP` | 73.467% | 1.120x | 통과 | `191012_node-restart-router-router-reqrep-wss-c` / `191035_node-restart-router-router-reqrep-wss-current` |
| `tls` | `PAIR` | 91.899% | 2.927x | 통과 | `191414_node-restart-pair-tls-c` / `191441_node-restart-pair-tls-current` |
| `tls` | `PUBSUB` | 101.446% | 1.458x | 통과 | `191448_node-restart-pubsub-tls-c` / `191528_node-restart-pubsub-tls-current` |
| `tls` | `DEALER_DEALER` | 90.274% | 2.003x | 통과 | `191536_node-restart-dealer-dealer-tls-c` / `191602_node-restart-dealer-dealer-tls-current` |
| `tls` | `DEALER_ROUTER` | 66.018% | 4.444x | 통과 | `191608_node-restart-dealer-router-tls-c` / `191636_node-restart-dealer-router-tls-current` |
| `tls` | `DEALER_ROUTER_REQREP` | 64.661% | 1.650x | 통과 | `191648_node-restart-dealer-router-reqrep-tls-c` / `191708_node-restart-dealer-router-reqrep-tls-current` |
| `tls` | `ROUTER_ROUTER` | 65.329% | 3.577x | 통과 | `191739_node-restart-router-router-tls-c` / `191806_node-restart-router-router-tls-current` |
| `tls` | `ROUTER_ROUTER_REQREP` | 58.360% | 1.725x | 통과 | `191810_node-restart-router-router-reqrep-tls-c` / `191830_node-restart-router-router-reqrep-tls-current` |
