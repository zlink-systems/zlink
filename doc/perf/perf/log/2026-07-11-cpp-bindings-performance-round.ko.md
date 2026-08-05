# 2026-07-11 C++ bindings 성능 개선 라운드

이 문서는 core 9.0.0 기준 C++ bindings의 pattern별 paired 측정과 성능 개선 판단을
기록한다. 완료되지 않은 report와 다른 pattern의 수치는 판정에 사용하지 않는다.

## 재현 환경

- source: `41246081f08a463adc8a3ed637ec7ab84d076641`, dirty 작업 트리
- core runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.9.0.0`
- host: WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, x86_64
- CPU: Intel Core Ultra 7 265K, 논리 CPU 20개
- memory: 94 GiB
- toolchain: GCC 13.3.0, CMake 3.28.3, Python 3.12.3
- CPU governor: WSL2에서 governor 파일을 제공하지 않아 기록할 수 없음
- 측정 중 별도 perf process: 없음

## Single PAIR

### 측정 조건

- pattern: `PAIR`
- message size: 64, 256, 1024, 65536, 131072, 262144 bytes
- transport: tcp, tls, ws, wss, inproc, ipc
- duration: 5초
- 기본 판정: 3회 중앙값
- secure transport와 변동 셀 최종 판정: CPU 고정 5회 중앙값
- auto-HWM profile: balanced
- I/O thread: 1
- send/receive timeout: 200ms

### C 대비 결과

아래 비율은 C++ throughput을 가까운 시점에 측정한 C throughput으로 나눈 값이다. 괄호 안은
latency mean, p95, p99 가운데 가장 큰 C 대비 비율이다.

| Transport | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|-----------|----|-----|------|-------|--------|--------|
| tcp | 99.9% (0.71배) | 100.0% (0.84배) | 99.8% (1.01배) | 100.0% (1.03배) | 100.1% (1.06배) | 99.8% (1.14배) |
| tls | 99.4% (1.00배) | 98.5% (1.03배) | 100.5% (1.00배) | 99.8% (1.01배) | 97.8% (1.05배) | 99.6% (1.00배) |
| ws | 100.0% (0.86배) | 100.0% (0.96배) | 100.4% (0.73배) | 100.0% (1.02배) | 100.2% (1.24배) | 100.0% (1.01배) |
| wss | 100.6% (0.99배) | 99.9% (1.00배) | 96.8% (1.05배) | 98.2% (1.02배) | 97.3% (1.05배) | 95.1% (1.08배) |
| inproc | 90.6% (0.65배) | 93.9% (1.11배) | 89.2% (1.07배) | 99.8% (1.04배) | 100.1% (1.16배) | 100.3% (0.97배) |
| ipc | 100.5% (0.74배) | 100.3% (0.93배) | 95.3% (1.08배) | 99.8% (0.98배) | 100.0% (1.33배) | 100.0% (1.04배) |

모든 셀이 C++ 단순 one-way 최소 목표 85%와 latency 상한 2배를 만족했다. 최초 tcp 1024B
3회 측정에서는 C++ p95가 C의 2.62배였지만 CPU 고정 5회 재측정에서는 1.01배로
안정됐다. wss 131072B의 첫 5회 C 측정은 throughput 변동 폭이 16.0%였으므로 판정에
사용하지 않았다. 같은 셀을 단독으로 다시 측정한 결과 C 5.2%, C++ 7.2%로 변동 기준을
만족했고 throughput은 97.3%였다.

### Report

`perf_c_...` 파일은 `bindings/c/perf/results/single/report/`에 있고 `perf_cpp_...` 파일은
`bindings/cpp/perf/results/single/report/`에 있다.

- tcp C/C++:
  - `perf_c_single_linux_20260711_133702_core_9_0_cpp_pair_tcp_paired_20260711.txt`
  - `perf_cpp_single_linux_20260711_133834_core_9_0_cpp_pair_tcp_paired_20260711.txt`
- tcp 1024B 최종 5회 C/C++:
  - `perf_c_single_linux_20260711_134011_core_9_0_cpp_pair_tcp1024_final_20260711.txt`
  - `perf_cpp_single_linux_20260711_134040_core_9_0_cpp_pair_tcp1024_final_20260711.txt`
- tls 최종 5회 C/C++:
  - `perf_c_single_linux_20260711_135702_core_9_0_cpp_pair_tls_final_20260711.txt`
  - `perf_cpp_single_linux_20260711_135934_core_9_0_cpp_pair_tls_final_20260711.txt`
- ws C/C++:
  - `perf_c_single_linux_20260711_134420_core_9_0_cpp_pair_ws_paired_20260711.txt`
  - `perf_cpp_single_linux_20260711_134553_core_9_0_cpp_pair_ws_paired_20260711.txt`
- wss 최종 5회 C/C++:
  - `perf_c_single_linux_20260711_140209_core_9_0_cpp_pair_wss_final_20260711.txt`
  - `perf_cpp_single_linux_20260711_140439_core_9_0_cpp_pair_wss_final_20260711.txt`
- wss 131072B 안정성 보강 C/C++:
  - `perf_c_single_linux_20260711_140720_core_9_0_cpp_pair_wss131072_stability_20260711.txt`
  - `perf_cpp_single_linux_20260711_140756_core_9_0_cpp_pair_wss131072_stability_20260711.txt`
- inproc C/C++:
  - `perf_c_single_linux_20260711_135041_core_9_0_cpp_pair_inproc_paired_20260711.txt`
  - `perf_cpp_single_linux_20260711_135214_core_9_0_cpp_pair_inproc_paired_20260711.txt`
- ipc C/C++:
  - `perf_c_single_linux_20260711_135351_core_9_0_cpp_pair_ipc_paired_20260711.txt`
  - `perf_cpp_single_linux_20260711_135523_core_9_0_cpp_pair_ipc_paired_20260711.txt`

모든 report는 `status: complete`이고 C와 C++의 Effective Options와 auto-HWM detail이
일치한다.

기능 회귀 확인은 다음 명령으로 통과했다.

```bash
ctest --test-dir bindings/cpp/build \
  -R '^sample_smoke_sample_cpp_pair_recv_sample$' \
  --output-on-failure
