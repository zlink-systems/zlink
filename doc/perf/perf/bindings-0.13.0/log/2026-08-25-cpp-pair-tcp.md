# C++ Single PAIR / tcp — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch | `core-0.13.0-bindings-performance` |
| source commit | `7556f3f8ff` (latest `main` merge 후 측정 commit metadata) |
| source version | `0.13.2` (최신 `main` 병합 결과) |
| Core runtime version | `0.13.0` |
| Core release tag | `core/v0.13.0` |
| Core revision | `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| Core runtime | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` |
| Core runtime SHA-256 | `96f18853a19e06fe22b0c13bbbb70b9409516547102b5542e862386c3f9d06f7` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2` |
| CPU | Intel Core i7-1260P, logical CPU 16 |
| memory | 11 GiB |
| CPU pin | 사용 안 함 |
| 동시 perf process | 없음 |

최신 `main` 병합으로 repository version은 0.13.2다. 공식 측정은
`--core-version 0.13.0`이 선택한 release prefix와 provenance를 사용하며
`ZLINK_CORE_ALLOW_VERSION_MISMATCH=1`은 source version 차이만 허용한다. report의
`core_version`, `core_runtime`, `core_revision`, `core_release_tag`가 모두
0.13.0 release package를 가리키는지 별도로 검증한다.

## Inventory와 parity 수정

- C와 C++ Single runner의 pattern은 `PAIR`, `PUBSUB`, `DEALER_DEALER`,
  `DEALER_ROUTER`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER`,
  `ROUTER_ROUTER_REQREP`로 일치한다.
- Single 기본 size는 64, 256, 1024, 65536, 131072, 262144 bytes다.
- Single socket transport는 Linux에서 tcp, tls, ws, wss, inproc, ipc다.
- C++ report가 Core release metadata를 기록하지 않던 문제를 수정했다.
- C++ Multi STREAM 기본 client 수가 C의 100과 달리 10,000이던 문제를 100으로 정렬했다.

## Smoke

조건: `PAIR / tcp / 64B / duration=1 / runs=1 / io_threads=1`,
auto-HWM balanced, CPU pin 없음.

| 언어 | report | status | throughput | mean latency |
|------|--------|--------|------------|--------------|
| C | `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_082956_cpp-pair-tcp-core0130-parityfix-smoke-20260825.txt` | complete | 3,122,240 msg/s | 0.299 ms |
| C++ | `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_083003_cpp-pair-tcp-core0130-parityfix-smoke-20260825.txt` | complete | 2,606,397 msg/s | 0.126 ms |

두 report는 동일한 Core version, runtime, revision, release tag와 Effective Options를
기록했다.

## Before paired 측정

pair tag: `cpp-pair-tcp-core0130-before-20260825`

조건: `PAIR / tcp / 64,256,1024,65536,131072,262144B / duration=5 / runs=1 /
io_threads=1`, auto-HWM balanced, CPU pin 없음.

- C report:
  `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_083017_cpp-pair-tcp-core0130-before-20260825.txt`
- C++ report:
  `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_083054_cpp-pair-tcp-core0130-before-20260825.txt`
- 두 report status: `complete`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 3,080,284.6 | 2,461,352.8 | 79.91% | 0.227 ms | 0.163 ms | 0.718x |
| 256 | 1,699,730.8 | 1,521,419.2 | 89.51% | 0.987 ms | 1.111 ms | 1.126x |
| 1024 | 660,236.6 | 541,877.6 | 82.07% | 0.730 ms | 0.897 ms | 1.229x |
| 65536 | 32,675.0 | 25,633.4 | 78.45% | 0.295 ms | 0.537 ms | 1.820x |
| 131072 | 23,545.0 | 15,342.0 | 65.16% | 0.405 ms | 0.407 ms | 1.005x |
| 262144 | 13,145.8 | 10,839.8 | 82.46% | 0.312 ms | 0.377 ms | 1.208x |

- throughput ratio 산술평균: **79.59%** — 목표 95% 미달
- throughput ratio 중앙값: 80.99%
- mean-latency ratio 산술평균: **1.184x** — 상한 2.0x 통과
- mean-latency ratio 중앙값: 1.167x

## 자체 pass 진단

`-pg` 진단 빌드로 131072B를 3초 실행했다. Core release runtime은 계측 대상이
아니어서 시간 sample은 Core 내부에 집중됐지만 C++ public 경계 호출 수는 확인됐다.
약 6.5만 active send마다 다음 경로가 한 번씩 실행됐다.

1. `message_t::from` payload allocation/copy
2. `pair_socket_t::send` operation-state acquire
3. callback-state lifetime 확인
4. outbound submit 직렬화 gate
5. `send_submit_operation_t::submit`

C reference도 payload message allocation/copy를 수행하므로 이를 제거하는 perf 전용
우회는 후보에서 제외한다. socket close와 multipart interleave를 보호하는 submit gate,
operation builder의 ownership/error 복구도 계약 확인 없이 제거하지 않는다.

