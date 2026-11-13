# C++ completion dispatcher pool 2 / 4 / 8 측정 (#1639)

## 조건

같은 세션에서 base·pool8·pool4·pool2를 각각 3회 실행했다. Base는 기존 `psmeas-base` Release 빌드(HEAD `168c679590`), 나머지는 `perf/1639-pool-exp`의 로컬 커밋 `17c5a80757` 빌드다. 네 빌드는 Release `-O3 -DNDEBUG`, Core/binding 1.23.0, 같은 vcpkg prefix와 복사한 bench harness를 사용했다. Pool8/4/2는 `ZLINK_PERF_COMPLETION_POOL`을 source와 target 모두에 설정했다. Base는 변수를 설정하지 않았다.

bstage A/B/A/B 결과는 launch 방식 차이를 분리하지 못했고 app-worker 합계 차이가 5초 캡처 전후에 모두 존재한 TID 수에서 비롯됨을 확인했다. 따라서 psched8 Python supervisor 방식으로 통일했다. Source CPU는 0–7, target CPU는 8–15였다. 각 측정 호출은 `/root/jobs/fx/cx/measure.lock`으로 직렬화했다. PM은 `/pm/request` readiness 확인 후 30초 측정했고, 22k는 1 KiB request-backpressure의 `TARGET_RATE=48000` paced 실행에서 5초 window A를 수집했다. 이전 세션 값은 비교에 사용하지 않았다.

| Round | 실행 순서 |
|---:|---|
| 1 | base → pool8 → pool4 → pool2 |
| 2 | pool8 → pool4 → pool2 → base |
| 3 | pool4 → pool2 → base → pool8 |

## Framework throughput (1 KiB)

Base 자체 3회 범위: send-saturation **179231.141–184615.327 msg/s**, request-backpressure **53082.873–55037.694 msg/s**.