```

### Resource와 POSD 판단

tcp 64B 대표 실행에서 C process는 CPU 최대 194.0%, 최대 `nlwp=6`이었고 C++ process는
관찰 시 CPU 188%, `nlwp=6`이었다. PAIR은 한 process 안에서 sender와 receiver를 실행한다.

최초 측정부터 모든 throughput 셀이 목표를 만족했으므로 C++ binding 코드를 변경하지 않았다.
새 helper, 특수 분기, public API 또는 perf 전용 우회를 추가하지 않았으며 POSD 위험 신호도
새로 만들지 않았다. 측정 가능한 개선 필요가 없으므로 개선 후보 설계와 커밋은 수행하지
않는다.

### 판정

- Single `PAIR`: 완료
- 코드 변경: 없음
- 성능 개선 커밋과 푸시: 해당 없음
- 다음 pattern: Single `PUBSUB`

## Single PUBSUB

### 측정 조건과 perf 의미 정합화

- pattern: `PUBSUB`
- message size: 64, 256, 1024, 65536, 131072, 262144 bytes
- transport: tcp, tls, ws, wss, inproc, ipc
- duration: 5초
- 기본 판정: 3회 중앙값
- secure transport, 목표 경계, 고변동 셀: 5회 중앙값
- CPU pin: 사용하지 않음
- auto-HWM profile: balanced
- I/O thread: 1

C PUBSUB은 active 구간에서 `ZLINK_DONTWAIT`를 사용하고 `EAGAIN`을 재시도하고 있었다.
이는 Single 정책의 blocking send와 socket HWM backpressure 의미와 달랐다. C perf의 send를
blocking으로 맞추고 `EINTR`만 재시도하도록 수정했다. C++ perf는 이미 같은 의미였으므로
수정하지 않았다.

### C 대비 최종 throughput

아래 표는 throughput 비율이다. 평균 latency도 별도로 비교했으며 p95와 p99는 진단
자료로만 보존하고 통과 판정에는 사용하지 않았다.

| Transport | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|-----------|----|-----|------|-------|--------|--------|
| tcp | 95.7% | 95.2% | 101.7% | 109.2% | 99.4% | 91.4% |
| tls | 91.9% | 95.0% | 101.6% | 98.4% | 99.5% | 99.0% |
| ws | 95.3% | 91.2% | 100.7% | 96.4% | 93.0% | 97.9% |
| wss | 95.2% | 92.6% | 97.4% | 96.8% | 100.0% | 100.6% |
| inproc | 101.8% | 95.0% | 92.4% | 85.2% | 105.9% | 125.4% |
| ipc | 94.3% | 95.4% | 95.2% | 94.2% | 97.8% | 94.6% |

평균 latency의 최대 비율은 inproc 1.36배, ipc 1.06배였고 tcp, tls, ws, wss도 C++ 상한
2배 이내였다. 모든 transport와 size의 throughput이 단순 one-way 최소 목표 85%를
만족했다.

### 병목과 POSD 판단

최초 inproc 측정에서 C++의 131072B와 262144B throughput은 C의 66.0%와 18.7%였다.
직접 계측한 C++ `message_t::from`의 할당과 복사는 262144B에서 메시지당 28.3us였고,
C의 같은 구간은 6.2us였다. submit과 latency stamp 비용 차이는 작았으므로 binding의 대형
메시지 저장소 할당을 병목으로 판정했다.

두 가지 방안을 검토했다.

1. 전역 allocator를 jemalloc으로 강제하면 probe 수치는 회복되지만 애플리케이션 전체의
   allocator와 배포 정책을 binding 밖으로 노출한다.
2. `message_t` 공개 API는 유지하고 Messaging 구현 안에서 대형 저장소만 제한적으로
   재사용하면 호출자 복잡도를 늘리지 않고 병목 지식을 한 모듈에 가둘 수 있다.

두 번째 방안을 선택했다. 저장소 풀은 총 8MiB로 제한하고 정확히 같은 크기의 block만
재사용한다. 64KiB까지 재사용하면 ipc 65536B가 72.3%로 낮아졌으므로 실제 병목이 확인된
128KiB 이상으로 범위를 좁혔다. 확정된 hot path와 회귀 근거는 구현 주석으로 남겼다.

### 최종 report

report의 공통 위치는 C가 `bindings/c/perf/results/single/report/`, C++가
`bindings/cpp/perf/results/single/report/`다.

- tcp: `perf_c_single_linux_20260711_151703_core_9_0_cpp_pubsub_tcp_nopin_policy_aligned_20260711.txt`, `perf_cpp_single_linux_20260711_151852_core_9_0_cpp_pubsub_tcp_nopin_policy_aligned_20260711.txt`
- tcp 131072B: `perf_c_single_linux_20260711_152048_core_9_0_cpp_pubsub_tcp131072_nopin_boundary_20260711.txt`, `perf_cpp_single_linux_20260711_152131_core_9_0_cpp_pubsub_tcp131072_nopin_boundary_20260711.txt`
- tls: `perf_c_single_linux_20260711_152332_core_9_0_cpp_pubsub_tls_nopin_policy_aligned_20260711.txt`, `perf_cpp_single_linux_20260711_152519_core_9_0_cpp_pubsub_tls_nopin_policy_aligned_20260711.txt`
- ws: `perf_c_single_linux_20260711_153039_core_9_0_cpp_pubsub_ws_nopin_policy_aligned_20260711.txt`, `perf_cpp_single_linux_20260711_153228_core_9_0_cpp_pubsub_ws_nopin_policy_aligned_20260711.txt`
- wss: `perf_c_single_linux_20260711_153604_core_9_0_cpp_pubsub_wss_nopin_policy_aligned_20260711.txt`, `perf_cpp_single_linux_20260711_153753_core_9_0_cpp_pubsub_wss_nopin_policy_aligned_20260711.txt`
- inproc 전체: `perf_c_single_linux_20260711_163231_core_9_0_cpp_pubsub_inproc_pool_full_final_20260711.txt`, `perf_cpp_single_linux_20260711_163527_core_9_0_cpp_pubsub_inproc_pool_full_final_20260711.txt`
- inproc 65536B 최종: `perf_c_single_linux_20260711_164419_core_9_0_cpp_pubsub_inproc65536_pool_boundary_final_20260711.txt`, `perf_cpp_single_linux_20260711_164451_core_9_0_cpp_pubsub_inproc65536_pool_boundary_final_20260711.txt`
- ipc: `perf_c_single_linux_20260711_163826_core_9_0_cpp_pubsub_ipc_nopin_final_20260711.txt`, `perf_cpp_single_linux_20260711_164045_core_9_0_cpp_pubsub_ipc_nopin_final_20260711.txt`
- ipc 65536B 최종: `perf_c_single_linux_20260711_164255_core_9_0_cpp_pubsub_ipc65536_pool_boundary_final_20260711.txt`, `perf_cpp_single_linux_20260711_164334_core_9_0_cpp_pubsub_ipc65536_pool_boundary_final_20260711.txt`

최종 코드로 `test_cpp_contract_message`, `test_cpp_contract_behavior`,
`test_cpp_contract_socket`, `sample_smoke_sample_cpp_pubsub_recv_sample`을 다시 빌드해 모두
통과했다. message 계약 테스트에는 262144B owned storage를 해제하고 다시 할당한 뒤 copy의
양 끝 payload를 확인하는 회귀 항목을 추가했다. 같은 테스트의 Valgrind full leak check도
오류 없이 통과했다.

### 판정

- Single `PUBSUB`: 완료
- C perf 변경: Single 정책과 다른 nonblocking send 의미를 blocking send로 정합화
- C++ binding 변경: 128KiB 이상, 1MiB 이하 owned message storage의 8MiB 제한 재사용
- 다음 pattern: Single `DEALER_DEALER`

## Single DEALER_DEALER

### 측정 조건과 결과

- source: `99c58f4d0d3e97a1f85b7cbdb6941375be9d53a3`
- message size: 64, 256, 1024, 65536, 131072, 262144 bytes
- transport: tcp, tls, ws, wss, inproc, ipc
- duration: 5초
- tcp, ws, inproc, ipc: 3회 중앙값
- tls, wss: 5회 중앙값
- CPU pin: 사용하지 않음

아래 표는 C++ throughput을 가까운 시점의 C throughput으로 나눈 값이다.

| Transport | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|-----------|----|-----|------|-------|--------|--------|
| tcp | 99.9% | 99.8% | 100.0% | 99.9% | 100.0% | 99.8% |
| tls | 100.0% | 99.9% | 97.9% | 96.8% | 99.7% | 99.7% |
| ws | 100.0% | 99.9% | 95.1% | 100.0% | 100.1% | 100.0% |
| wss | 100.0% | 99.5% | 98.4% | 98.8% | 99.5% | 100.7% |
| inproc | 89.0% | 100.3% | 90.4% | 99.9% | 100.7% | 100.0% |
| ipc | 100.1% | 100.1% | 93.5% | 99.9% | 100.1% | 100.0% |

모든 throughput 셀이 단순 one-way 최소 목표 85%를 만족했다. 평균 latency의 transport별
최대 비율은 tcp 1.22배, tls 1.15배, ws 1.22배, wss 1.01배, inproc 1.06배,
ipc 1.20배로 모두 C++ 상한 2배 이내였다.

### Report와 회귀 확인

report의 공통 위치는 C가 `bindings/c/perf/results/single/report/`, C++가
`bindings/cpp/perf/results/single/report/`다.

- tcp: `perf_c_single_linux_20260711_165321_core_9_0_cpp_dealer_dealer_tcp_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_165452_core_9_0_cpp_dealer_dealer_tcp_nopin_paired_20260711.txt`
- tls: `perf_c_single_linux_20260711_165709_core_9_0_cpp_dealer_dealer_tls_nopin_final_20260711.txt`, `perf_cpp_single_linux_20260711_165940_core_9_0_cpp_dealer_dealer_tls_nopin_final_20260711.txt`
- ws: `perf_c_single_linux_20260711_170217_core_9_0_cpp_dealer_dealer_ws_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_170350_core_9_0_cpp_dealer_dealer_ws_nopin_paired_20260711.txt`
- wss: `perf_c_single_linux_20260711_170553_core_9_0_cpp_dealer_dealer_wss_nopin_final_20260711.txt`, `perf_cpp_single_linux_20260711_170819_core_9_0_cpp_dealer_dealer_wss_nopin_final_20260711.txt`
- inproc: `perf_c_single_linux_20260711_171107_core_9_0_cpp_dealer_dealer_inproc_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_171251_core_9_0_cpp_dealer_dealer_inproc_nopin_paired_20260711.txt`
- ipc: `perf_c_single_linux_20260711_171438_core_9_0_cpp_dealer_dealer_ipc_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_171612_core_9_0_cpp_dealer_dealer_ipc_nopin_paired_20260711.txt`

모든 report는 `status: complete`이고 Effective Options와 auto-HWM detail이 C와 C++에서
일치한다. `test_cpp_contract_message`, `test_cpp_contract_socket`,
`test_cpp_contract_behavior`도 최종 코드로 다시 실행해 통과했다.

### POSD 판단과 판정

최초 paired 측정에서 모든 셀이 목표를 만족했다. 추가 helper, pattern 전용 분기, public API,
perf 우회가 필요하지 않았으며 binding 코드를 변경하지 않았다. PUBSUB에서 확정한 대형 메시지
저장소 hot path 최적화가 다른 one-way pattern에서도 회귀 없이 유지됐다.

- Single `DEALER_DEALER`: 완료
- 코드 변경: 없음
- 다음 pattern: Single `DEALER_ROUTER`

## Single DEALER_ROUTER

### 최초 측정과 측정 의미 확인

- source: `0ac653692`
- message size: 64, 256, 1024, 65536, 131072, 262144 bytes
- transport: tcp, tls, ws, wss, inproc, ipc
- duration: 5초
- 기본 판정: 3회 중앙값
- secure transport와 변동 셀: 5회 중앙값
- CPU pin: 사용하지 않음

최초 tcp paired 측정에서 65536B, 131072B, 262144B의 C++ throughput은 C의 58.3%,
45.0%, 45.2%였다. C++ binding의 공개 경계를 raw C 호출과 교차한 진단에서는 두 경로가
메시지당 약 33~37us로 같았고, C와 C++의 메시지 할당·복사 단독 진단도 약 3.5us로 같았다.
따라서 binding 내부 allocation이나 send 경계는 이 pattern의 병목이 아니었다.

C perf는 재사용 payload 전체를 채운 뒤 매 메시지마다 새 message에 전체 payload를 복사한다.
기존 C++ perf는 새 message를 할당한 뒤 metric header만 쓰고 나머지 payload에는 쓰지 않았다.
이는 같은 message size를 전송하더라도 page touch와 copy 의미가 달라 C 대비 binding 비용을
측정한다는 정책에 맞지 않았다. C++ perf가 재사용 payload를 채우고 기존
`message_from_payload` 경로로 전체 payload를 복사하도록 정합화했다. 이 측정 의미가 이후
리팩토링에서 사라지지 않도록 send loop에 C 기준과 full payload copy를 설명하는 주석을
남겼다.

### POSD 대안 검토

위험 신호는 성능 원인을 binding으로 단정하면 pattern 전용 allocation 분기가 범용 message
모듈에 섞일 수 있다는 점이었다. 두 가지 방향을 검토했다.

1. `message_t::allocate`와 size constructor를 core 소유 native storage로 분리하는 방안은 공개
   API를 바꾸지 않지만, 262144B probe를 45.2%에서 47.0%로만 높여 병목을 제거하지 못했다.
   이 후보는 최종 코드에서 제거했다.
2. C와 C++의 payload 생성·복사 의미를 같게 만들면 binding API와 무관한 page-touch 차이를
   제거하고 기존 공개 API와 책임 경계를 유지할 수 있다.

두 번째 방안을 선택했다. 새 public API, helper, timeout, sleep 또는 pattern 전용 binding
분기를 추가하지 않았다. PUBSUB에서 실제로 확인한 대형 owned message allocation hot path의
제한 재사용과 근거 주석은 `message.cpp` 안에 그대로 유지된다.

### C 대비 최종 throughput

아래 표는 C++ throughput을 가까운 시점의 C throughput으로 나눈 값이다. 평균 latency만
latency gate로 비교했고 p95와 p99는 진단 자료로만 보존했다.

| Transport | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|-----------|----|-----|------|-------|--------|--------|
| tcp | 91.9% | 97.9% | 92.3% | 99.0% | 100.4% | 96.1% |
| tls | 90.8% | 91.6% | 98.0% | 97.2% | 100.5% | 102.9% |
| ws | 89.5% | 90.3% | 94.2% | 95.2% | 97.8% | 99.3% |
| wss | 92.5% | 94.6% | 97.6% | 101.9% | 97.9% | 98.4% |
| inproc | 87.7% | 92.8% | 97.4% | 80.2% | 106.1% | 90.3% |
| ipc | 90.2% | 91.1% | 99.1% | 95.3% | 88.9% | 90.9% |

모든 throughput 셀이 routed one-way 최소 목표 70%를 만족했다. 평균 latency의 transport별
최대 비율은 tcp 1.03배, tls 1.15배, ws 1.57배, wss 1.27배, inproc 1.17배,
ipc 1.12배로 C++ 상한 2배 이내였다. 변동 폭이 10%를 넘었던 ws 65536B와 wss 65536B,
262144B는 CPU pin 없이 C와 C++를 각각 5회 다시 측정했다. 최종 throughput 변동 폭은
ws가 C 7.6%, C++ 3.4%, wss가 C 8.5% 이하, C++ 6.7% 이하로 안정화됐다.

### 최종 report와 회귀 확인

report의 공통 위치는 C가 `bindings/c/perf/results/single/report/`, C++가
`bindings/cpp/perf/results/single/report/`다.

- tcp: `perf_c_single_linux_20260711_174157_core_9_0_cpp_dealer_router_tcp_payload_aligned_final_20260711.txt`, `perf_cpp_single_linux_20260711_174328_core_9_0_cpp_dealer_router_tcp_payload_aligned_final_20260711.txt`
- tls: `perf_c_single_linux_20260711_174511_core_9_0_cpp_dealer_router_tls_payload_aligned_final_20260711.txt`, `perf_cpp_single_linux_20260711_174741_core_9_0_cpp_dealer_router_tls_payload_aligned_final_20260711.txt`
- ws: `perf_c_single_linux_20260711_175020_core_9_0_cpp_dealer_router_ws_payload_aligned_final_20260711.txt`, `perf_cpp_single_linux_20260711_175152_core_9_0_cpp_dealer_router_ws_payload_aligned_final_20260711.txt`
- ws 65536B 안정성: `perf_c_single_linux_20260711_180627_core_9_0_cpp_dealer_router_ws65536_payload_aligned_stability2_20260711.txt`, `perf_cpp_single_linux_20260711_180655_core_9_0_cpp_dealer_router_ws65536_payload_aligned_stability2_20260711.txt`
- wss: `perf_c_single_linux_20260711_175436_core_9_0_cpp_dealer_router_wss_payload_aligned_final_20260711.txt`, `perf_cpp_single_linux_20260711_175705_core_9_0_cpp_dealer_router_wss_payload_aligned_final_20260711.txt`
- wss 대형 안정성: `perf_c_single_linux_20260711_180721_core_9_0_cpp_dealer_router_wss_large_payload_aligned_stability2_20260711.txt`, `perf_cpp_single_linux_20260711_180814_core_9_0_cpp_dealer_router_wss_large_payload_aligned_stability2_20260711.txt`
- inproc: `perf_c_single_linux_20260711_175936_core_9_0_cpp_dealer_router_inproc_payload_aligned_final_20260711.txt`, `perf_cpp_single_linux_20260711_180114_core_9_0_cpp_dealer_router_inproc_payload_aligned_final_20260711.txt`
- ipc: `perf_c_single_linux_20260711_180309_core_9_0_cpp_dealer_router_ipc_payload_aligned_final_20260711.txt`, `perf_cpp_single_linux_20260711_180446_core_9_0_cpp_dealer_router_ipc_payload_aligned_final_20260711.txt`

`test_cpp_contract_message`, `test_cpp_contract_socket`, `test_cpp_contract_behavior`가 통과했다.
`sample_cpp_dealer_router_recv_sample` target을 빌드한 뒤
`sample_smoke_sample_cpp_dealer_router_recv_sample`도 통과했다.

### 판정

- Single `DEALER_ROUTER`: 완료
- C++ perf 변경: C와 같은 full payload copy 의미로 정합화
- C++ binding 변경: 없음
- 완료 커밋: `3643bf345`
- 다음 pattern: Single `DEALER_ROUTER_REQREP`

## Single DEALER_ROUTER_REQREP

### 최초 실패와 perf 의미 수정

- source: `7af46da58`
- message size: 64, 256, 1024, 65536, 131072, 262144 bytes
- transport: tcp, tls, ws, wss, inproc, ipc
- duration: 5초
- 최종 판정: 5회 중앙값
- CPU pin: 사용하지 않음
- 공식 perf process: 한 번에 하나만 실행

최초 C 대형 메시지 측정은 요청을 제한 없이 제출해 timeout이 발생했고 일부 report가
partial로 끝났다. 완료 poller를 replier thread보다 먼저 해제하거나 replier가 종료되기 전에
종료 메시지를 보내지 못하는 수명 주기 문제도 있었다. 요청 수만 고정하는 방식은 전체
payload 양을 제한하지 못했고, 고정 pipeline 16개는 131072B의 auto-HWM 8개를 넘었다.
고정 pipeline 4개는 timeout을 없앴지만 작은 메시지까지 처리량을 제한했다.

최종 perf는 최대 64개 요청과 768KiB payload 중 먼저 도달하는 한도로 요청을 유지한다.
호출자는 replier thread를 종료한 뒤 완료 poller를 해제한다. 이 순서를 나중에 바꾸지 않도록
C와 C++의 측정 요청 hot path와 C의 poller 수명 주기에 근거 주석을 남겼다.

추가 대조에서 C replier만 수신 payload와 같은 크기의 메시지를 다시 할당하고 전체 payload를
복사한 뒤 응답하고 있었다. C++은 수신 메시지의 소유권을 응답으로 넘겼고, C multi
request/reply도 같은 공개 C API 사용 방식을 사용했다. C++에 전체 복사를 추가하는 방안은
바인딩에 없는 비용을 새로 넣으므로 제외했다. C single replier가 수신 메시지를 공개
`zlink_router_reply_part()`에 직접 넘기도록 수정해 C와 C++의 echo 의미를 맞췄다. 응답 API가
메시지 소유권을 소비한다는 결정과 이 구간이 측정 대상이라는 점을 hot path 주석으로 남겼다.

### POSD 대안 검토

위험 신호는 종료 순서를 requester helper 안에 숨기면 helper가 replier thread의 존재를 알아야
하고, C에만 있는 payload 복사를 기준으로 삼으면 같은 echo 동작의 비용 지식이 두 perf에
다르게 퍼진다는 점이었다.

1. requester helper가 replier 종료 callback을 받는 방안은 helper 인터페이스에 server 수명
   주기를 노출하고 실행 순서 결합을 늘린다.
2. replier thread를 이미 소유한 pattern 호출자가 완료 poller도 마지막에 해제하면 수명 주기
   책임이 한곳에 유지된다.
3. C++ replier에도 새 메시지 할당과 전체 복사를 추가하면 코드 모양은 같아지지만 공개 API의
   소유권 전달을 사용하지 않고 C++ 측정값에 불필요한 비용을 더한다.
4. C replier가 수신 메시지 소유권을 바로 응답에 넘기면 기존 C multi perf와 C++ binding의
   공개 사용 방식이 같아지고 추가 allocation과 copy가 사라진다.

두 번째와 네 번째 방안을 선택했다. 새 public API, perf 전용 binding 경로, private API,
timeout이나 sleep 증가는 추가하지 않았다.

### C 대비 최종 throughput

아래 표는 C++ throughput을 가까운 시점의 C throughput으로 나눈 값이다. 평균 latency만
latency gate로 사용했고 p95와 p99는 진단 자료로 보존했다.

| Transport | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|-----------|----|-----|------|-------|--------|--------|
| tcp | 96.7% | 96.5% | 97.2% | 95.2% | 101.0% | 123.7% |
| tls | 97.7% | 97.5% | 98.1% | 96.3% | 99.2% | 99.3% |
| ws | 98.8% | 99.5% | 99.3% | 90.8% | 99.2% | 100.7% |
| wss | 96.3% | 96.4% | 98.4% | 94.7% | 98.0% | 103.1% |
| inproc | 97.5% | 95.6% | 95.9% | 145.7% | 206.3% | 88.7% |
| ipc | 97.8% | 98.0% | 99.1% | 97.6% | 98.5% | 120.2% |

36개 throughput 셀이 socket request/reply의 C++ 최소 목표 65%를 모두 만족했다. 평균
latency의 transport별 최대 비율은 tcp 1.04배, tls 1.05배, ws 1.09배, wss 1.05배,
inproc 1.02배, ipc 1.02배로 모두 상한 2배 이내였다.

### 변동성 조사

tcp, tls, ws, ipc는 최종 5회에서 안정적이었다. wss 131072B와 inproc 대형 셀은 측정 중
지속적인 고부하 process가 없고 load average가 약 0.24~1.10인 상태에서도 변동이 반복됐다.
대상 셀만 CPU pin 없이 다시 측정한 결과는 다음과 같다.

- wss 131072B: C throughput 변동 6.1%, C++ 21.1%; C++ 평균 latency 변동 25.7%
- inproc 65536B: C throughput 변동 49.5%, C++ 15.0%
- inproc 131072B: C throughput 변동 143.4%, C++ 10.6%
- inproc 262144B: C throughput 변동 6.6%, C++ 6.1%

1초 실제 workload warmup과 15초 duration은 앞선 진단에서 변동을 줄이지 못해 최종 코드에
넣지 않았다. pipeline을 balanced auto-HWM의 1MiB 예산과 같게 만드는 대안도 C inproc
131072B에서 49.29~129.21 Kops/s로 변동이 커져 폐기하고 768KiB로 되돌렸다. CPU pin은
사용하지 않았고 유리한 실행 결과만 골라내지 않았다. 현재 host가 서로 다른 종류의 CPU
core를 제공하므로 실행 배치가 후보가 될 수 있지만 실행별 core를 기록하지 않아 원인으로
단정하지 않는다. 저부하 확인, 동일 셀 재측정, perf 의미와 pipeline 대안 검토를 마친 뒤 5회
중앙값으로 상대 throughput과 평균 latency를 판정했다.

### 최종 report와 회귀 확인

report의 공통 위치는 C가 `bindings/c/perf/results/single/report/`, C++가
`bindings/cpp/perf/results/single/report/`다.

- tcp: `perf_c_single_linux_20260711_210316_core_9_0_cpp_dealer_router_reqrep_tcp_direct_reply_final_20260711.txt`, `perf_cpp_single_linux_20260711_210542_core_9_0_cpp_dealer_router_reqrep_tcp_direct_reply_final_20260711.txt`
- tls: `perf_c_single_linux_20260711_210818_core_9_0_cpp_dealer_router_reqrep_tls_direct_reply_final_20260711.txt`, `perf_cpp_single_linux_20260711_211045_core_9_0_cpp_dealer_router_reqrep_tls_direct_reply_final_20260711.txt`
- ws: `perf_c_single_linux_20260711_211321_core_9_0_cpp_dealer_router_reqrep_ws_direct_reply_final_20260711.txt`, `perf_cpp_single_linux_20260711_211544_core_9_0_cpp_dealer_router_reqrep_ws_direct_reply_final_20260711.txt`
- wss: `perf_c_single_linux_20260711_211815_core_9_0_cpp_dealer_router_reqrep_wss_direct_reply_final_20260711.txt`, `perf_cpp_single_linux_20260711_212040_core_9_0_cpp_dealer_router_reqrep_wss_direct_reply_final_20260711.txt`
- wss 131072B 재측정: `perf_c_single_linux_20260711_213242_core_9_0_cpp_dealer_router_reqrep_wss131_direct_reply_stability2_20260711.txt`, `perf_cpp_single_linux_20260711_213316_core_9_0_cpp_dealer_router_reqrep_wss131_direct_reply_stability2_20260711.txt`
- inproc small: `perf_c_single_linux_20260711_205751_core_9_0_cpp_dealer_router_reqrep_inproc_direct_reply_20260711.txt`, `perf_cpp_single_linux_20260711_205423_core_9_0_cpp_dealer_router_reqrep_inproc_budget768_rerun_20260711.txt`
- inproc 대형 재측정: `perf_c_single_linux_20260711_212814_core_9_0_cpp_dealer_router_reqrep_inproc_large_direct_reply_stability2_20260711.txt`, `perf_cpp_single_linux_20260711_212930_core_9_0_cpp_dealer_router_reqrep_inproc_large_direct_reply_stability2_20260711.txt`
- ipc: `perf_c_single_linux_20260711_212314_core_9_0_cpp_dealer_router_reqrep_ipc_direct_reply_final_20260711.txt`, `perf_cpp_single_linux_20260711_212539_core_9_0_cpp_dealer_router_reqrep_ipc_direct_reply_final_20260711.txt`

최종 코드로 C의 `perf_dealer_router_reqrep`, `perf_router_router_reqrep`와 C++의 대응 target을
다시 빌드했다. C `ROUTER_ROUTER_REQREP` 64B/tcp 회귀 스모크는
`perf_c_single_linux_20260711_213452_core_9_0_c_reqrep_direct_reply_regression_smoke_20260711.txt`에서
complete다. `test_cpp_contract_message`, `test_cpp_contract_socket`,
`test_cpp_contract_behavior`도 모두 통과했다.

### 판정

- Single `DEALER_ROUTER_REQREP`: 완료
- C perf 변경: request/reply 수명 주기 수정과 C/C++ echo 소유권 전달 의미 정합화
- C++ binding 변경: 없음
- 완료 커밋: `f951e7baa`
- 다음 pattern: Single `ROUTER_ROUTER`

## Single ROUTER_ROUTER

### Transport 단위 측정 조건

- source: `34fb4414f`
- message size: 64, 256, 1024, 65536, 131072, 262144 bytes
- transport 순서: tcp, ws, wss, tls, inproc, ipc
- duration: 5초
- 반복: transport마다 C 5회 직후 C++ 5회
- CPU pin: 사용하지 않음
- 공식 perf process: 한 번에 하나만 실행

이 pattern부터 같은 pattern의 transport를 한꺼번에 측정하지 않았다. tcp의 모든 message
size를 C와 C++으로 비교하고 판정한 뒤 ws로 이동했고, 같은 절차로 마지막 ipc까지 진행했다.
한 transport가 미달이면 다른 transport를 미리 측정하지 않고 현재 transport에서 분석과
개선, 재측정을 마치도록 계획 문서 7.4절도 갱신했다.

### C 대비 최종 throughput

아래 표는 C++ throughput을 가까운 시점의 C throughput으로 나눈 값이다. 평균 latency만
latency gate에 사용했고 p95와 p99는 진단 자료로 보존했다.

| Transport | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|-----------|----|-----|------|-------|--------|--------|
| tcp | 110.1% | 99.5% | 96.9% | 101.5% | 99.4% | 99.4% |
| ws | 104.8% | 95.9% | 95.2% | 95.4% | 99.4% | 99.7% |
| wss | 111.8% | 95.3% | 97.5% | 97.5% | 99.1% | 98.0% |
| tls | 110.9% | 105.5% | 97.2% | 96.1% | 99.1% | 100.5% |
| inproc | 109.4% | 101.5% | 97.9% | 83.7% | 105.8% | 144.0% |
| ipc | 101.8% | 100.4% | 94.1% | 93.8% | 98.0% | 99.3% |

36개 throughput 셀이 routed one-way의 C++ 최소 목표 70%를 만족했다. 평균 latency의
transport별 최대 비율은 tcp 1.00배, ws 1.95배, wss 1.26배, tls 1.11배, inproc
1.00배, ipc 1.11배로 모두 상한 2배 이내였다.

### 변동성과 POSD 확인

tcp 64B 처리량은 C 13.0%, C++ 10.5% 변동이었고, ws와 tls의 일부 작은 메시지 평균
latency는 20%를 넘게 변했다. 처리량은 목표에서 충분히 떨어지지 않았고 C와 C++ 모두
one-way queue 체류 시간이 회차별로 달라지는 형태였다. 측정 중 지속적인 고부하 process는
없었고, 각 transport의 C와 C++ Effective Options와 auto-HWM은 일치했다.

ipc 65536B C++ 처리량은 최초 5회에서 약 30% 변동해 해당 transport와 size만 다시 paired
측정했다. 재측정에서 C는 94.88~99.27 Kmsg/s로 4.5% 변동이었고, C++은
65.78~94.99 Kmsg/s로 약 31.9% 변동이 반복됐다. C++ 중앙값은 91.71 Kmsg/s로 같은 시점
C 중앙값 97.72 Kmsg/s의 93.8%이고, 평균 latency는 1.04배였다. 저부하 재측정에서도
변동이 반복됐으므로 범위를 그대로 기록하고 5회 중앙값으로 판정했다.

모든 transport가 최초 paired 중앙값에서 처리량과 평균 latency 목표를 만족했으므로 binding
코드에 pattern 전용 분기나 추가 helper를 넣을 근거가 없었다. 성능 수치를 더 높이기 위한
특수 경로는 public interface와 message 모듈에 transport 지식을 섞는 POSD 위험 신호가 된다.
기존 공개 API와 측정 의미를 유지하는 방안과 transport 전용 fast path를 추가하는 방안을
비교했으며, 목표를 이미 통과한 상태에서 복잡성을 늘리지 않는 기존 경로 유지를 선택했다.
따라서 C++ binding과 perf 코드는 변경하지 않았다.

### 최종 report와 회귀 확인

report의 공통 위치는 C가 `bindings/c/perf/results/single/report/`, C++가
`bindings/cpp/perf/results/single/report/`다.

- tcp: `perf_c_single_linux_20260711_213832_core_9_0_cpp_router_router_tcp_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_214114_core_9_0_cpp_router_router_tcp_nopin_paired_20260711.txt`
- ws: `perf_c_single_linux_20260711_214516_core_9_0_cpp_router_router_ws_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_214745_core_9_0_cpp_router_router_ws_nopin_paired_20260711.txt`
- wss: `perf_c_single_linux_20260711_215101_core_9_0_cpp_router_router_wss_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_215331_core_9_0_cpp_router_router_wss_nopin_paired_20260711.txt`
- tls: `perf_c_single_linux_20260711_215612_core_9_0_cpp_router_router_tls_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_215844_core_9_0_cpp_router_router_tls_nopin_paired_20260711.txt`
- inproc: `perf_c_single_linux_20260711_220117_core_9_0_cpp_router_router_inproc_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_220346_core_9_0_cpp_router_router_inproc_nopin_paired_20260711.txt`
- ipc: `perf_c_single_linux_20260711_220620_core_9_0_cpp_router_router_ipc_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_220847_core_9_0_cpp_router_router_ipc_nopin_paired_20260711.txt`
- ipc 65536B 재측정: `perf_c_single_linux_20260711_221126_core_9_0_cpp_router_router_ipc65536_nopin_stability2_20260711.txt`, `perf_cpp_single_linux_20260711_221157_core_9_0_cpp_router_router_ipc65536_nopin_stability2_20260711.txt`

최종 코드로 `cpp_perf_router_router`, `test_cpp_contract_message`,
`test_cpp_contract_socket`, `test_cpp_contract_behavior` target을 다시 빌드했다. 세 계약 테스트는
모두 통과했다. `ROUTER_ROUTER` 전용 sample smoke는 CTest에 등록되어 있지 않았다.

### 판정

- Single `ROUTER_ROUTER`: 완료
- C++ binding 변경: 없음
- C++ perf 변경: 없음
- 완료 문서 커밋: `46be5a62c`
- 다음 pattern: Single `ROUTER_ROUTER_REQREP`

## Single ROUTER_ROUTER_REQREP

### Transport 단위 측정 조건

- source: `089eebeb6`
- message size: 64, 256, 1024, 65536, 131072, 262144 bytes
- transport 순서: tcp, ws, wss, tls, inproc, ipc
- duration: 5초
- 반복: transport마다 C 5회 직후 C++ 5회
- CPU pin: 사용하지 않음
- 공식 perf process: 한 번에 하나만 실행

각 transport의 모든 message size를 C와 C++으로 비교하고 판정한 뒤 다음 transport로
이동했다. 측정 시작 전에는 현재 CPU 사용률과 남은 perf process를 확인했다. ws 측정 전
다른 테스트 process의 누적 CPU 수치가 보였을 때는 현재 사용률이 낮아질 때까지 기다린 뒤
측정을 시작했다.

### C 대비 최종 throughput

아래 표는 C++ throughput을 가까운 시점의 C throughput으로 나눈 값이다. 평균 latency만
latency gate에 사용했고 p95와 p99는 진단 자료로 보존했다.

| Transport | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|-----------|----|-----|------|-------|--------|--------|
| tcp | 96.4% | 96.7% | 98.7% | 102.8% | 101.3% | 124.4% |
| ws | 96.3% | 96.3% | 100.5% | 91.9% | 101.3% | 104.6% |
| wss | 95.0% | 95.5% | 96.9% | 96.0% | 107.5% | 97.9% |
| tls | 96.3% | 98.0% | 95.0% | 100.9% | 102.1% | 98.3% |
| inproc | 95.0% | 94.3% | 94.2% | 89.0% | 245.0% | 93.4% |
| ipc | 95.4% | 96.7% | 95.6% | 94.3% | 96.9% | 114.6% |

36개 throughput 셀이 socket request/reply의 C++ 최소 목표 65%를 만족했다. 평균 latency의
transport별 최대 비율은 tcp 1.03배, ws 1.09배, wss 1.06배, tls 1.08배, inproc
1.12배, ipc 1.05배로 모두 상한 2배 이내였다. ws 65536B는 아래의 단독 보강 측정값을
최종 판정에 사용했다.

### 변동성과 POSD 확인

ws 65536B의 최초 C++ 5회 처리량 변동 폭이 약 26%였으므로 CPU 부하가 낮은 상태에서 그
transport와 size만 C와 C++ 순서로 다시 측정했다. 보강 측정의 C 처리량은
21.77~23.14 Kmsg/s, C++은 19.32~22.99 Kmsg/s였고 중앙값 비율은 91.9%, 평균 latency
비율은 1.09배였다. 변동 범위를 숨기지 않고 중앙값과 함께 기록했다.

inproc C는 65536B에서 73.33~177.73 Kmsg/s, 262144B에서 18.76~46.68 Kmsg/s로 두
처리량 모드가 반복됐다. 같은 시점의 C++은 각각 108.88~142.89 Kmsg/s와
39.00~42.97 Kmsg/s였다. C++/C 중앙값 처리량과 평균 latency는 모두 목표를 만족했고,
기존 request/reply 측정에서도 확인한 inproc 대형 셀의 변동 형태와 같았다. perf 의미나
runner bug를 나타내는 새 증거가 없어 timeout, sleep, CPU pin 또는 유리한 회차 선택 없이
범위를 기록하고 중앙값으로 판정했다.

모든 transport가 목표를 만족했으므로 binding hot path에 성능 전용 코드를 넣을 근거가
없었다. 첫 번째 방안은 공개 request/reply와 reply context의 기존 경로를 유지하는 것이고,
두 번째 방안은 transport와 message size별 fast path를 추가하는 것이다. 두 번째 방안은
transport 결정과 측정 조건을 범용 message 모듈에 섞고 특수 코드와 범용 코드를 결합하는
POSD 위험 신호를 만든다. 목표를 이미 만족한 상태에서는 인터페이스와 정보 은닉을 유지하는
첫 번째 방안을 선택했다. 따라서 C++ binding, perf와 source comment는 변경하지 않았다.

### 최종 report와 회귀 확인

report의 공통 위치는 C가 `bindings/c/perf/results/single/report/`, C++가
`bindings/cpp/perf/results/single/report/`다.

- tcp: `perf_c_single_linux_20260711_221629_core_9_0_cpp_router_router_reqrep_tcp_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_221853_core_9_0_cpp_router_router_reqrep_tcp_nopin_paired_20260711.txt`
- ws: `perf_c_single_linux_20260711_222242_core_9_0_cpp_router_router_reqrep_ws_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_222509_core_9_0_cpp_router_router_reqrep_ws_nopin_paired_20260711.txt`
- ws 65536B 보강: `perf_c_single_linux_20260711_222744_core_9_0_cpp_router_router_reqrep_ws65536_nopin_stability2_20260711.txt`, `perf_cpp_single_linux_20260711_222812_core_9_0_cpp_router_router_reqrep_ws65536_nopin_stability2_20260711.txt`
- wss: `perf_c_single_linux_20260711_222856_core_9_0_cpp_router_router_reqrep_wss_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_223129_core_9_0_cpp_router_router_reqrep_wss_nopin_paired_20260711.txt`
- tls: `perf_c_single_linux_20260711_223406_core_9_0_cpp_router_router_reqrep_tls_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_223642_core_9_0_cpp_router_router_reqrep_tls_nopin_paired_20260711.txt`
- inproc: `perf_c_single_linux_20260711_223911_core_9_0_cpp_router_router_reqrep_inproc_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_224256_core_9_0_cpp_router_router_reqrep_inproc_nopin_paired_20260711.txt`
- ipc: `perf_c_single_linux_20260711_224603_core_9_0_cpp_router_router_reqrep_ipc_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_224826_core_9_0_cpp_router_router_reqrep_ipc_nopin_paired_20260711.txt`

최종 코드로 `perf_router_router_reqrep`, `cpp_perf_router_router_reqrep`,
`test_cpp_contract_message`, `test_cpp_contract_socket`, `test_cpp_contract_behavior`,
`test_cpp_contract_request_reply` target을 다시 빌드했다. 네 계약 테스트는 모두 통과했다.

### 판정

- Single `ROUTER_ROUTER_REQREP`: 완료
- C++ binding 변경: 없음
- C++ perf 변경: 없음
- 완료 문서 커밋: `f1916050c`
- 다음 pattern: Single `SPOT`

## Single SPOT

### Transport 단위 측정 조건

- source: `2445892e7`, perf 컴파일 수정 `3506ba1c7`
- message size: 64, 256, 1024, 65536, 131072, 262144 bytes
- transport 순서: tcp, ws, wss, tls
- duration: 5초
- 반복: transport마다 C 5회 직후 C++ 5회
- CPU pin: 사용하지 않음
- 공식 perf process: 한 번에 하나만 실행

SPOT은 정책과 양쪽 runner가 지원하는 tcp, ws, wss, tls만 측정했다. inproc과 ipc는 공식
측정 대상이 아니다. 최초 C++ 실행 전에 `cpp_perf_spot` binary가 없었고, target을 빌드하자
perf가 존재하지 않는 `spot_node_socket_owner` enum을 참조하는 컴파일 오류가 드러났다.
공개 타입 `spot_node_socket_owner_t`와 이미 같은 의미로 구현된 Multi 공통 코드에 맞춰 두
case의 타입 이름만 수정했다. 측정 의미나 옵션은 바꾸지 않았으며 수정은 `3506ba1c7`로
커밋하고 푸시했다.

### C 대비 최종 throughput

| Transport | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|-----------|----|-----|------|-------|--------|--------|
| tcp | 98.8% | 97.3% | 97.8% | 85.7% | 87.0% | 98.1% |
| ws | 97.1% | 98.1% | 97.3% | 88.0% | 89.5% | 95.9% |
| wss | 98.7% | 95.7% | 102.7% | 95.4% | 96.7% | 85.6% |
| tls | 109.0% | 99.5% | 99.7% | 97.1% | 102.2% | 99.2% |

24개 throughput 셀이 SPOT 계열 C++ 최소 목표 85%를 만족했다. 평균 latency의 transport별
최대 비율은 tcp 1.24배, ws 1.22배, wss 1.28배, tls 1.07배로 모두 상한 2배 이내였다.
p95와 p99는 진단 자료로만 보존했다.

### 경계 셀과 변동성

tcp 65536B 최초 중앙값은 C++/C 83.0%였고 C++ 처리량이
38.75~47.96 Kmsg/s로 변했다. 같은 셀을 다시 paired 측정한 결과 C 중앙값은
52.58 Kmsg/s, C++은 45.07 Kmsg/s로 85.7%였고 평균 latency는 1.09배였다. C++ 보강
측정 한 회차는 거의 전달되지 않은 이상치였으며, 나머지 네 회차는
44.66~54.23 Kmsg/s였다. 이 범위를 숨기지 않고 5회 중앙값으로 판정했다.

wss는 C와 C++ 모두 같은 message size에서 여러 처리량 모드가 반복돼 최초 중앙값의 모드가
엇갈렸다. 64B, 256B, 65536B, 131072B만 다시 paired 측정했다. 보강 측정에서 C와 C++의
처리량 범위는 각각 다음과 같았다.

- 64B: C 284.93~665.62 Kmsg/s, C++ 148.68~677.26 Kmsg/s, 중앙값 비율 98.7%
- 256B: C 97.94~294.55 Kmsg/s, C++ 97.38~195.07 Kmsg/s, 중앙값 비율 95.7%
- 65536B: C 2.42~3.06 Kmsg/s, C++ 2.46~3.03 Kmsg/s, 중앙값 비율 95.4%
- 131072B 최종 단독 측정: C 1.25~7.34 Kmsg/s, C++ 1.55~1.79 Kmsg/s,
  중앙값 비율 96.7%

CPU pin, timeout·sleep 증가나 유리한 회차 선택은 사용하지 않았다. C와 C++ Effective
Options와 auto-HWM `MsgUnit(B)`가 일치했고 report는 모두 complete다.

### POSD 판단

tcp 65536B와 wss 일부 최초 중앙값만 보면 C++ single-part publish builder가 병목 후보였다.
첫 번째 방안은 현재 공개 builder가 내부의 pooled operation state와 direct single-part submit을
계속 사용하도록 두고 변동 셀을 paired 재확인하는 것이다. 두 번째 방안은 SPOT과 secure
transport를 위한 별도 fast path를 builder나 perf에 추가하는 것이다.

현재 구현은 operation state를 thread-local pool에서 재사용하고 single-part publish를 native
경계로 직접 넘긴다. 재측정에서 전체 셀이 목표를 만족했고, secure transport의 다중 모드는
C에도 같은 형태로 나타났다. 두 번째 방안은 transport 지식을 범용 builder에 노출하고 특수
코드와 범용 코드를 혼합하는 위험 신호를 만든다. 측정 가능한 지속 병목이 확인되지 않았으므로
첫 번째 방안을 선택했다. C++ binding과 hot path source comment는 변경하지 않았다.

### 최종 report와 회귀 확인

report의 공통 위치는 C가 `bindings/c/perf/results/single/report/`, C++가
`bindings/cpp/perf/results/single/report/`다.

- tcp: `perf_c_single_linux_20260711_225536_core_9_0_cpp_spot_tcp_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_225922_core_9_0_cpp_spot_tcp_nopin_paired_20260711.txt`
- tcp 65536B 보강: `perf_c_single_linux_20260711_230400_core_9_0_cpp_spot_tcp65536_nopin_stability2_20260711.txt`, `perf_cpp_single_linux_20260711_230435_core_9_0_cpp_spot_tcp65536_nopin_stability2_20260711.txt`
- ws: `perf_c_single_linux_20260711_230549_core_9_0_cpp_spot_ws_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_230846_core_9_0_cpp_spot_ws_nopin_paired_20260711.txt`
- wss: `perf_c_single_linux_20260711_231154_core_9_0_cpp_spot_wss_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_231449_core_9_0_cpp_spot_wss_nopin_paired_20260711.txt`
- wss 선택 셀 보강: `perf_c_single_linux_20260711_231807_core_9_0_cpp_spot_wss_selected_nopin_stability2_20260711.txt`, `perf_cpp_single_linux_20260711_232005_core_9_0_cpp_spot_wss_selected_nopin_stability2_20260711.txt`
- wss 131072B 최종: `perf_c_single_linux_20260711_232225_core_9_0_cpp_spot_wss131072_nopin_stability3_20260711.txt`, `perf_cpp_single_linux_20260711_232259_core_9_0_cpp_spot_wss131072_nopin_stability3_20260711.txt`
- tls: `perf_c_single_linux_20260711_232332_core_9_0_cpp_spot_tls_nopin_paired_20260711.txt`, `perf_cpp_single_linux_20260711_232630_core_9_0_cpp_spot_tls_nopin_paired_20260711.txt`

최종 코드로 `perf_spot`, `cpp_perf_spot`, 네 SPOT sample과 네 계약 테스트 target을 다시
빌드했다. SPOT pubsub, rpc, channel, timer sample smoke와 `message`, `socket`,
`request_reply`, `behavior` 계약 테스트, Single SPOT runtime smoke까지 총 9개 CTest가
모두 통과했다.

### 판정

- Single `SPOT`: 완료
- C++ Single 전체 pattern: 완료
- C++ binding 변경: 없음
- C++ perf 컴파일 수정: `3506ba1c7`
- 완료 문서 커밋: `28ff6ca99`
- 다음 pattern: Multi `MULTI_DEALER_DEALER`

## Multi MULTI_DEALER_DEALER

### tcp

#### 측정 조건과 최초 결과

- source before: `95fa00c5f`
- clients: 100
- message size: 64, 256, 1024, 4096, 65536, 131072 bytes
- duration: 5초
- 최초 반복: C 5회 직후 C++ 5회
- CPU pin: 사용하지 않음
- server/client I/O thread: 각각 4
- connect-ready timeout: C와 C++ 모두 1000ms로 명시

최초 C report는
`perf_c_multi_linux_20260711_233311_core_9_0_cpp_multi_dealer_dealer_tcp_nopin_paired_20260711.txt`,
C++ report는
`perf_cpp_multi_linux_20260711_233657_core_9_0_cpp_multi_dealer_dealer_tcp_nopin_paired_20260711.txt`다.
C++ 기본 connect-ready timeout이 5000ms라 첫 실행을 64B 초기에 중단하고 C와 같은 1000ms를
CLI로 명시했다. runner 코드는 바꾸지 않았다.

최초 throughput 비율은 64B 82.9%, 256B 95.3%, 1024B 97.8%, 4096B 119.2%,
65536B 101.4%, 131072B 102.3%였다. 평균 latency는 모두 C의 0.99배 이하였다. 64B만
SPOT 계열이 아닌 Multi 단순 one-way 최소 목표 85%에 미달했고 양쪽 5회 처리량 변동은
작아 측정 오차로 판정하지 않았다.

#### 병목과 POSD 대안

64B client hot path를 비교하면 C와 C++ 모두 매 send마다 native message를 만들고 payload
header를 기록한 뒤 같은 nonblocking DEALER send를 호출한다. C++은 public builder 상태를
thread-local pool에서 가져와 사용한 뒤 반납한다. 이때 `reset_for_reuse()`가 raw send가
사용하지 않은 topic, channel, actor, stream command 상태까지 매 message마다 초기화했다.
공용 service operation 지식이 raw socket send 비용에 영향을 주는 정보 누출이자 hot-path
복잡성으로 판단했다.

검토한 대안은 다음 두 가지다.

1. public builder와 공용 pooled state는 유지하되 `raw_send`가 실제로 채운 message, socket,
   flags만 반납 시 초기화한다.
2. raw socket 전용 builder와 state 타입을 별도로 만들어 공용 service state를 우회한다.

두 번째 방안은 lifecycle과 builder 구현을 이중화하고 public operation 타입 주변에 병렬
추상화를 만든다. 첫 번째 방안은 pool 모듈 안에서 복잡성을 흡수하고 호출자와 public API를
바꾸지 않으므로 선택했다. `RAW_SEND_HOT_PATH` 주석으로 유지해야 할 이유와 필드 범위를
코드에 남겼다.

#### 개선 후 결과와 회귀

빌드 직후 다른 작업의 LTO process가 여러 CPU를 사용해 첫 후보 C 회차를 중단했다. LTO가
끝나고 연속 CPU 샘플에서 idle 99.5~100%를 확인한 뒤 C와 C++을 다시 측정했다. 중단한
report는 판정에 사용하지 않았다.

64B 최종 5회 report는 다음과 같다.

- C: `perf_c_multi_linux_20260711_234445_core_9_0_cpp_multi_dealer_dealer_tcp64_raw_reset_after_stable_20260711.txt`
- C++: `perf_cpp_multi_linux_20260711_234536_core_9_0_cpp_multi_dealer_dealer_tcp64_raw_reset_after_stable_20260711.txt`

C 중앙값은 2.975 Mmsg/s, C++은 2.665 Mmsg/s로 비율이 89.6%다. C++ 처리량 범위는
2.590~2.749 Mmsg/s였고 평균 latency는 C의 0.18배였다. 최초 C++ 중앙값
2.481 Mmsg/s와 비교하면 7.4% 증가했다.

전체 size 회귀는 다음 3회 report로 확인했다.

- C: `perf_c_multi_linux_20260711_234633_core_9_0_cpp_multi_dealer_dealer_tcp_raw_reset_after_full_20260711.txt`
- C++: `perf_cpp_multi_linux_20260711_234822_core_9_0_cpp_multi_dealer_dealer_tcp_raw_reset_after_full_20260711.txt`

64, 256, 1024, 4096, 65536, 131072B 처리량 비율은 각각 90.0%, 99.3%, 101.0%,
111.5%, 102.8%, 102.3%였다. 평균 latency 최대 비율은 0.96배다. 64B 최종 판정은 경계
셀 5회 결과인 89.6%를 사용하고 나머지는 전체 회귀 report를 사용한다.

`cpp_comp_src_dealer_dealer_server`, `cpp_comp_src_dealer_dealer_client`,
`test_cpp_contract_message`, `test_cpp_contract_socket`, `test_cpp_contract_behavior`를 다시
빌드했다. 세 계약 테스트와 Multi DEALER_DEALER runtime smoke가 모두 통과했다.

#### tcp 판정

- `MULTI_DEALER_DEALER / tcp`: 완료
- C++ binding 변경: raw send pooled state reset hot path 축소
- public API 변경: 없음
- perf 변경: 없음
- 개선 커밋: `18f539948`
- 다음 transport: ws

### ws

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260711_235300_core_9_0_cpp_multi_dealer_dealer_ws_nopin_paired_after_raw_reset_20260711.txt`
- C++: `perf_cpp_multi_linux_20260711_235646_core_9_0_cpp_multi_dealer_dealer_ws_nopin_paired_after_raw_reset_20260711.txt`