계측 후 C++ build cache의 `CMAKE_CXX_FLAGS`와 `CMAKE_EXE_LINKER_FLAGS`는 빈 값으로
복원하고 `ENABLE_LTO=OFF` 공식 구성을 다시 build했다.

## Sol read-only pass

Sol 검토는 파일 수정 없이 진행했다. 다음 항목을 확인했다.

- C/C++ report의 Core 0.13.0 runtime, revision, option은 동일하다.
- C++ hot path의 message wrapper와 fluent builder, 정상 경로 예외 경계는 public binding
  비용이므로 perf 전용 C API 호출이나 direct-send 우회로 제거하면 안 된다.
- submit mutex 제거는 multipart interleave와 close/native-submit 동시 실행 계약을 깨뜨릴
  수 있고, weak lifetime token 제거는 socket보다 오래 사는 builder에서 UAF가 되므로
  후보에서 제외한다.
- large-message pool은 계획에서 금지되며 다시 도입하지 않는다.
- 가장 안전한 자체 후보는 TLS operation-state pool의 fixed-capacity array(C7)지만,
  vector가 첫 reuse 뒤 재할당하지 않아 기대 효과는 작으므로 단독 A/B가 필요하다.
- C++ PAIR 하네스의 매-message `.flags(none)` 호출은 state 기본값과 reset 값이 이미
  `none`이라 의미 없는 out-of-line setter다. 수신 counter의 release increment도 join 뒤
  읽는 C 기준의 relaxed increment와 달랐다.

## Harness parity 수정 후 paired 측정

다음 의미 보존 수정만 적용했다.

1. C++ PAIR active send에서 기본값과 동일한 `.flags(none)` setter 호출 제거
2. receiver thread join으로 동기화되는 수신 counter increment를 C와 같은 relaxed로 변경

pair tag: `cpp-pair-tcp-core0130-parity-ab-{c,cpp}-20260825`

- C report:
  `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_084219_cpp-pair-tcp-core0130-parity-ab-c-20260825.txt`
- C++ report:
  `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_084400_cpp-pair-tcp-core0130-parity-ab-cpp-20260825.txt`
- 조건: 6 sizes, duration 5초, runs 3 중앙값, Core 0.13.0 release
- 두 report status: `complete`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,549,964.0 | 2,128,402.4 | 83.47% | 0.254 ms | 0.198 ms | 0.780x |
| 256 | 1,400,600.8 | 1,246,234.2 | 88.98% | 1.209 ms | 1.368 ms | 1.132x |
| 1024 | 556,139.0 | 518,786.2 | 93.28% | 0.875 ms | 0.938 ms | 1.072x |
| 65536 | 28,296.2 | 25,643.6 | 90.63% | 0.351 ms | 0.448 ms | 1.276x |
| 131072 | 20,493.4 | 16,281.4 | 79.45% | 0.299 ms | 0.524 ms | 1.753x |
| 262144 | 12,393.0 | 12,234.4 | 98.72% | 0.430 ms | 0.334 ms | 0.777x |

- throughput ratio 산술평균: **89.09%** — 목표 95%, 완화 기준 90% 미달
- mean-latency ratio 산술평균: **1.131x** — 상한 2.0x 통과

run 사이 host throughput 하락이 있었지만 계획 규칙에 따라 변동을 판정 유예 사유로
사용하지 않고 3회 중앙값을 최종 paired 값으로 사용한다.

## C7 fixed-capacity pool A/B와 기각

`std::vector<std::unique_ptr<operation_state_t>>` TLS pool을 동일 capacity 8의
`std::array + size`로 바꾸고 capacity+1 builder, reset, overflow allocation을 검사하는
전용 계약 테스트를 추가했다. 전용 테스트는 통과했다.

- C7 report:
  `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_084923_cpp-pair-tcp-core0130-c7-cpp-20260825.txt`
- vector crossover report:
  `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_085208_cpp-pair-tcp-core0130-vector-crossover-cpp-20260825.txt`

첫 순서에서는 C7/vector 대비 크기별 처리량 변화가
`+5.4%, +8.8%, +2.9%, -10.8%, +15.9%, -12.9%`로 섞였다. 순서를 뒤집은
vector crossover까지 비교하면 C7의 크기별 결과가 다시 교차하고 aggregate는 vector보다
약 2.6% 낮았다. 재현 가능한 개선이 아니므로 C7 코드와 전용 테스트는 모두 제거했다.

기존 `test_cpp_contract_request_reply`는 C7 적용/제거 양쪽에서 기존
`test_reply_submit_is_one_shot_without_ghost_retry` 또는 SNDTIMEO assertion으로 실패했다.
C7 전용 테스트보다 앞에서 실패했고 vector 복원 뒤에도 동일하게 재현되어 C7 회귀는 아니다.
이 기존 branch test 실패는 별도 회귀 항목으로 남긴다.

## 판정

- 자체 hot-path pass 완료
- Sol read-only pass 완료
- 계약을 유지하며 채택할 추가 라이브러리 후보 없음
- 최종 throughput aggregate 89.09%로 목표 미달, latency 1.131x로 통과
- **C++ Single PAIR / tcp: 보류**