| Variant | Round | Send-saturation msg/s | Request-backpressure msg/s | Raw results |
|---|---:|---:|---:|---|
| base | 1 | 179,231 | 53,495 | [measure/1/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/1/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/1/base/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/1/base/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| base | 2 | 184,615 | 55,038 | [measure/2/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/2/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/2/base/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/2/base/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| base | 3 | 184,124 | 53,083 | [measure/3/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/3/base/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/3/base/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/3/base/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| pool8 | 1 | 185,489 | 53,884 | [measure/1/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/1/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/1/pool8/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/1/pool8/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| pool8 | 2 | 184,145 | 54,065 | [measure/2/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/2/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/2/pool8/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/2/pool8/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| pool8 | 3 | 182,035 | 54,670 | [measure/3/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/3/pool8/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/3/pool8/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/3/pool8/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| pool4 | 1 | 172,559 | 53,610 | [measure/1/pool4/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/1/pool4/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/1/pool4/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/1/pool4/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| pool4 | 2 | 177,118 | 54,339 | [measure/2/pool4/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/2/pool4/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/2/pool4/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/2/pool4/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| pool4 | 3 | 176,997 | 55,301 | [measure/3/pool4/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/3/pool4/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/3/pool4/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/3/pool4/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| pool2 | 1 | 175,091 | 54,840 | [measure/1/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/1/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/1/pool2/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/1/pool2/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| pool2 | 2 | 176,795 | 53,428 | [measure/2/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/2/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/2/pool2/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/2/pool2/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |
| pool2 | 3 | 183,951 | 56,387 | [measure/3/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json](measure/3/pool2/throughput-send-saturation/run1/zlink-framework-cpp-send-saturation-1024/results.json)<br>[measure/3/pool2/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json](measure/3/pool2/throughput-request-backpressure/run1/zlink-framework-cpp-request-backpressure-1024/results.json) |

## PM Spot (µs)

Base 자체 3회 범위: p50 **312.004–325.970**, p99 **495.702–607.418**.

| Variant | Round | p50 | p99 | Raw latencies |
|---|---:|---:|---:|---|
| base | 1 | 312.055 | 539.883 | [measure/1/base/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/1/base/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| base | 2 | 312.004 | 607.418 | [measure/2/base/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/2/base/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| base | 3 | 325.970 | 495.702 | [measure/3/base/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/3/base/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| pool8 | 1 | 305.131 | 472.410 | [measure/1/pool8/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/1/pool8/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| pool8 | 2 | 309.921 | 540.965 | [measure/2/pool8/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/2/pool8/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| pool8 | 3 | 301.815 | 657.419 | [measure/3/pool8/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/3/pool8/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| pool4 | 1 | 299.612 | 498.588 | [measure/1/pool4/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/1/pool4/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| pool4 | 2 | 308.037 | 509.117 | [measure/2/pool4/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/2/pool4/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| pool4 | 3 | 297.678 | 597.981 | [measure/3/pool4/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/3/pool4/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| pool2 | 1 | 306.604 | 488.509 | [measure/1/pool2/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/1/pool2/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| pool2 | 2 | 299.592 | 541.266 | [measure/2/pool2/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/2/pool2/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |
| pool2 | 3 | 302.297 | 516.500 | [measure/3/pool2/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json](measure/3/pool2/pm/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json) |

## 22k Framework request-backpressure

Base 자체 3회 범위: completion pool attribution **69.105–69.181 µs/msg**, voluntary wakes **2.306–2.338 /msg**; target/source I/O sendto **0.800–0.818 / 0.718–0.728 /msg**; target receive/app-worker **15.110–15.234 / 11.606–12.446 µs/msg** (app-worker 1/3 TID); target/source process CPU **55.697–57.739 / 99.380–99.808 µs/msg**; effective GHz **2.876–2.883 / 2.888–2.896**; process voluntary wakes **1.254–1.284 / 2.967–2.990 /msg**.

| Variant | Round | Pool CPU µs/msg; wakes/msg; TIDs | Target receive / app-worker µs/msg (matched/observed) | Process CPU target/source µs/msg (matched TIDs) | Raw capture |
|---|---:|---:|---:|---:|---|
| base | 1 | 69.118; 2.306; 35 residual | 15.193 (1/1) / 12.446 (1/3) | 57.739 / 99.808 (38/50) | [measure/1/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/1/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| base | 2 | 69.105; 2.324; 35 residual | 15.110 (1/1) / 11.606 (1/3) | 56.421 / 99.583 (38/50) | [measure/2/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/2/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| base | 3 | 69.181; 2.338; 35 residual | 15.234 (1/1) / 11.831 (1/3) | 55.697 / 99.380 (38/50) | [measure/3/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/3/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool8 | 1 | 30.724; 1.675; 8 named | 14.927 (1/1) / 21.699 (2/3) | 66.030 / 100.564 (39/50) | [measure/1/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/1/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool8 | 2 | 30.333; 1.662; 8 named | 15.500 (1/1) / 22.539 (2/3) | 67.553 / 100.462 (39/50) | [measure/2/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/2/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool8 | 3 | 30.329; 1.668; 8 named | 15.461 (1/1) / 11.352 (1/3) | 55.691 / 99.655 (38/50) | [measure/3/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/3/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool4 | 1 | 22.179; 1.319; 4 named | 14.284 (1/1) / 10.932 (1/3) | 53.145 / 87.344 (34/46) | [measure/1/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/1/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool4 | 2 | 22.490; 1.344; 4 named | 14.119 (1/1) / 21.105 (2/3) | 62.137 / 86.058 (35/46) | [measure/2/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/2/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool4 | 3 | 22.461; 1.310; 4 named | 13.930 (1/1) / 20.285 (2/3) | 61.993 / 87.121 (35/46) | [measure/3/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/3/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool2 | 1 | 12.830; 0.710; 2 named | 13.688 (1/1) / 9.879 (1/3) | 49.269 / 72.551 (32/44) | [measure/1/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/1/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool2 | 2 | 12.386; 0.677; 2 named | 13.041 (1/1) / 11.561 (1/3) | 49.238 / 68.951 (32/44) | [measure/2/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/2/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool2 | 3 | 12.942; 0.724; 2 named | 13.743 (1/1) / 20.365 (2/3) | 60.253 / 71.107 (33/44) | [measure/3/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/3/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |

| Variant | Round | I/O sendto target/source per msg | Effective GHz target/source | Voluntary wakes target/source per msg | Window A msg/s | Raw capture |
|---|---:|---:|---:|---:|---:|---|
| base | 1 | 0.812 / 0.728 | 2.88 / 2.89 | 1.256 / 2.967 | 32,227 | [measure/1/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/1/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| base | 2 | 0.818 / 0.724 | 2.88 / 2.89 | 1.254 / 2.982 | 32,321 | [measure/2/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/2/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| base | 3 | 0.800 / 0.718 | 2.88 / 2.90 | 1.284 / 2.990 | 32,270 | [measure/3/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/3/22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool8 | 1 | 0.797 / 0.723 | 2.88 / 2.89 | 1.339 / 2.923 | 32,240 | [measure/1/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/1/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool8 | 2 | 0.818 / 0.726 | 2.87 / 2.89 | 1.392 / 2.977 | 32,265 | [measure/2/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/2/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool8 | 3 | 0.810 / 0.718 | 2.87 / 2.89 | 1.280 / 2.974 | 32,378 | [measure/3/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/3/22k/pool8-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool4 | 1 | 0.793 / 0.703 | 2.93 / 2.94 | 1.238 / 2.605 | 33,525 | [measure/1/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/1/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool4 | 2 | 0.806 / 0.683 | 2.94 / 2.94 | 1.368 / 2.687 | 33,786 | [measure/2/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/2/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool4 | 3 | 0.787 / 0.711 | 2.93 / 2.94 | 1.308 / 2.570 | 33,457 | [measure/3/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/3/22k/pool4-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool2 | 1 | 0.770 / 0.687 | 3.02 / 3.02 | 1.232 / 1.957 | 34,034 | [measure/1/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/1/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool2 | 2 | 0.754 / 0.645 | 3.02 / 3.03 | 1.202 / 1.910 | 34,556 | [measure/2/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/2/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |
| pool2 | 3 | 0.771 / 0.672 | 3.02 / 3.03 | 1.309 / 2.029 | 34,217 | [measure/3/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture](measure/3/22k/pool2-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture) |

Pool2/4/8 CPU와 wakes는 source의 `zl-cmpl-*` TID를 합산했다. Base는 이름이 없어 source matched-TID 합계에서 `/IO/`, `ZLINKbg/Reaper`, `ZLINKbg/core-ct`, `zlink-req-time`, `zlink-timer`, process leader를 뺀 residual을 썼다. Base residual 35 TID에는 이름 없는 비-dispatcher thread가 섞일 수 있어 dispatcher 전용 합계로 보지 않는다. Process CPU/wakes는 before·after 두 snapshot에 모두 존재한 TID 합계다. App-worker는 로그에 기록된 3개 TID 중 snapshot 양쪽에 잡힌 TID만 합산했고, receive는 1/1이었다. 따라서 app-worker 값은 coverage 차이를 함께 보아야 한다.

I/O는 `/IO/` 이름의 thread 중 sendto가 가장 많은 thread다. Effective GHz는 그 thread의 `aperf / mperf × 2.0`이다. 22k 원자료의 `windows.json`, thread snapshots, `A-perfstat.csv`, `target.log`는 각 capture 경로에 보존했다.

## 결과 확인

24개 throughput 셀은 모두 clean이며 completed와 target received가 일치했다. PM은 12개 모두 Spot 250개를 수집했다. 22k는 12회 모두 source/target 완료·수신 수가 일치했다. `bench_cpp_client`, `bench_cpp_framework_server`, `bench_cpp_pm_server`는 수정 source보다 최신이고 각 binary에서 `ZLINK_PERF_COMPLETION_POOL`을 확인했다. 테스트는 실행하지 않았다.

작업 변경은 [provider_task_context.cpp](/root/worktree/zlink-v21-s033/pool/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp) 한 파일이며 로컬 commit은 `17c5a80757`이다. `.pool-build`는 복사한 실행 harness와 조건 기록에서 계속 참조하므로 보존했다.