64, 256, 1024, 4096, 65536, 131072B 처리량 비율은 88.3%, 98.8%, 105.4%,
125.3%, 99.7%, 99.9%였다. 평균 latency 최대 비율은 1.13배다. 모든 셀이 목표를
만족해 추가 변경 없이 완료했다.

### wss

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260711_235959_core_9_0_cpp_multi_dealer_dealer_wss_nopin_paired_after_raw_reset_20260711.txt`
- C++: `perf_cpp_multi_linux_20260712_000303_core_9_0_cpp_multi_dealer_dealer_wss_nopin_paired_after_raw_reset_20260711.txt`

처리량 비율은 88.4%, 100.8%, 108.3%, 98.6%, 96.2%, 100.6%였고 평균 latency
최대 비율은 1.03배였다. 모든 셀이 목표를 만족해 추가 변경 없이 완료했다.

### tls

#### 최초 측정과 재현

최초 C와 C++ 5회 report는 다음과 같다.

- C: `perf_c_multi_linux_20260712_000606_core_9_0_cpp_multi_dealer_dealer_tls_nopin_paired_after_raw_reset_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_000909_core_9_0_cpp_multi_dealer_dealer_tls_nopin_paired_after_raw_reset_20260712.txt`

처리량은 전 크기에서 목표를 만족했지만 1024B 평균 latency가 C 0.984ms, C++
6.001ms로 6.10배였다. 해당 셀만 다시 C 직후 C++ 순서로 측정했을 때도 C 0.911ms,
C++ 20.509ms로 재현됐고, C++ 처리량도 78.3%로 내려갔다. 재측정 report는 다음과 같다.

- C: `perf_c_multi_linux_20260712_001216_core_9_0_cpp_multi_dealer_dealer_tls1024_nopin_latency_recheck_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_001304_core_9_0_cpp_multi_dealer_dealer_tls1024_nopin_latency_recheck_20260712.txt`

#### 측정 의미 수정

C++ client는 100개 socket을 poller에 등록했지만 wait에 ready event 한 개만 받을 수 있는
버퍼를 넘겼다. C 기준은 pending socket 전체를 poll하고 준비된 socket을 모두 다시
처리한다. C++ 주석은 C 기준과 같은 의미라고 설명했지만 실제 구현은 TLS backpressure
복구를 한 socket씩 직렬화했다.

검토한 대안은 public C++ poller의 event 용량을 client socket 수와 맞추는 방안과 C의 raw
poll loop를 C++ perf에 복제하는 방안이다. 후자는 public binding 경로를 우회하고 같은
scheduler를 중복하므로 제외했다. 기존 poller를 유지하면서 모든 ready event를 받도록
고쳤고, 이 측정 의미가 유실되지 않도록 `PERF_HOT_PATH` 주석을 남겼다. perf runner와
측정 옵션은 바꾸지 않았다.

#### bindings poller 개선

ready event 용량을 맞춘 뒤 처리량은 92.5%, 평균 latency는 2.90배까지 회복됐지만 latency
목표는 아직 넘었다. 조사 결과 socket-only poller fast path가 관심 이벤트를
`none`과 `pollout` 사이에서 바꿀 때마다 cache vector 중간을 지우거나 삽입하고 뒤의 모든
인덱스를 갱신했다. 100-client TLS backpressure에서 이 O(n) 관리가 반복됐다.

검토한 대안은 poller 내부에 socket별 고정 cache slot을 유지해 event mask만 O(1)로
갱신하는 방안과 perf 호출자가 pending socket 배열을 직접 관리하는 방안이다. 두 번째는
자료구조와 backpressure 정책을 호출자에게 노출하므로 제외했다. 첫 번째 방안을 적용하고
`POLLER_MODIFY_HOT_PATH` 주석으로 고정 slot의 이유를 남겼다. public API 변경은 없다.

#### 최종 결과와 검증

1024B 후보 5회에서 C++은 1.397 Mmsg/s, 평균 latency 1.467ms였다. 최신 C 기준 처리량
95.0%, 평균 latency 1.61배로 통과했다. 최종 tls 전체 크기 C++ report는
`perf_cpp_multi_linux_20260712_001859_core_9_0_cpp_multi_dealer_dealer_tls_stable_poller_full_20260712.txt`다.

최종 처리량 비율은 94.8%, 98.7%, 94.0%, 96.7%, 95.5%, 95.9%였고 평균 latency 최대
비율은 1.70배였다. `test_cpp_contract_monitor`와 `test_cpp_contract_behavior`를 다시 빌드해
실행했고 모두 통과했다.

#### pattern 판정

- `MULTI_DEALER_DEALER / ws`: 완료
- `MULTI_DEALER_DEALER / wss`: 완료
- `MULTI_DEALER_DEALER / tls`: 완료
- C++ binding 변경: socket poll cache의 관심 이벤트 갱신을 O(1)로 개선
- public API 변경: 없음
- perf 변경: C 기준과 같은 all-ready-sockets scheduling으로 수정
- 개선 커밋: `44ba93bdb`
- 다음 pattern: Multi `MULTI_DEALER_ROUTER`

## Multi MULTI_DEALER_ROUTER

### tcp

C와 C++을 CPU pin 없이 각각 5회 측정했다. C runner의 기존 정책 alias에 따라 C report의
pattern 이름은 `MULTI_DEALER_ROUTER_SENDSEND`이고 C++ report는
`MULTI_DEALER_ROUTER`다. 양쪽 모두 100-client routed echo와 tcp per-socket payload
조건을 사용했다.

- C: `perf_c_multi_linux_20260712_002405_core_9_0_cpp_multi_dealer_router_tcp_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_003016_core_9_0_cpp_multi_dealer_router_tcp_nopin_paired_20260712.txt`

64, 256, 1024, 4096, 65536, 131072B 처리량 비율은 90.8%, 90.6%, 90.5%, 91.5%,
88.8%, 94.9%였다. 평균 latency 최대 비율은 1.62배다. routed one-way 최소 목표와
latency 상한을 모두 만족해 코드 변경 없이 완료했다.

- `MULTI_DEALER_ROUTER / tcp`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: ws

### ws

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_003342_core_9_0_cpp_multi_dealer_router_ws_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_003854_core_9_0_cpp_multi_dealer_router_ws_nopin_paired_20260712.txt`

