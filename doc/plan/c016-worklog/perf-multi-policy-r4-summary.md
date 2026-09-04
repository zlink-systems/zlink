# C perf multi 정책 복원 결과

## 결과

`bindings/c/perf`의 multi runner를 단일 active 측정 모델로 복원했다. 각 client는 별도의
outstanding 상한 없이 `DONTWAIT` 제출을 계속하며, Core가 거절한 record만 원본 payload로
보관한다. `BACKPRESSURED`에는 `EAGAIN`과 0이 아닌 wait token이 필요하다. 이후 completion
queue를 `NO_DATA`까지 비우고, 같은 token·context·RID의 `WRITABLE`을 확인한 뒤 보관한
payload를 다시 제출한다.

Echo client는 client별 상한을 두지 않는다. 한 application thread가 특정 socket의 큰 HWM을
채우느라 수신을 굶기지 않도록, writable client마다 라운드당 한 번씩 제출하고 매 라운드에서
수신과 completion을 처리한다. Relay server는 reply queue의 head가 backpressure 상태여도
inbound request를 계속 받아 application FIFO에 보관한다. PUBSUB은 기본
`PERF_MULTI_PUBSUB_XPUB_NODROP=0`이며 receiver가 active window 안에서 받은 record를 센다.

`core/build`와 `core/build-dev`는 외부 symlink 상태 그대로 유지했다. C perf runner는
symlink된 `core/build`를 외부 소유 runtime으로 취급해 존재와 버전만 확인하며, symlink를
통해 Core를 빌드하지 않는다. 검증에 사용한 runtime은 `libzlink.so.0.17.0`이다.

## 변경 파일

- 측정·completion 정책:
  `multi/common/perf_multi_client_helpers.hpp`,
  `perf_multi_relay_server.hpp`, `perf_multi_socket_reqrep.hpp`,
  `perf_multi_echo_policy.hpp`, `perf_multi_metric_header.hpp`
- pattern 구현:
  `perf_multi_dealer_dealer_client.cpp`, `perf_multi_dealer_dealer_server.cpp`,
  `perf_multi_pubsub_client.cpp`, `perf_multi_pubsub_server.cpp`,
  `perf_multi_router_router_matched_client.cpp`
- runner·검증·설명:
  `run_benchmarks_multi.sh`, `run_comparison.py`, `README.md`,
  `multi/tests/test_perf_multi_metrics.cpp`

모든 tracked 변경은 `bindings/c/perf/**` 안에 있다. commit, push, reset, checkout, stash는
실행하지 않았다.

## 검증

| 검증 | 결과 |
|---|---|
| `cmake --build bindings/c/build -j3` | 통과 |
| `perf_multi_metrics_test` 반복 실행 | 5/5 통과 |
| CTest `perf_multi_metrics_test` | 1/1 통과 |
| 요청된 `ci_multi_smoke.sh` (`CCU=8`, `DUR=2`, 1024/65536B, DD/DR_SENDSEND/PUBSUB) | 6/6 cell, 30/30 metric 통과 |
| REQREP smoke (`CCU=8`, `DUR=2`, 1024/65536B, DR/RR) | 4/4 cell, 20/20 metric 통과 |
| 비교 측정 (`CCU=100`, `DUR=5`, TCP, 지정 pattern·size) | before 15/15, after 15/15 통과 |
| `bash -n`, Python AST parse, `git diff --check`, tracked scope 검사 | 통과 |

전체 빌드는 성공했다. GCC가 REQREP의 외부 `size_t` 입력 범위에 대해 기존
`-Wstringop-overflow` 정적 진단을 출력했으나 build 또는 smoke 실패는 없었다.

## 성능 비교

조건은 `CCU=100`, `DUR=5`, TCP, 1회 실행이다. Throughput 단위는 Kmsg/s(DD·PUBSUB) 또는
Kops/s(DR_SENDSEND), latency 단위는 ms이다.

| Pattern | Size | Throughput before | Throughput after | 변화 | Latency before | Latency after |
|---|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 582.664 | 1064.316 | +82.7% | 0.099599 | 0.186440 |
| DEALER_DEALER | 256 | 418.336 | 1045.032 | +149.8% | 0.099882 | 1.766758 |
| DEALER_DEALER | 1024 | 429.973 | 937.978 | +118.1% | 0.100573 | 1.211945 |
| DEALER_DEALER | 4096 | 252.514 | 385.696 | +52.7% | 0.151101 | 624.993380 |
| DEALER_DEALER | 65536 | 48.116 | 56.975 | +18.4% | 0.131446 | 45.341543 |
| DEALER_ROUTER_SENDSEND | 64 | 273.537 | 188.774 | -31.0% | 0.327955 | 1.932628 |
| DEALER_ROUTER_SENDSEND | 256 | 236.298 | 168.447 | -28.7% | 0.398380 | 2.566993 |
| DEALER_ROUTER_SENDSEND | 1024 | 199.364 | 228.746 | +14.7% | 0.418237 | 1.222663 |
| DEALER_ROUTER_SENDSEND | 4096 | 159.969 | 157.725 | -1.4% | 0.516872 | 1.357381 |
| DEALER_ROUTER_SENDSEND | 65536 | 29.068 | 32.251 | +11.0% | 1.703184 | 25.246926 |
| PUBSUB | 64 | 717.509 | 625.139 | -12.9% | 1.002294 | 1466.934428 |
| PUBSUB | 256 | 864.440 | 744.474 | -13.9% | 1.175569 | 1871.392545 |
| PUBSUB | 1024 | 108.840 | 870.594 | +699.9% | 1.288877 | 982.603704 |
| PUBSUB | 4096 | 7.380 | 691.270 | +9266.8% | 0.862051 | 443.901407 |
| PUBSUB | 65536 | 0.300 | 67.422 | +22374.0% | 1.464858 | 219.237347 |

Before latency는 별도의 in-flight 1 latency phase에서 측정됐고, after latency는 saturated
active traffic에서 받은 record의 queue 체류 시간을 포함한다. 따라서 latency 두 열은 측정
의미가 다르며 regression 비율로 해석할 수 없다. Throughput도 1회 실행값이므로 정책 복원 전후
경향을 확인하는 자료이며 안정적인 성능 판정에는 반복 측정이 필요하다.

원본 log:

- `perf-multi-policy-r4-before.log`
- `perf-multi-policy-r4-after-final.log`
- `perf-multi-policy-r4-reqrep-final.log`

남은 실패는 없다.
