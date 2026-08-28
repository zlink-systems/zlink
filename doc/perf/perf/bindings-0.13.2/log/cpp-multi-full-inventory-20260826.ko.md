# C++ Multi 전체 inventory 측정 (2026-08-26)

## 목적과 조건

- local Core 0.13.2 (`99164bdc3e`, dirty working tree)
- C++ Multi, 100 clients (`STREAM` 포함)
- server/client I/O threads 4/4
- 2초, 3회, median
- transport: `tcp`, `tls`, `ws`, `wss`
- size: 64, 256, 1024, 4096, 65536, 131072 bytes
- pattern: `DEALER_DEALER`, `DEALER_ROUTER_SENDSEND`,
  `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_SENDSEND`,
  `ROUTER_ROUTER_REQREP`, `PUBSUB`, `STREAM`

## 최초 실패 원인과 수정

`DEALER_DEALER`는 첫 size 뒤 서버가 종료 조건을 받지 못해 runner가 정지했다.
클라이언트가 active backlog 뒤에 stop token을 enqueue한 직후 `linger=0`으로
socket/context를 파괴해 stop token이 유실될 수 있었기 때문이다. runner가 서버의 모든
size/metric 결과를 확인한 뒤 클라이언트에 `STOP`을 보내고, 클라이언트는 그때까지
socket/context를 유지하도록 lifecycle handshake를 추가했다.

4096B 이상에서는 routed builder의 동기 `submit()`이 `SNDTIMEO=200ms`와 HWM 압력에 걸려
`EAGAIN`을 fatal failure로 처리했다. 이 패턴의 실제 workload는 비동기 admission이므로
active send와 stop-token send를 public builder의 `async()` await로 바꿨다. HWM에서는
coroutine이 대기하고 admission 뒤 재개된다.

두 변경은 benchmark harness의 종료 lifecycle과 backpressure 소비 방식만 수정한다.
Core 또는 C++ public 계약, routing/failover 의미, payload ownership은 변경하지 않았다.

## 최종 실행 결과

| 범위 | 성공 | 실패 | RESULT | 상태 | 보고서 |
|---|---:|---:|---:|---|---|
| `DEALER_DEALER / tcp` | 6 | 0 | 30/30 | complete | `perf_cpp_multi_linux_20260826_124201_dealer-dealer-tcp-r3-async-handshake-final-20260826.txt` |
| `DEALER_DEALER / tls,ws,wss` | 18 | 0 | 90/90 | complete | `perf_cpp_multi_linux_20260826_124306_dealer-dealer-secure-ws-r3-async-handshake-final-20260826.txt` |
| 나머지 6 patterns × 4 transports | 144 | 0 | 720/720 | complete | `perf_cpp_multi_linux_20260826_124621_remaining-patterns-r3-core0132-local-20260826.txt` |
| **합계** | **168** | **0** | **840/840** | **complete** | |

## 판정 범위

이번 실행으로 C++ Multi 전체 inventory의 동작, 종료, 결과 완전성은 확인했다. 그러나 같은
revision/manifest의 C 전체 Multi 보고서가 없다. 최신 paired C 보고서는
`ROUTER_ROUTER_SENDSEND/REQREP / tcp` 두 패턴만 포함한다. 따라서 이 로그는
**C++ 측정 완료** 근거이며, C 대비 throughput/latency 통과·미달 판정은 아니다.

공식 성능 판정은 동일 revision에서 C를 먼저 전체 실행하고 C++를 다시 연속 실행한 paired
보고서가 생긴 뒤 표와 시트에 반영한다. 그 전까지 C++ Multi의 공식 비교 셀은 `미측정`으로
유지한다.