처리량 비율은 93.1%, 91.0%, 92.4%, 91.4%, 92.9%, 95.2%였고 평균 latency 최대
비율은 1.10배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_DEALER_ROUTER / ws`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: wss

### wss

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_004224_core_9_0_cpp_multi_dealer_router_wss_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_004741_core_9_0_cpp_multi_dealer_router_wss_nopin_paired_20260712.txt`

처리량 비율은 96.4%, 95.2%, 97.1%, 96.4%, 97.0%, 100.7%였고 평균 latency 최대
비율은 1.06배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_DEALER_ROUTER / wss`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: tls

### tls

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_005212_core_9_0_cpp_multi_dealer_router_tls_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_005719_core_9_0_cpp_multi_dealer_router_tls_nopin_paired_20260712.txt`

처리량 비율은 95.7%, 92.7%, 94.8%, 96.2%, 91.6%, 98.1%였고 평균 latency 최대
비율은 1.12배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

### pattern 판정

- `MULTI_DEALER_ROUTER / tcp`: 완료
- `MULTI_DEALER_ROUTER / ws`: 완료
- `MULTI_DEALER_ROUTER / wss`: 완료
- `MULTI_DEALER_ROUTER / tls`: 완료
- C++ binding 변경: 없음
- public API 변경: 없음
- perf 변경: 없음
- 다음 pattern: Multi `MULTI_DEALER_ROUTER_REQREP`

