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
