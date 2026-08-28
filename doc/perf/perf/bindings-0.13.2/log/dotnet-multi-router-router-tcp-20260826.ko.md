# .NET Multi ROUTER↔ROUTER TCP 재검증 및 최적화

## 측정 범위

- Core: local `0.13.2`, revision `99164bdc3e` (dirty)
- suite: Multi
- patterns: `ROUTER_ROUTER_SENDSEND`, `ROUTER_ROUTER_REQREP`
- transport: `tcp`
- clients: 100
- server/client I/O threads: 4/4
- message sizes: 64, 256, 1024, 4096, 65536, 131072 bytes
- final measurement: 2 seconds × 3 runs, median

## 원인 분리

기존 40%대 판단에는 Single과 Multi 결과가 혼재돼 있었다. 먼저 동일 Multi manifest로 C와 .NET을
다시 짝지어 측정한 결과, 전송 완료 방식 자체가 40%대 회귀를 만든 것은 아니었다.

`send_ready`와 `send_complete` 전환점의 clean worktree A/B에서도 `send_complete`가 더 빨랐다.

| 방식 | SENDSEND 64B | REQREP 64B |
| --- | ---: | ---: |
| 과도기 `send_ready` | 26.366 Kmsg/s | 0.075 Kmsg/s |
| `send_complete` | 128.098 Kmsg/s | 57.714 Kmsg/s |

따라서 `send_ready` 복구는 채택하지 않았다. 0.10.1의 정상 `send_ready`와 현재 completion 경로를
비교해도 REQREP은 58.848→58.327 Kmsg/s(-0.89%)였고, SENDSEND 하락의 대부분은 동일 시점 C
Core의 하락과 함께 움직였다.

## 채택 후보

Multi helper의 `PerfSocketIo.SendAsync(IRouterSocket, RoutingId, Message, ...)`가 단일 메시지에도
매번 routed operation builder를 만들고 있었다. public `RouterSocket.SendAsync(routingId, message)`
단일 메시지 경로를 직접 호출하고, 즉시 완료는 동기 반환하며 실제 pending만 await하도록 변경했다.
public API, ownership, exactly-once completion, close/failure/cancellation 및 callback context는 바꾸지 않았다.

선택 크기 3-run median A/B:

| 패턴 | 크기 | builder | direct | 변화 |
| --- | ---: | ---: | ---: | ---: |
| SENDSEND | 64B | 126.777 | 133.385 | +5.21% |
| SENDSEND | 64KiB | 31.675 | 32.102 | +1.35% |
| SENDSEND | 128KiB | 19.525 | 19.212 | -1.60% (측정 변동) |
| REQREP | 64B | 53.229 | 55.876 | +4.97% |
| REQREP | 64KiB | 20.445 | 22.860 | +11.81% |
| REQREP | 128KiB | 13.900 | 14.993 | +7.86% |

내부 `Task<bool>`을 `ValueTask<bool>`로 바꾸는 별도 후보는 반복 측정에서 일관된 개선이 없어
완전히 되돌렸다.

## 최종 paired 결과

| 패턴 | C throughput | .NET throughput | 평균 비율 | 판정 |
| --- | --- | --- | ---: | --- |
| ROUTER_ROUTER_SENDSEND | 168.275 / 159.836 / 150.173 / 126.860 / 25.531 / 16.103 Kmsg/s | 134.362 / 130.727 / 126.812 / 116.625 / 35.624 / 20.417 Kmsg/s | 100.72% | 통과 |
| ROUTER_ROUTER_REQREP | 82.664 / 75.692 / 66.401 / 54.872 / 18.926 / 11.840 Kmsg/s | 55.680 / 53.203 / 51.706 / 50.487 / 21.694 / 14.239 Kmsg/s | 90.40% | 통과 |

모든 크기의 throughput 비율이 50% 이상이고 latency 비율은 3.0x 이하다.

## 증거와 검증

- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_114758_final-direct-fastpath-paired-r3.txt`
- .NET final: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260826_114946_final-direct-fastpath-paired-r3.txt`
- builder control: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260826_113923_current-builder-send-r3.txt`
- direct candidate: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260826_113636_current-direct-single-send-r3.txt`
- .NET perf build: 성공, warning/error 0
- .NET tests: 177/177 통과