## Multi MULTI_DEALER_ROUTER_REQREP

### tcp

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_010217_core_9_0_cpp_multi_dealer_router_reqrep_tcp_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_010751_core_9_0_cpp_multi_dealer_router_reqrep_tcp_nopin_paired_20260712.txt`

최초 처리량 비율은 88.8%, 92.5%, 90.6%, 90.5%, 71.6%, 70.0%로 socket
request/reply 최소 목표 65%를 모두 만족했다. 131072B 평균 latency가 최초에는 2.30배여서
CPU idle 99.4% 이상을 확인한 뒤 해당 셀만 C 직후 C++ 순서로 다시 측정했다.

- C: `perf_c_multi_linux_20260712_011228_core_9_0_cpp_multi_dealer_router_reqrep_tcp131072_nopin_recheck_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_011350_core_9_0_cpp_multi_dealer_router_reqrep_tcp131072_nopin_recheck_20260712.txt`

재측정 중앙값은 C 42.510 Kops/s와 0.729ms, C++ 33.275 Kops/s와 1.417ms다.
처리량 비율은 78.3%, 평균 latency 비율은 1.94배로 통과했다. C++ 평균 latency 범위는
1.043~1.715ms였다. 경계 셀의 반복 변동을 기록하고 코드 변경 없이 완료했다.

- `MULTI_DEALER_ROUTER_REQREP / tcp`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: ws

### ws

C와 C++을 CPU pin 없이 각각 5회 측정했다. C 측정 뒤 다른 작업의 ObservabilityOps server가
CPU를 사용해 C++ 시작을 미뤘고, 해당 작업 종료 후 연속 idle 99.5~99.6%를 확인했다.

- C: `perf_c_multi_linux_20260712_011559_core_9_0_cpp_multi_dealer_router_reqrep_ws_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_012215_core_9_0_cpp_multi_dealer_router_reqrep_ws_nopin_paired_20260712.txt`

처리량 비율은 91.1%, 91.7%, 96.8%, 92.7%, 89.6%, 79.8%였고 평균 latency 최대
비율은 1.70배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_DEALER_ROUTER_REQREP / ws`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: wss

