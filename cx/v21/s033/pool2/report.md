# Completion pool send-saturation 측정 (#1639)

## 조건과 판정 입력

1 KiB Framework `send-saturation`, 5초 활성 구간, 30초 warmup. 매 라운드 네 variant를 순차 실행하고 순서를 회전했다. Source는 CPU 0–7, target은 8–15에 고정했다. 매 벤치 실행은 `/root/jobs/fx/cx/measure.lock` 아래에서 이뤄졌다. Base는 기존 `psmeas-base/.psmeas-build`를 그대로 사용했다. Pool은 `.pool-build`에서 worker 수를 8/3/2로 설정했다. Pool variant queue log는 source와 target에서 각각 수집했다.

아래 throughput은 각 실행 `results.json`의 `throughput_per_second`다. Process CPU는 5초 window의 thread `schedstat` 합을 해당 window의 source 완료 메시지 수로 나눈 값이다. I/O 표시는 역할별 CPU가 가장 높은 I/O thread의 CPU 사용률과 effective GHz다. TID 계산은 before/after의 합집합을 썼다. After에만 있는 TID는 after 전체 counter를 포함하고, before에만 있던 TID 수는 매번 source 1·target 1이다.

| Round | Variant | msg/s | Process CPU µs/msg source/target | Queue depth p50/p99/max source | target | Busy I/O thread source/target CPU% @ GHz | Raw results |
|---:|---|---:|---:|---:|---:|---:|---|
| 1 | base | 192,756 | 4.649 / 9.802 | 미수집 | 미수집 | 75.1 @ 2.97 / 64.1 @ 2.99 | `measure/1/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 1 | pool8 | 195,483 | 4.688 / 9.991 | 1 / 4 / 7 | — / — / 0¹ | 77.6 @ 2.96 / 66.2 @ 2.98 | `measure/1/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 1 | pool3 | 178,832 | 4.426 / 9.636 | 1 / 5 / 7 | — / — / 0¹ | 66.4 @ 3.01 / 53.5 @ 3.04 | `measure/1/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 1 | pool2 | 191,311 | 4.304 / 9.478 | 1 / 6 / 7 | — / — / 0¹ | 71.3 @ 2.96 / 61.0 @ 2.97 | `measure/1/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 2 | base | 190,595 | 4.732 / 8.919 | 미수집 | 미수집 | 76.2 @ 2.98 / 64.7 @ 2.99 | `measure/2/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 2 | pool8 | 191,455 | 4.681 / 9.842 | 1 / 4 / 7 | — / — / 0¹ | 75.1 @ 2.97 / 64.9 @ 3.00 | `measure/2/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 2 | pool3 | 194,041 | 4.414 / 9.503 | 1 / 5 / 7 | — / — / 0¹ | 73.8 @ 2.96 / 59.2 @ 2.96 | `measure/2/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 2 | pool2 | 196,542 | 4.256 / 9.185 | 1 / 6 / 7 | — / — / 0¹ | 72.8 @ 2.95 / 58.4 @ 2.96 | `measure/2/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 3 | base | 194,350 | 4.587 / 9.804 | 미수집 | 미수집 | 76.4 @ 3.01 / 61.8 @ 3.02 | `measure/3/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 3 | pool8 | 191,578 | 4.770 / 10.125 | 1 / 4 / 7 | — / — / 0¹ | 78.2 @ 3.00 / 66.7 @ 3.02 | `measure/3/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 3 | pool3 | 189,894 | 4.391 / 9.710 | 1 / 6 / 7 | — / — / 0¹ | 71.7 @ 2.97 / 58.3 @ 2.98 | `measure/3/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 3 | pool2 | 185,898 | 4.446 / 9.869 | 1 / 6 / 8 | — / — / 0¹ | 71.9 @ 3.01 / 58.4 @ 3.02 | `measure/3/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 4 | base | 190,727 | 4.700 / 10.150 | 미수집 | 미수집 | 76.4 @ 3.01 / 65.4 @ 3.02 | `measure/4/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 4 | pool8 | 194,209 | 4.663 / 9.791 | 1 / 4 / 7 | — / — / 0¹ | 77.4 @ 3.01 / 62.2 @ 3.03 | `measure/4/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 4 | pool3 | 191,802 | 4.407 / 9.320 | 1 / 6 / 7 | — / — / 0¹ | 72.3 @ 2.98 / 57.8 @ 2.98 | `measure/4/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 4 | pool2 | 196,171 | 4.276 / 9.636 | 1 / 6 / 7 | — / — / 0¹ | 73.5 @ 2.95 / 62.7 @ 2.96 | `measure/4/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 5 | base | 190,189 | 4.715 / 9.854 | 미수집 | 미수집 | 75.4 @ 3.00 / 63.8 @ 3.01 | `measure/5/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 5 | pool8 | 196,865 | 4.682 / 9.705 | 1 / 4 / 7 | — / — / 0¹ | 78.0 @ 2.97 / 66.2 @ 2.99 | `measure/5/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 5 | pool3 | 191,407 | 4.346 / 9.514 | 1 / 6 / 7 | — / — / 0¹ | 71.6 @ 2.97 / 58.5 @ 2.98 | `measure/5/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |
| 5 | pool2 | 189,772 | 4.353 / 9.433 | 1 / 6 / 7 | — / — / 0¹ | 70.8 @ 2.97 / 56.4 @ 2.98 | `measure/5/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json` |

¹ Target queue histogram에는 관측값이 없고, 측정된 max counter가 0이다. p50/p99는 계산할 표본이 없다. Base는 worker 이름과 queue logger가 없는 빌드라 queue histogram을 얻지 못했다. `pool8`은 이름·로그가 있는 pool build의 기준 variant다.

## Completion worker별 CPU·wakes

표의 각 항목은 `CPU 사용률% / voluntary wakes per source 완료 메시지`다. 각 줄의 worker 번호 순서대로 기재했다. Target의 `zl-cmpl-N`은 pool variant 모든 실행에서 각각 0.00% / 0.0000이었다. Base는 `zl-cmpl` 이름이 없어 개별 pool worker를 식별할 수 없다.

| Round | Pool | Source `zl-cmpl-0, 1, ...` CPU% / wakes/msg | 원시 window snapshot |
|---:|---:|---|---|
| 1 | pool8 | 0.60/0.0017, 0.60/0.0017, 0.60/0.0017, 0.60/0.0017, 0.60/0.0017, 0.62/0.0017, 0.60/0.0017, 0.60/0.0017 | `measure/1/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 1 | pool3 | 0.93/0.0024, 0.92/0.0025, 0.94/0.0025 | `measure/1/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 1 | pool2 | 0.87/0.0020, 0.88/0.0020 | `measure/1/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 2 | pool8 | 0.66/0.0020, 0.65/0.0019, 0.64/0.0020, 0.65/0.0020, 0.65/0.0020, 0.65/0.0019, 0.64/0.0019, 0.64/0.0019 | `measure/2/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 2 | pool3 | 0.82/0.0022, 0.80/0.0022, 0.81/0.0021 | `measure/2/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 2 | pool2 | 0.77/0.0017, 0.77/0.0017 | `measure/2/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 3 | pool8 | 0.55/0.0016, 0.54/0.0016, 0.54/0.0016, 0.54/0.0016, 0.55/0.0017, 0.55/0.0016, 0.55/0.0016, 0.55/0.0016 | `measure/3/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 3 | pool3 | 0.82/0.0021, 0.80/0.0022, 0.82/0.0021 | `measure/3/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 3 | pool2 | 0.83/0.0020, 0.83/0.0019 | `measure/3/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 4 | pool8 | 0.55/0.0016, 0.55/0.0016, 0.54/0.0016, 0.55/0.0016, 0.54/0.0016, 0.55/0.0016, 0.54/0.0016, 0.55/0.0016 | `measure/4/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 4 | pool3 | 0.79/0.0019, 0.80/0.0019, 0.80/0.0019 | `measure/4/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 4 | pool2 | 0.76/0.0016, 0.76/0.0016 | `measure/4/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 5 | pool8 | 0.61/0.0017, 0.60/0.0017, 0.61/0.0017, 0.61/0.0017, 0.60/0.0017, 0.61/0.0017, 0.60/0.0017, 0.61/0.0017 | `measure/5/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 5 | pool3 | 0.76/0.0018, 0.76/0.0018, 0.76/0.0018 | `measure/5/pool3/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |
| 5 | pool2 | 0.97/0.0023, 0.95/0.0023 | `measure/5/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/capture` |

## 비교 입력과 큐 위치

5-run base 범위는 **190,189–194,350 msg/s**, median은 **190,727 msg/s**다. Pool median과 base 자체 범위를 대조하는 입력:

| Variant | 5-run median msg/s | Base 범위 내 median | Base 최소값 미만인 run |
|---|---:|---|---:|
| pool8 | 194,209 | 예 | 0/5 |
| pool3 | 191,407 | 예 | 2/5 |
| pool2 | 191,311 | 예 | 2/5 |

Queue log에서 표본은 source completion dispatcher에만 나타났다. Source p50은 모든 pool에서 1, p99는 pool8 4, pool3 5–6, pool2 6, max는 pool8 7, pool3 7, pool2 7–8이었다. Target에는 queue 시작 표본이 없었다. 이 계측이 보는 대기 지점은 [`provider_task_context.cpp:112`](/root/worktree/zlink-v21-s033/pool/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:112)의 `completion_dispatcher_t::post`와 [`provider_task_context.cpp:121`](/root/worktree/zlink-v21-s033/pool/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:121)의 worker handler 시작 사이이며, depth는 handler 시작 시 현재 handler를 뺀 미시작 completion 수다. Queue counter는 [`provider_task_context.cpp:117`](/root/worktree/zlink-v21-s033/pool/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:117), [`provider_task_context.cpp:123`](/root/worktree/zlink-v21-s033/pool/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:123)에서 갱신하고 histogram은 close 시 기록한다([`provider_task_context.cpp:95`](/root/worktree/zlink-v21-s033/pool/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:95)).

원시 queue log는 `measure/<round>/<variant>/queue/source-queue.tsv`와 `target-queue.tsv`에 있다. Queue histogram은 고정 5초 snapshot이 아니라 해당 dispatcher의 생성부터 close까지 수집한 값이다. CPU·wakes·process CPU와 I/O GHz는 고정 active window에서 측정했다. Queue log 사용 시 각 handler 시작에서 mutex/map histogram 갱신이 수행되므로 pool throughput에는 이 계측 비용이 포함된다. Log 환경변수가 없을 때는 post에서 relaxed atomic increment 1회, handler 시작에서 relaxed atomic decrement 1회만 추가되며 histogram lock/map/file 작업은 없다(위 counter 두 줄).

## 산출물과 검증

- Measurement source: [`provider_task_context.cpp`](/root/worktree/zlink-v21-s033/pool/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp). `.pool-build`의 `bench_cpp_client`, `bench_cpp_framework_server`가 source보다 최신이고 두 실행 파일 모두 pool·queue-log 환경 문자열을 포함한다.
- Union TID 규칙을 적용한 copied 22k analyzer: `/root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py`.
- Send-saturation collector/analyzer와 runner: `collect-send-saturation.py`, `analyze-send-saturation.py`, `run-B.sh`, `run-experiment.sh`.
- 20개의 원시 `results.json`, `samples.json`, thread TSV, perf CSV 및 pool queue TSV는 `measure/<round>/<variant>/` 아래 보존했다. 집계 JSON은 `measure/summary-send-saturation.json`이다.
- Bash/Python 구문 검사와 대상 빌드를 수행했다. 테스트는 실행하지 않았다. 벤치 20회 모두 exit 0, 완료/수신 일치 확인, perf 캡처 성공. Push·GitHub 작업 없음.
