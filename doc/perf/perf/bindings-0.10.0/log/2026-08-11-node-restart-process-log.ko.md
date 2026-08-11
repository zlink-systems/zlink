# Node 재측정 작업 log

## 진행 원칙

Node는 inventory gate부터 Single, Multi 순서로 다시 측정한다. 각 transport와 pattern은 C를
먼저 단독 실행하고 종료한 뒤 Node를 단독 실행한다. 자체 성능 개선과 POSDDD 리팩터링을 함께
검토하고, 의미 있는 구조 변경 후보는 Sol 리뷰를 거친다. Public interface와 Message ownership,
C perf의 측정 의미는 변경하지 않는다.

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