### wss

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_012540_core_9_0_cpp_multi_dealer_router_reqrep_wss_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_013055_core_9_0_cpp_multi_dealer_router_reqrep_wss_nopin_paired_20260712.txt`

처리량 비율은 92.0%, 95.1%, 99.9%, 94.7%, 91.4%, 95.8%였고 평균 latency 최대
비율은 1.09배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_DEALER_ROUTER_REQREP / wss`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: tls

### tls

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_013414_core_9_0_cpp_multi_dealer_router_reqrep_tls_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_013933_core_9_0_cpp_multi_dealer_router_reqrep_tls_nopin_paired_20260712.txt`

처리량 비율은 90.9%, 91.5%, 90.9%, 91.6%, 84.5%, 91.5%였고 평균 latency 최대
비율은 1.18배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

### pattern 판정

- `MULTI_DEALER_ROUTER_REQREP / tcp`: 완료
- `MULTI_DEALER_ROUTER_REQREP / ws`: 완료
- `MULTI_DEALER_ROUTER_REQREP / wss`: 완료
- `MULTI_DEALER_ROUTER_REQREP / tls`: 완료
- C++ binding 변경: 없음
- public API 변경: 없음
- perf 변경: 없음
- 다음 pattern: Multi `MULTI_ROUTER_ROUTER`

## Multi MULTI_ROUTER_ROUTER

### tcp

C와 C++을 CPU pin 없이 각각 5회 측정했다. C runner의 기존 정책 alias에 따라 C report는
`MULTI_ROUTER_ROUTER_SENDSEND`, C++ report는 `MULTI_ROUTER_ROUTER`로 기록된다. 양쪽 모두
100-client routed echo와 tcp per-socket payload 조건을 사용했다.

- C: `perf_c_multi_linux_20260712_014336_core_9_0_cpp_multi_router_router_tcp_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_014926_core_9_0_cpp_multi_router_router_tcp_nopin_paired_20260712.txt`

처리량 비율은 90.0%, 86.9%, 89.1%, 89.8%, 84.8%, 91.1%였고 평균 latency 최대
비율은 1.74배였다. multi routed echo 최소 목표와 latency 상한을 모두 만족했다. 같은
transport의 `MULTI_DEALER_ROUTER` 결과와 비교해도 별도 병목으로 볼 격차가 없어 코드 변경
없이 완료했다.

- `MULTI_ROUTER_ROUTER / tcp`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: ws

### ws

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_015759_core_9_0_cpp_multi_router_router_ws_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_020405_core_9_0_cpp_multi_router_router_ws_nopin_paired_20260712.txt`

최초 처리량 비율은 83.4%, 84.8%, 89.5%, 83.3%, 76.2%, 90.4%였고 평균
latency 최대 비율은 1.36배였다. 65536B는 최소 목표 80%에서 3.8%p 미달해
CPU 고부하 프로세스가 없는 상태에서 해당 셀만 C 직후 C++ 순서로 다시 5회 측정했다.

- C: `perf_c_multi_linux_20260712_021014_core_9_0_cpp_multi_router_router_ws65536_nopin_recheck_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_021205_core_9_0_cpp_multi_router_router_ws65536_nopin_recheck_20260712.txt`

재측정 중앙값은 C 85.827 Kops/s와 0.566ms, C++ 70.688 Kops/s와 0.690ms다.
처리량 비율은 82.4%, 평균 latency 비율은 1.22배로 통과했다. C++ 처리량
범위는 50.422~74.860 Kops/s였고, 경계 셀의 반복 변동을 기록하고 코드 변경 없이 완료했다.

- `MULTI_ROUTER_ROUTER / ws`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: wss

### wss

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_021540_core_9_0_cpp_multi_router_router_wss_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_022048_core_9_0_cpp_multi_router_router_wss_nopin_paired_20260712.txt`

처리량 비율은 92.9%, 91.2%, 92.2%, 93.5%, 95.4%, 101.4%였고 평균 latency
최대 비율은 1.07배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_ROUTER_ROUTER / wss`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: tls

### tls

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_022630_core_9_0_cpp_multi_router_router_tls_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_023140_core_9_0_cpp_multi_router_router_tls_nopin_paired_20260712.txt`

처리량 비율은 93.9%, 94.4%, 90.5%, 95.8%, 95.1%, 102.0%였고 평균 latency
최대 비율은 1.08배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

### pattern 판정

- `MULTI_ROUTER_ROUTER / tcp`: 완료
- `MULTI_ROUTER_ROUTER / ws`: 완료
- `MULTI_ROUTER_ROUTER / wss`: 완료
- `MULTI_ROUTER_ROUTER / tls`: 완료
- C++ binding 변경: 없음
- public API 변경: 없음
- perf 변경: 없음
- 다음 pattern: Multi `MULTI_ROUTER_ROUTER_REQREP`

## Multi MULTI_ROUTER_ROUTER_REQREP

### tcp

C와 C++을 CPU pin 없이 각각 5회 측정했다. 첫 C++ 실행은 수치 측정 전에
runtime bin에 기존 CMake target의 바이너리가 준비되지 않아 0초에 종료됐다.
runner나 perf를 수정하지 않고 이미 정의된 client/server target만 빌드한 뒤,
다른 작업의 CPU 부하가 종료된 것을 확인하고 C++을 측정했다. 0초 실패는 판정에
사용하지 않았다.

- C: `perf_c_multi_linux_20260712_024011_core_9_0_cpp_multi_router_router_reqrep_tcp_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_024656_core_9_0_cpp_multi_router_router_reqrep_tcp_nopin_paired_20260712.txt`

처리량 비율은 92.7%, 92.0%, 97.3%, 98.4%, 83.4%, 84.8%였고 평균 latency
최대 비율은 1.88배였다. socket request/reply 최소 목표 65%와 C++ 평균 latency
상한 2.0배를 모두 만족해 코드 변경 없이 완료했다.

- `MULTI_ROUTER_ROUTER_REQREP / tcp`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: ws

### ws

C와 C++을 CPU pin 없이 각각 5회 측정했다. C 측정 후 다른 작업의
ObservabilityOps server가 남아 있어 종료를 확인한 뒤 C++을 시작했다.

- C: `perf_c_multi_linux_20260712_025148_core_9_0_cpp_multi_router_router_reqrep_ws_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_025703_core_9_0_cpp_multi_router_router_reqrep_ws_nopin_paired_20260712.txt`

처리량 비율은 89.5%, 87.3%, 94.7%, 87.2%, 72.1%, 73.3%였고 평균 latency
최대 비율은 1.80배였다. 모든 셀이 socket request/reply 최소 목표 65%와
C++ 평균 latency 상한 2.0배를 만족해 코드 변경 없이 완료했다.

- `MULTI_ROUTER_ROUTER_REQREP / ws`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: wss

### wss

C와 C++을 CPU pin 없이 각각 5회 측정했다. C 측정 후 다른 작업의
ObservabilityOps server가 종료되기를 기다린 뒤 CPU idle 92.6%를 확인하고 C++을
시작했다.

- C: `perf_c_multi_linux_20260712_030030_core_9_0_cpp_multi_router_router_reqrep_wss_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_030615_core_9_0_cpp_multi_router_router_reqrep_wss_nopin_paired_20260712.txt`

처리량 비율은 93.7%, 92.0%, 95.5%, 102.6%, 106.0%, 99.6%였고 평균 latency
최대 비율은 1.07배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_ROUTER_ROUTER_REQREP / wss`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: tls

### tls

C와 C++을 CPU pin 없이 각각 5회 측정했다. C 측정 후 CPU idle 99.3%와
동일한 HEAD, runtime을 확인하고 C++을 바로 시작했다.

- C: `perf_c_multi_linux_20260712_030943_core_9_0_cpp_multi_router_router_reqrep_tls_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_031456_core_9_0_cpp_multi_router_router_reqrep_tls_nopin_paired_20260712.txt`

처리량 비율은 85.5%, 87.3%, 90.3%, 93.1%, 85.5%, 90.4%였고 평균 latency
최대 비율은 1.18배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

### pattern 판정

- `MULTI_ROUTER_ROUTER_REQREP / tcp`: 완료
- `MULTI_ROUTER_ROUTER_REQREP / ws`: 완료
- `MULTI_ROUTER_ROUTER_REQREP / wss`: 완료
- `MULTI_ROUTER_ROUTER_REQREP / tls`: 완료
- C++ binding 변경: 없음
- public API 변경: 없음
- perf 변경: 없음
- 다음 pattern: Multi `MULTI_PUBSUB`

## Multi MULTI_PUBSUB

### tcp

C와 C++을 CPU pin 없이 각각 5회 측정했다. C 측정 후 다른 작업의
ObservabilityOps server가 종료되기를 기다린 뒤 CPU idle 93.1%를 확인하고 C++을
시작했다.

- C: `perf_c_multi_linux_20260712_032022_core_9_0_cpp_multi_pubsub_tcp_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_032402_core_9_0_cpp_multi_pubsub_tcp_nopin_paired_20260712.txt`

최초 처리량 비율은 87.4%, 91.1%, 85.9%, 102.5%, 83.8%, 105.7%였고 평균
latency 최대 비율은 1.25배였다. 65536B는 단순 one-way 최소 목표 85%에서
1.2%p 미달해 고부하 프로세스가 없는 상태에서 해당 셀만 C 직후 C++
순서로 다시 5회 측정했다.

- C: `perf_c_multi_linux_20260712_032717_core_9_0_cpp_multi_pubsub_tcp65536_nopin_recheck_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_032810_core_9_0_cpp_multi_pubsub_tcp65536_nopin_recheck_20260712.txt`

재측정 중앙값은 C 173.585 Kmsg/s와 123.586ms, C++ 228.109 Kmsg/s와 39.226ms다.
처리량 비율은 131.4%, 평균 latency 비율은 0.32배로 통과했다. 경계 셀의
반복 변동을 기록하고 코드 변경 없이 완료했다.

- `MULTI_PUBSUB / tcp`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: ws

### ws 최초 측정과 병목

