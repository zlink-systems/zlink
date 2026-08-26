# Core Router POSDDD 구조·성능 검토 (2026-08-26)

## 범위

- local Core 0.13.2, branch `core-0.13.2-bindings-performance`
- C/C++ Multi `ROUTER_ROUTER_SENDSEND`, `ROUTER_ROUTER_REQREP`
- TCP, 100 clients, server/client I/O threads 4/4, 256B/4096B, 2초×3회 median
- 공개 send-completion, exact-target, byte-HWM 및 flow-state 계약은 유지

## 0.10.1 대비 Core 분리 측정

동일 C Multi runner로 Core 구간을 다시 측정했다. 0.10.1 대비 0.13.2는 SENDSEND의
256B/4096B가 86.15%/64.69%, REQREP가 82.19%/91.16%였다. 같은 날 앞선 0.13.2
재측정에서도 SENDSEND 159.836/126.860 Kops/s, REQREP 75.692/54.872 Kops/s로
0.10.1보다 낮아 C++ coroutine만으로 설명되지 않는 Core 차이가 확인됐다.

보고서:

- 0.10.1 C: `/home/hep7hep7/project/zlink-perf-0.10.1-final/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_135105_core-compare-v0101-router-router-tcp-r3-20260826.txt`
- 0.13.2 C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_135254_core-compare-v0132-router-router-tcp-r3-20260826.txt`

## 검토 후보와 판정

### A. completion callback TLS 경계

completion callback 재진입을 모든 공개 submit에서 `EDEADLK`로 막는 계약은 제거할 수 없다.
대신 anonymous free-function과 out-of-line query를 callback scope가 소유하는
`inline static thread_local` 상태로 응집했다.

대조 측정에서 SENDSEND는 +0.68%/+2.84%, REQREP는 -2.83%/-3.11%로 혼재해 성능
개선으로 판정하지 않는다. 그러나 TLS 상태의 소유권이 scope로 모이고 얕은 bridge가 제거되는
구조 개선이므로 유지한다.

- 후보:
  `perf_c_multi_linux_20260826_135844_ab-inline-completion-tls-bridge-r3-20260826.txt`
- 대조:
  `perf_c_multi_linux_20260826_140330_ab-control-outofline-completion-tls-r3-20260826.txt`

### B. send completion 책임 분리

1,065줄의 `socket_send_complete.cpp`가 queue 구동/완료 dispatch와 공개 submit/cancel의
validation·ownership transfer를 함께 소유했다. 이를 다음 두 deep module로 분리했다.

- `socket_send_complete.cpp`: pending queue, deadline 결과, admission driver, completion dispatch
- `socket_send_async_submit.cpp`: public submit/cancel, validation, immediate admission, message ownership transfer

외부 API와 런타임 계약은 바꾸지 않았으며 파일은 각각 610줄/520줄이 됐다.

### C. 단일 메시지 pipe fast path

정상 단일 메시지 경로가 multipart 누적값을 더하고 overflow를 재검사한 뒤 5개 상태를 매번
초기화하던 코드를 정리했다. multipart/owner 상태가 깨끗하다는 불변식을 fast-path 조건으로
명시하고, 조건이 맞지 않으면 기존 일반 상태 머신으로 내려간다. 정상 경로에서는 frame charge
한 번으로 admission·accounting을 끝낸다.

### D. 전역 pending head-of-line 차단

기존 최적화는 `pending_msgs != 0`이면 대상과 무관하게 immediate admission을 막았다.
따라서 대상 A가 HWM에 막혔을 때 독립적인 B도 op id 0으로 즉시 전송되지 않고 pending
callback 경로로 밀렸다. `test_send_complete_isolated_by_exact_target_and_terminal_cause`가
expected 0 / actual 2로 이를 재현했다.

대상별 `inline_attempts` reservation을 복원하고 전역 `admission_gate`는 실제 물리 제출
직렬화에만 사용하도록 책임을 분리했다. 같은 대상 FIFO는 유지하고 다른 대상의 backpressure는
서로 전파되지 않는다. 수정 후 exact-target 테스트 5/5와 async multipart 8/8이 통과했다.

### E. auto-HWM

0.13.2의 256B HWM을 0.10.1 수준으로 낮춘 실험에서 SENDSEND 160.306→165.055 Kops/s,
REQREP 80.989→88.453 Kops/s가 나와 HWM 정책이 일부 영향을 줄 가능성은 있다. 그러나
4096B는 양쪽 모두 1MiB인데도 135.352→145.980, 65.107→74.073 Kops/s로 바뀌어 run drift가
함께 섞였음이 확인됐다. profile의 resource/latency 의미를 바꿀 근거가 부족하므로 이번 pass에서는
Core 정책을 변경하지 않는다.

## 최종 측정

### C Multi

| Pattern | Size | 대조 Kops/s | 최종 Kops/s | 비율 |
|---|---:|---:|---:|---:|
| SENDSEND | 256 | 154.801 | 155.757 | 100.62% |
| SENDSEND | 4096 | 137.823 | 138.060 | 100.17% |
| REQREP | 256 | 78.444 | 77.123 | 98.32% |
| REQREP | 4096 | 69.424 | 63.344 | 91.24% |

최종 run은 load average가 더 높고 REQREP 4096B에 31.571 Kops/s outlier가 포함됐다.
따라서 단일 메시지 정리를 성능 향상으로 주장하지 않으며 구조 개선으로 채택한다.

최종 보고서:
`/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_141225_posddd-core-router-fastpath-r3-20260826.txt`

### C++ Multi

| Pattern | Size | 기존 0.13.2 Kops/s | 최종 Kops/s | 비율 |
|---|---:|---:|---:|---:|
| SENDSEND | 256 | 160.307 | 154.080 | 96.12% |
| SENDSEND | 4096 | 135.353 | 140.320 | 103.67% |
| REQREP | 256 | 80.989 | 82.244 | 101.55% |
| REQREP | 4096 | 65.107 | 69.680 | 107.02% |

크기별 결과는 혼재하지만 REQREP 두 크기와 SENDSEND 4096B는 개선됐고, Core 구조 변경에
따른 일관된 binding 회귀는 관찰되지 않았다.

최종 보고서:
`/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260826_141332_posddd-core-router-fastpath-r3-20260826.txt`

## 검증

- `test_router_mandatory_hwm`: 5/5
- `test_send_async_multipart`: 8/8
- 관련 CTest 6/6:
  `test_send_async_multipart`, `test_send_async_tls_admit`,
  `test_router_mandatory_hwm`, `test_flow_state_paired`,
  `test_routed_submit_target`, `unittest_pipe_byte_charge`
- 전체 CTest 첫 실행은 102/103 통과했고, 유일한 실패는 삭제된
  `core/doc/internals/threading-model.*` 경로를 읽는 stale test였다. 현재 정식
  `core/doc/guide/11-thread-safety.*`를 검사하도록 경로와 문구를 맞춘 뒤 해당 테스트와
  Router/send-async 회귀 테스트를 재실행해 3/3 통과했다.
- C/C++ benchmark: 각각 success 4, fail 0, status complete
