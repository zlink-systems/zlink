# Node routed native frame pool 결과

## 확인한 조건

- branch: `core-0.10.0-bindings-performance`
- Core: `0.10.1` release runtime
- Node: `22.23.2`
- 대상: `MULTI_DEALER_ROUTER_SENDSEND / tcp`
- size: 64·256·1024·4096·65536·131072B
- clients: 100, duration: 2초, runs: 1, balanced auto-HWM
- 실행 순서: C 종료 후 Node 실행. 병렬 실행 없음

기존 측정은 package의 지원 범위인 Node 22 이상이 아니라 WSL system Node 18.19.1로 실행됐다.
Node 22.23.2를 사용자 cache에 설치하고 addon을 해당 ABI로 다시 build했다. single·multi perf
runner에는 Node 22 미만을 거부하는 guard와 실제 runtime metadata를 추가했다.

## C 의미 일치

C requester는 reply를 기다리는 socket에 `POLLIN`만 등록하고 실제 send backpressure가 발생한
socket에만 `POLLOUT`을 추가한다. C relay도 pending reply가 있을 때만 `POLLOUT`을 등록한다.
Node requester와 relay의 상시 `POLLOUT` 등록을 같은 조건부 등록으로 변경했다. 이 변경만 적용한
1초 선별 측정의 평균은 약 56%로 기존 55.81%와 비슷했지만, writable event busy poll을 제거해
C와 같은 의미가 되었으므로 유지했다.

## 후보 판단

`Received.send()`가 single-part payload를 scalar native API에 전달하도록 바꾸고 relay가 이 경로를
사용하는 후보는 64~4096B 처리량이 약 5~9% 하락해 원복했다. receive envelope용 builder/context
비용이 줄인 routing-id 전달 비용보다 컸다.

routed single-part receive는 payload copy를 피하려고 `native_message_frame_t`에 `msg_t` ownership을
옮긴다. 기존 구현은 매 recv마다 frame을 heap allocation했다. 참조가 0이 되어 `msg_t`를 닫은
frame을 최대 64개까지 JS thread별 pool에 보관하고 재사용하도록 변경했다. pool은 thread 종료 때
해제하며, 사용자가 참조 중인 frame은 반환되지 않으므로 재사용하지 않는다.

1초 선별 측정에서 Node 22 처리량은 64B 79,806→111,947 msg/s, 256B
62,795→108,655 msg/s로 상승했다. 1024B와 4096B도 각각 98,410, 95,032 msg/s로 상승했다.

## 정식 측정 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 176,719.0 | 107,965.5 | 61.09% |
| 256B | 179,737.0 | 102,777.0 | 57.18% |
| 1024B | 172,283.0 | 100,089.0 | 58.10% |
| 4096B | 144,481.5 | 92,472.5 | 64.00% |
| 65536B | 36,190.0 | 28,354.0 | 78.35% |
| 131072B | 21,012.0 | 15,503.5 | 73.78% |
| 산술평균 | - | - | 65.42% |

목표 60%를 통과했으므로 native frame pool을 채택했다.

- C report: `/tmp/zlink-node22-dr-frame-pool-c/multi/report/perf_c_multi_linux_20260813_054041_node22-dr-frame-pool-c.txt`
- Node report: `/tmp/zlink-node22-dr-frame-pool-node-final/multi/report/perf_node_multi_linux_20260813_054152_node22-dr-frame-pool-node-final.txt`

## 검증

- Node 22 addon Release build 통과
- TypeScript source/tools compile 통과
- Node raw test 전체 통과
- pool bound를 네 번 순환하는 256회 routed single-part ownership transfer test 통과