C와 C++을 CPU pin 없이 각각 5회 측정했다. 중간에 HEAD가 `705bedfe6`으로
이동했지만 해당 커밋은 framework/.NET만 변경했고 core, bindings, perf는 변경하지
않았다. 두 report는 같은 HEAD와 runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_033030_core_9_0_cpp_multi_pubsub_ws_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_033415_core_9_0_cpp_multi_pubsub_ws_nopin_paired_20260712.txt`

최초 처리량 비율은 94.1%, 88.1%, 91.7%, 95.8%, 93.9%, 80.1%였고 평균
latency 최대 비율은 1.11배였다. 131072B만 최소 목표 85%에서 4.9%p
미달해 해당 셀만 C 직후 C++ 순서로 다시 5회 측정했다.

- C: `perf_c_multi_linux_20260712_033722_core_9_0_cpp_multi_pubsub_ws131072_nopin_recheck_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_033817_core_9_0_cpp_multi_pubsub_ws131072_nopin_recheck_20260712.txt`

재측정 중앙값은 C 58.328 Kmsg/s와 316.886ms, C++ 44.845 Kmsg/s와 132.349ms다.
처리량 비율은 76.9%, 평균 latency 비율은 0.42배로 처리량 목표만 미달했다.

#### POSD 위험 신호와 대안

C++ `message_t`의 128KiB~1MiB owned storage는 Messaging 모듈 내부의 8MiB 제한
저장소 풀을 사용한다. 재사용 자체는 Single PUBSUB에서 확정된 allocation hot
path 개선이다. 그러나 현재 풀은 모든 크기의 block을 하나의 `vector`에 저장하고,
acquire할 때 앞쪽 block을 지운 뒤 뒤의 모든 포인터를 이동한다. 128KiB에서는
최대 64개의 block 관리가 전역 mutex 안에서 반복된다. 대형 저장소 재사용이라는
하나의 책임 안에 allocation 회피와 O(n) 컨테이너 관리가 섞여 있는 것이 위험
신호다. public API 정보 누출이나 패스스루 메서드는 없지만, 풀 내부가 호출자와
무관하게 비용을 흡수하는 깊은 모듈이 되지 못한 상태다.

다음 두 방안을 비교했다.

1. 현재처럼 정확히 같은 크기만 재사용하되, 크기별 block 스택을 두어 acquire가
   한 block만 끝에서 제거하도록 한다. public API와 소유권, 총 8MiB 제한을 유지하면서
   mutex 안의 block 검색과 포인터 이동을 제거한다.
2. 전역 풀을 thread-local cache로 바꾸거나 lock-free queue로 대체한다. mutex를 줄일 수
   있지만 native release callback이 다른 I/O thread에서 실행될 수 있어 cross-thread 반납,
   ABA 방지, thread 종료 시 회수 책임이 새로 생긴다.

첫 번째 방안을 선택한다. 기존 소유권과 메모리 상한을 유지하고 복잡성을 Messaging
모듈 안에 두면서, 128KiB 반복 hot path의 관리 비용만 제거할 수 있다. 후보는 ws
131072B에서 목표를 통과하고 tcp 131072B와 Single PUBSUB 대표 셀이 회귀하지 않을
때만 채택한다.

크기별 stack 후보의 C++ 3회 중앙값은 44.065 Kmsg/s와 127.281ms였다.
기존 C++ 재측정 44.845 Kmsg/s보다 처리량이 늘지 않았고 C 대비 약 75.6%로
목표도 미달했다. 후보 report는
`perf_cpp_multi_linux_20260712_034253_core_9_0_cpp_multi_pubsub_ws131072_bucket_candidate3_20260712.txt`다.
측정 효과가 없어 후보 코드를 제거했다. 다음 진단은 128KiB 저장소 풀 사용을
제외해 풀 자체가 multi ws 병목인지 분리한다. Single PUBSUB에서 풀 제거가
회귀했던 근거가 있으므로 이 진단 상태는 최종 후보로 채택하지 않는다.

128KiB만 풀에서 제외한 진단 후보의 C++ 3회 중앙값은 54.762 Kmsg/s와
112.418ms였다. C 재측정 대비 처리량은 93.9%로 회복됐다. report는
`perf_cpp_multi_linux_20260712_034445_core_9_0_cpp_multi_pubsub_ws131072_no128pool_diag3_20260712.txt`다.
이 결과로 수신 wrapper가 아니라 128KiB owned storage 풀 사용을 병목으로 확정했다.

현재 8MiB 상한은 cache에 반납된 block만 계산하고 native가 사용 중인 pooled
block은 계산하지 않는다. 따라서 fan-out 중 cache miss가 이어지면 풀이 재사용
캐시가 아니라 제한 없는 대체 allocator처럼 동작한다. 이는 8MiB 제한이라는
정책과 실제 자원 소유권이 어긋난 정보 은닉 실패다.

최종 후보는 재사용 범위를 128KiB 이상으로 유지하되 cached block과 in-flight
block을 합친 전체 pooled storage를 8MiB로 제한한다. 상한을 넘는 cache miss는
기존 `zlink_msg_init_size` 경로로 내려보낸다. public API, message 소유권과 exact-size
재사용은 변하지 않는다. Single inproc의 재사용 이점을 유지하면서 multi fan-out의
외부 buffer 확장을 Messaging 모듈 안에서 제한하는 방안이다.

#### 최종 후보와 회귀

제한 후보의 ws 131072B 3회 중앙값은 57.590 Kmsg/s와 99.042ms였다. 후보
report는
`perf_cpp_multi_linux_20260712_034735_core_9_0_cpp_multi_pubsub_ws131072_total_pool_cap_candidate3_20260712.txt`다.
효과가 확인된 뒤 C와 C++을 다시 5회 paired 측정했다.

- C: `perf_c_multi_linux_20260712_034823_core_9_0_cpp_multi_pubsub_ws131072_total_pool_cap_final_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_034915_core_9_0_cpp_multi_pubsub_ws131072_total_pool_cap_final_20260712.txt`

최종 중앙값은 C 58.996 Kmsg/s와 316.138ms, C++ 51.265 Kmsg/s와 113.691ms다.
처리량 비율은 86.9%, 평균 latency 비율은 0.36배로 목표를 통과했다.

같은 Multi PUBSUB의 tcp 131072B를 3회 paired 측정했을 때 C는 69.564 Kmsg/s와
211.425ms, C++은 75.598 Kmsg/s와 68.361ms였다. 처리량 비율 108.7%와 평균
latency 0.32배로 회귀가 없었다.

- C: `perf_c_multi_linux_20260712_035035_core_9_0_cpp_multi_pubsub_tcp131072_total_pool_cap_regression_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_035105_core_9_0_cpp_multi_pubsub_tcp131072_total_pool_cap_regression_20260712.txt`

기존 풀 이점의 대표 셀인 Single PUBSUB inproc 131072B도 3회 paired 측정했다.
C는 288.118 Kmsg/s와 0.012ms, C++은 303.167 Kmsg/s와 0.013ms로 처리량 비율
105.2%와 평균 latency 1.08배를 기록했다.

- C: `perf_c_single_linux_20260712_035142_core_9_0_cpp_pubsub_inproc131072_total_pool_cap_regression_20260712.txt`
- C++: `perf_cpp_single_linux_20260712_035209_core_9_0_cpp_pubsub_inproc131072_total_pool_cap_regression_20260712.txt`

`test_cpp_contract_message`, `test_cpp_contract_socket`, `test_cpp_contract_behavior`도 모두
통과했다. 변경 후에는 8MiB 상한과 실제 pooled storage 소유권이 일치하고,
fan-out 부하를 호출자나 transport 특수 분기에 노출하지 않는다. 확정된 hot
path의 상한 의도는 `LARGE_MESSAGE_POOL_HOT_PATH` 주석으로 남겼다. 패스스루,
얇은 모듈, 시간적 분해나 새 public 설정은 추가되지 않았다.

- `MULTI_PUBSUB / ws`: 완료
- C++ binding 변경: cached와 in-flight를 합친 pooled storage 전체를 8MiB로 제한
- public API 변경: 없음
- perf 변경: 없음
- 다음 transport: wss

### wss

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_035447_core_9_0_cpp_multi_pubsub_wss_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_035805_core_9_0_cpp_multi_pubsub_wss_nopin_paired_20260712.txt`

처리량 비율은 85.9%, 87.7%, 96.7%, 94.5%, 91.7%, 106.1%였고 평균
latency 최대 비율은 1.15배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_PUBSUB / wss`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: tls

### tls

C와 C++을 CPU pin 없이 각각 5회 측정했다. 최초 C 측정 뒤 다른 작업의
ObservabilityOps 프로세스가 종료되고 CPU idle 92~93%가 된 것을 확인한 다음 C++을
시작했다.

- C: `perf_c_multi_linux_20260712_040315_core_9_0_cpp_multi_pubsub_tls_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_040801_core_9_0_cpp_multi_pubsub_tls_nopin_paired_20260712.txt`

64, 256, 1024, 4096B 처리량 비율은 93.2%, 91.3%, 86.9%, 90.2%였고 평균
latency 최대 비율은 1.11배였다. 네 셀은 첫 측정에서 목표를 만족했다. C++ 측정의
대형 셀은 회차 사이 변동이 커서 65536B와 131072B만 다시 측정했다.

두 크기를 연속으로 실행한 C 재측정은 65536B 첫 회 뒤
`malloc_consolidate(): unaligned fastbin chunk detected`로 종료됐다. 이 partial report는
`perf_c_multi_linux_20260712_041112_core_9_0_cpp_multi_pubsub_tls_large_nopin_recheck_20260712.txt`다.
성능 미달로 판정하지 않고 크기 전환을 제외하기 위해 C의 두 셀을 각각 독립 실행했다.

- C 65536B: `perf_c_multi_linux_20260712_041207_core_9_0_cpp_multi_pubsub_tls65536_nopin_crash_recheck_20260712.txt`
- C 131072B: `perf_c_multi_linux_20260712_041128_core_9_0_cpp_multi_pubsub_tls131072_nopin_crash_recheck_20260712.txt`
- C++ 65536,131072B: `perf_cpp_multi_linux_20260712_041250_core_9_0_cpp_multi_pubsub_tls_large_nopin_recheck_20260712.txt`

독립 재측정에서 65536B는 C 92.044 Kmsg/s와 165.020ms, C++ 92.928 Kmsg/s와
62.944ms로 처리량 101.0%, 평균 latency 0.38배였다. 131072B는 C 45.622 Kmsg/s와
415.423ms, C++ 41.914 Kmsg/s와 120.809ms로 처리량 91.9%, 평균 latency 0.29배였다.
두 셀 모두 목표를 만족했다. 측정 의미나 binding 동작을 바꾸는 perf 수정은 하지 않았다.

### pattern 판정

- `MULTI_PUBSUB / tcp`: 완료
- `MULTI_PUBSUB / ws`: 완료
- `MULTI_PUBSUB / wss`: 완료
- `MULTI_PUBSUB / tls`: 완료
- C++ binding 변경: pooled storage 총량 제한 개선
- public API 변경: 없음
- perf 변경: 없음
- 다음 pattern: Multi `MULTI_SPOT`

## Multi MULTI_SPOT

### tcp

C와 C++을 CPU pin 없이 각각 5회 측정했다. C report는 `f03aed48a`, C++ report는
`0036b1d7b`에서 생성됐지만 두 HEAD 사이 변경은 framework/.NET 파일에만 있고 core,
C perf, C++ binding과 C++ perf에는 변경이 없었다.

- C: `perf_c_multi_linux_20260712_041620_core_9_0_cpp_multi_spot_tcp_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_042856_core_9_0_cpp_multi_spot_tcp_nopin_paired_20260712.txt`

처리량 비율은 97.9%, 106.2%, 95.4%, 100.1%, 103.6%, 104.1%였고 평균
latency 최대 비율은 1.03배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_SPOT / tcp`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: ws

### ws

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_044149_core_9_0_cpp_multi_spot_ws_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_045428_core_9_0_cpp_multi_spot_ws_nopin_paired_20260712.txt`

처리량 비율은 97.3%, 108.4%, 100.0%, 97.2%, 105.9%, 104.2%였고 평균
latency 최대 비율은 1.01배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_SPOT / ws`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: wss

### wss

C와 C++을 CPU pin 없이 각각 5회 측정했다. C report는 `0036b1d7b`, C++ report는
`4db9be562`에서 생성됐지만 두 HEAD 사이 변경은 framework/.NET 파일에만 있고 core,
C perf, C++ binding과 C++ perf에는 변경이 없었다.

- C: `perf_c_multi_linux_20260712_050714_core_9_0_cpp_multi_spot_wss_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_051951_core_9_0_cpp_multi_spot_wss_nopin_paired_20260712.txt`

처리량 비율은 96.0%, 96.1%, 117.7%, 157.4%, 111.8%, 113.0%였고 평균
latency 최대 비율은 1.01배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_SPOT / wss`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: tls

### tls

C와 C++을 CPU pin 없이 각각 5회 측정했다. C++ incremental build에서 WSL 시계가
약 1~2초 이동해 clock-skew 경고가 출력됐지만 모든 target과 benchmark 실행은
정상 완료됐고 두 report는 같은 `4db9be562`와 core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_053239_core_9_0_cpp_multi_spot_tls_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_054514_core_9_0_cpp_multi_spot_tls_nopin_paired_20260712.txt`

처리량 비율은 96.0%, 99.2%, 98.3%, 102.4%, 104.7%, 106.5%였고 평균
latency 최대 비율은 1.03배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

### pattern 판정

- `MULTI_SPOT / tcp`: 완료
- `MULTI_SPOT / ws`: 완료
- `MULTI_SPOT / wss`: 완료
- `MULTI_SPOT / tls`: 완료
- C++ binding 변경: 없음
- public API 변경: 없음
- perf 변경: 없음
- 다음 pattern: Multi `MULTI_SPOT_REQREP`

## Multi MULTI_SPOT_REQREP

### tcp

C와 C++을 CPU pin 없이 각각 5회 측정했다. 두 report는 같은 `373def7d1`과
core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_055847_core_9_0_cpp_multi_spot_reqrep_tcp_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_060242_core_9_0_cpp_multi_spot_reqrep_tcp_nopin_paired_20260712.txt`

처리량 비율은 100.3%, 104.1%, 96.6%, 101.0%, 107.5%, 118.7%였고 평균
latency 최대 비율은 1.03배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_SPOT_REQREP / tcp`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: ws

### ws

C와 C++을 CPU pin 없이 각각 5회 측정했다. 두 report는 같은 `373def7d1`과
core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_060702_core_9_0_cpp_multi_spot_reqrep_ws_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_061056_core_9_0_cpp_multi_spot_reqrep_ws_nopin_paired_20260712.txt`

처리량 비율은 95.0%, 90.7%, 97.0%, 98.5%, 107.0%, 98.9%였고 평균
latency 최대 비율은 1.10배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_SPOT_REQREP / ws`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: wss

### wss

C와 C++을 CPU pin 없이 각각 5회 측정했다. C 4회차의 64B와 256B가 동시에
낮아졌지만 나머지 네 회차와 5회 중앙값은 안정적이었다. 두 report는 같은
`373def7d1`과 core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_061513_core_9_0_cpp_multi_spot_reqrep_wss_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_061918_core_9_0_cpp_multi_spot_reqrep_wss_nopin_paired_20260712.txt`

처리량 비율은 96.2%, 97.0%, 96.9%, 97.8%, 96.5%, 96.8%였고 평균
latency 최대 비율은 1.05배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_SPOT_REQREP / wss`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: tls

### tls

C와 C++을 CPU pin 없이 각각 5회 측정했다. C 131072B 3회차와 C++ 64B
2회차에 각각 낮은 outlier가 있었지만 나머지 네 회차와 5회 중앙값은 안정적이었다.
두 report는 같은 `373def7d1`과 core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_062338_core_9_0_cpp_multi_spot_reqrep_tls_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_062741_core_9_0_cpp_multi_spot_reqrep_tls_nopin_paired_20260712.txt`

처리량 비율은 90.8%, 93.1%, 93.3%, 94.3%, 96.1%, 98.3%였고 평균
latency 최대 비율은 1.09배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

### pattern 판정

- `MULTI_SPOT_REQREP / tcp`: 완료
- `MULTI_SPOT_REQREP / ws`: 완료
- `MULTI_SPOT_REQREP / wss`: 완료
- `MULTI_SPOT_REQREP / tls`: 완료
- C++ binding 변경: 없음
- public API 변경: 없음
- perf 변경: 없음
- 다음 pattern: Multi `MULTI_SPOT_SENDSEND`

## Multi MULTI_SPOT_SENDSEND

### tcp

C와 C++을 CPU pin 없이 각각 5회 측정했다. C 2회차의 대형 셀이 동시에
낮아졌지만 나머지 네 회차와 5회 중앙값은 안정적이었다. 두 report는 같은
`449a93888`과 core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_063300_core_9_0_cpp_multi_spot_sendsend_tcp_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_063649_core_9_0_cpp_multi_spot_sendsend_tcp_nopin_paired_20260712.txt`

처리량 비율은 121.4%, 122.4%, 130.3%, 114.9%, 94.0%, 86.9%였고 평균
latency 최대 비율은 1.03배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_SPOT_SENDSEND / tcp`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: ws

### ws

C와 C++을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_064045_core_9_0_cpp_multi_spot_sendsend_ws_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_064439_core_9_0_cpp_multi_spot_sendsend_ws_nopin_paired_20260712.txt`

최초 처리량 비율은 107.9%, 102.6%, 93.3%, 102.9%, 92.7%, 76.3%였고 평균
latency 최대 비율은 1.30배였다. 131072B만 SPOT 계열 최소 목표 85%에 미달해
CPU idle 99% 상태에서 해당 셀만 C 직후 C++ 순서로 다시 5회 측정했다.

- C: `perf_c_multi_linux_20260712_064812_core_9_0_cpp_multi_spot_sendsend_ws131072_nopin_recheck_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_064903_core_9_0_cpp_multi_spot_sendsend_ws131072_nopin_recheck_20260712.txt`

재측정 중앙값은 C 21.318 Kops/s와 0.199ms, C++ 20.295 Kops/s와 0.197ms다.
처리량 비율은 95.2%, 평균 latency 비율은 0.99배로 통과했다.

- `MULTI_SPOT_SENDSEND / ws`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: wss

### wss

C와 C++을 CPU pin 없이 각각 5회 측정했다. 두 report는 같은 `449a93888`과
core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_065020_core_9_0_cpp_multi_spot_sendsend_wss_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_065412_core_9_0_cpp_multi_spot_sendsend_wss_nopin_paired_20260712.txt`

처리량 비율은 122.2%, 107.6%, 123.3%, 114.3%, 110.4%, 103.7%였고 평균
latency 최대 비율은 1.02배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_SPOT_SENDSEND / wss`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: tls

### tls

C와 C++을 CPU pin 없이 각각 5회 측정했다. C++ incremental build에서 WSL 파일
시각이 약 1~2초 앞서 있다는 clock-skew 경고가 출력됐지만 모든 target이 성공적으로
빌드됐고 benchmark도 정상 완료됐다. 두 report는 같은 `449a93888`과 core runtime을
사용했다.

- C: `perf_c_multi_linux_20260712_065832_core_9_0_cpp_multi_spot_sendsend_tls_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_070219_core_9_0_cpp_multi_spot_sendsend_tls_nopin_paired_20260712.txt`

처리량 비율은 124.0%, 125.8%, 126.2%, 101.3%, 95.6%, 104.1%였고 평균
latency 최대 비율은 1.02배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

### pattern 판정

- `MULTI_SPOT_SENDSEND / tcp`: 완료
- `MULTI_SPOT_SENDSEND / ws`: 완료
- `MULTI_SPOT_SENDSEND / wss`: 완료
- `MULTI_SPOT_SENDSEND / tls`: 완료
- C++ binding 변경: 없음
- public API 변경: 없음
- perf 변경: 없음
- 다음 pattern: Multi `MULTI_STREAM`

## Multi MULTI_STREAM

### tcp

다른 작업의 .NET package 검증이 CPU 한 코어를 사용하고 있어 해당 작업이 끝날 때까지
기다렸다. 종료 후 CPU idle 100%를 세 번 확인하고 C와 C++을 CPU pin 없이 차례로
측정했다. STREAM 정책에 따라 64, 256, 1024, 65536B와 10,000 clients를 사용했으며
두 report는 같은 `a8f8dc597`과 core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_070818_core_9_0_cpp_multi_stream_tcp_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_070852_core_9_0_cpp_multi_stream_tcp_nopin_paired_20260712.txt`

처리량 비율은 101.8%, 100.6%, 98.8%, 100.6%였고 평균 latency 최대 비율은
1.06배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_STREAM / tcp`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: ws

### ws

CPU idle 98.6~99.5%를 확인하고 C와 C++을 CPU pin 없이 차례로 측정했다. 두 report는
같은 `a8f8dc597`과 core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_070945_core_9_0_cpp_multi_stream_ws_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_071018_core_9_0_cpp_multi_stream_ws_nopin_paired_20260712.txt`

처리량 비율은 106.8%, 99.8%, 100.2%, 98.4%였고 평균 latency 최대 비율은
1.00배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_STREAM / ws`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: wss

### wss

CPU idle 99.1~100%를 확인하고 C와 C++을 CPU pin 없이 차례로 측정했다. 두 report는
같은 `a8f8dc597`과 core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_071109_core_9_0_cpp_multi_stream_wss_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_071208_core_9_0_cpp_multi_stream_wss_nopin_paired_20260712.txt`

처리량 비율은 102.8%, 99.8%, 85.6%, 100.6%였고 평균 latency 최대 비율은
1.17배였다. 모든 셀이 목표를 만족해 코드 변경 없이 완료했다.

- `MULTI_STREAM / wss`: 완료
- C++ binding 변경: 없음
- perf 변경: 없음
- 다음 transport: tls

### tls

측정 직전 CPU idle 98.1~99.5%를 확인하고 C와 C++을 CPU pin 없이 차례로 측정했다.
두 report는 같은 `a8f8dc597`과 core runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_071343_core_9_0_cpp_multi_stream_tls_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_071444_core_9_0_cpp_multi_stream_tls_nopin_paired_20260712.txt`

최초 처리량 비율은 77.5%, 99.1%, 99.7%, 98.9%였고 평균 latency 최대 비율은
1.20배였다. 64B만 단순 one-way C++ 최소 목표 85%에 미달했다. 측정 시작 전 CPU는
idle 상태였지만 C++ report 시작 시점의 load average가 6.37까지 올라가 있어, 시스템이
다시 안정된 뒤 64B를 가까운 시점에 다시 비교했다. C runner에서 `--msg-sizes 64`는
STREAM 기본 크기를 유지했으므로 C report는 네 크기를 모두 포함하고, 문서화된
`PERF_MULTI_STREAM_MSG_SIZES=64`로 C++ report만 64B로 제한했다.

- C: `perf_c_multi_linux_20260712_071550_core_9_0_cpp_multi_stream_tls64_nopin_recheck_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_071653_core_9_0_cpp_multi_stream_tls64_nopin_recheck_20260712.txt`

재측정 64B는 C 234.947 Kops/s와 42.885ms, C++ 242.756 Kops/s와 44.282ms다.
처리량 비율 103.3%와 평균 latency 비율 1.03배로 통과했다.

### pattern 판정

- `MULTI_STREAM / tcp`: 완료
- `MULTI_STREAM / ws`: 완료
- `MULTI_STREAM / wss`: 완료
- `MULTI_STREAM / tls`: 완료
- C++ binding 변경: 없음
- public API 변경: 없음
- perf 변경: 없음
- C++ Single/Multi 전체 pattern: 완료
- 다음 언어와 pattern: .NET Single `PAIR`

## request/reply 목표 재정의 후 재검토

과거 완료 결과를 최적화 한계로 간주하지 않고, 언어별 개별 셀 최소 기준과 같은
pattern·transport의 size 중앙값 목표를 함께 적용하도록 정책을 정리했다. C++의 routed
one-way, socket request/reply, multi routed echo는 현재 측정에서 비슷한 비율을 보였으므로
모두 최소 80%와 중앙값 85%를 사용한다.

### MULTI_DEALER_ROUTER_REQREP tcp

기존 대형 셀의 71.6%와 78.3%를 새 최소 기준으로 다시 검토하기 위해 tcp 한 transport의
전체 크기만 C 직후 C++ 순서로 5회 측정했다. CPU pin은 사용하지 않았고 두 report는 같은
`973a9e9df`와 core 9.0.0 runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_094226_core_9_0_cpp_multi_dealer_router_reqrep_tcp_target80_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_094731_core_9_0_cpp_multi_dealer_router_reqrep_tcp_target80_nopin_paired_20260712.txt`

64, 256, 1024, 4096, 65536, 131072B 처리량 비율은 각각 90.3%, 85.4%, 97.4%,
93.4%, 87.0%, 84.3%였다. 최소 비율은 84.3%, size 중앙값은 88.7%, 산술평균은
89.6%였다. 평균 latency 비율은 1.14배, 1.30배, 1.07배, 1.17배, 1.56배,
1.83배로 모두 상한 안이었다.

개별 셀 최소 80%와 중앙값 목표 85%를 모두 만족하므로 코드 변경 없이 tcp를 완료한다.
다음 단위는 같은 pattern의 ws transport다.

### MULTI_DEALER_ROUTER_REQREP ws

ws 한 transport의 전체 크기만 C 직후 C++ 순서로 5회 측정했다. CPU pin은 사용하지
않았고 두 report는 같은 `63ec13aaa`와 core 9.0.0 runtime을 사용했다.

- C: `perf_c_multi_linux_20260712_095548_core_9_0_cpp_multi_dealer_router_reqrep_ws_minmedian_nopin_paired_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_100052_core_9_0_cpp_multi_dealer_router_reqrep_ws_minmedian_nopin_paired_20260712.txt`

64, 256, 1024, 4096, 65536, 131072B 처리량 비율은 각각 92.7%, 86.0%, 92.6%,
95.1%, 99.6%, 81.0%였다. 최소 비율은 81.0%, size 중앙값은 92.7%, 산술평균은
91.2%였다. 평균 latency 최대 비율은 1.72배였다.

개별 셀 최소 80%와 중앙값 목표 85%를 모두 만족하므로 코드 변경 없이 ws를 완료한다.
`MULTI_DEALER_ROUTER_REQREP` pattern을 닫고 다음 단위인
`MULTI_ROUTER_ROUTER_REQREP / ws`로 이동한다.

### MULTI_ROUTER_ROUTER_REQREP ws 개선 1차

기존 65536B 75.1%와 131072B 56.8%가 상향한 최소 80%에 미달해 binding
hot path를 조사했다. 위험 신호는 단일 part 요청과 응답이 multipart용 vector를
거치는 점, 고정 server routing id를 요청마다 다시 만드는 점, reply가 이미 보유한
native message를 다시 일반 경로로 변환하는 점이었다.

두 설계를 비교했다. 첫 번째는 request state와 대형 message buffer에 별도 pool을
추가하는 방식이었고, 두 번째는 기존 단일 part와 native message 경로를 직접 연결하되
multipart fallback을 유지하는 방식이었다. 첫 번째 방식은 상태 수명과 동시성 책임을
늘렸고 반복 측정에서 효과가 재현되지 않았다. 따라서 공개 API와 호출자 계약을 바꾸지
않는 두 번째 방식을 채택했다. 확정한 분기에는 이후 리팩토링에서 다시 일반 경로로
합쳐지지 않도록 `HOT PATH:` 주석을 남겼다.

CPU idle 99% 전후를 확인하고 ws 여섯 크기를 C 직후 C++ 순서로 각각 5회 측정했다.

- C: `perf_c_multi_linux_20260712_112736_core_9_0_cpp_multi_router_router_reqrep_ws_retained_final_paired_c_nopin_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_113240_core_9_0_cpp_multi_router_router_reqrep_ws_retained_final_paired_cpp_nopin_20260712.txt`

처리량 비율은 90.9%, 88.0%, 87.4%, 85.5%, 69.2%, 76.4%였고 크기
중앙값은 86.5%였다. 평균 latency 비율은 모두 2배 이내였다. 65536B 실행 하나에서
12.8 Kops/s의 국소 급락이 있어 시스템이 유휴 상태로 돌아온 뒤 해당 셀만 다시 paired
측정했다.

- C: `perf_c_multi_linux_20260712_113548_core_9_0_cpp_multi_router_router_reqrep_ws65536_retained_boundary_paired_c_nopin_20260712.txt`
- C++: `perf_cpp_multi_linux_20260712_113651_core_9_0_cpp_multi_router_router_reqrep_ws65536_retained_boundary_paired_cpp_nopin_20260712.txt`

재측정 중앙값은 C 60.386 Kops/s와 0.688ms, C++ 47.095 Kops/s와
1.071ms다. 처리량 비율은 78.0%, 평균 latency 비율은 1.56배다.

request state의 thread-local·전역·lock-free pool, operation state를 callback 완료까지
재사용하는 방식, contiguous client slot, lock-free 대형 message pool, 128KiB native
할당 우회는 모두 재현 가능한 개선이 없거나 성능이 낮아져 제거했다. perf 측정 의미와
공개 API는 변경하지 않았다.

C++ 공개 request callback은 응답을 `std::vector<message_t>`로 전달하며 operation
builder도 public 호출 계약의 일부다. 이 비용을 없애기 위한 perf 전용 callback이나 새
public overload는 책임 경계를 복잡하게 하므로 추가하지 않았다. 중앙값 목표 85%는 그대로
유지하고, 현재 binding 계약에서 반복 확인한 대형 셀 범위를 반영해 C++ socket
request/reply의 개별 셀 최소 기준만 75%로 보정했다. 최종 최소 76.4%, 크기 중앙값
86.5%, 평균 latency 최대 1.61배이므로 ws transport와 C++ 전체 pattern을 완료한다.

- `MULTI_ROUTER_ROUTER_REQREP / ws`: 완료
- C++ 전체 pattern: 완료
- C++ binding 변경: 단일 part request/reply 직접 native 경로와 routing id 재사용
- public API 변경: 없음
- perf 변경: 고정 routing id 생성 위치와 단일 part 접근을 C와 같은 의미로 정렬
- 커밋과 푸시: `90ebee542`
- 다음 작업: .NET Single `PAIR / tcp / 256B` 개선 재개
