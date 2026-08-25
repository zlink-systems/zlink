# C++ Single PAIR / tls — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch | `core-0.13.0-bindings-performance` |
| source/report commit | `12fe9f8b5b` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke

`PAIR / tls / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- C: `perf_c_single_linux_20260825_094421_cpp-pair-tls-core0130-smoke-c-20260825.txt`
- C++: `perf_cpp_single_linux_20260825_094428_cpp-pair-tls-core0130-smoke-cpp-20260825.txt`

## 최초 paired 기준선 — 3 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_094437_cpp-pair-tls-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_094617_cpp-pair-tls-core0130-before-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- size별 throughput ratio: 78.07%, 88.66%, 93.34%, 75.58%, 92.22%, 78.84%
- throughput aggregate: **84.45%**
- mean-latency aggregate: **1.163x**

## 자체 pass와 Sol read-only pass

TLS 인증서와 client 설정은 측정 전 setup에서만 수행되고 active 구간의 TLS record와
암복호화는 고정 Core 0.13.0 runtime이 소유한다. C++ PAIR 송신은 이미 thread-local
pooled operation state, lvalue single-part borrow와 직접 `zlink_send_part` terminal을
사용한다. 수신도 caller-provided `message_t`의 empty-output fast path를 거쳐 직접
`zlink_recv_part`를 호출한다.

남은 builder state/liveness 검사, submit mutex, 실패 시 message ownership 복구와
submit result/errno 매핑은 공개 fluent builder, concurrent close, ownership 및 error
계약을 지킨다. 자체 검토와 Sol의 파일 수정 없는 독립 검토 모두 다음 후보를 기각했다.

- fixed pool 재설계: PAIR/tcp C7 crossover에서 이미 기각됐고 TLS 전용 binding 근거가 없음
- submit mutex/weak liveness 제거: native submit과 close race 및 escaped builder 수명 회귀
- state inline화 또는 direct terminal 추가: public builder layout/signature 변경
- C API 직접 호출 또는 PAIR/TLS 전용 우회: 일반 public binding 경로가 아님
- large-message pool: 계획에서 C++ binding 적용을 명시적으로 금지

따라서 자체 pass와 Sol pass는 모두 **no-go**이며 라이브러리 소스 변경을 남기지 않았다.
TLS가 secure transport이므로 계획서 규칙에 따라 변경 없는 최종 5회 paired 결과로
판정했다.

## 최종 판정 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_095114_cpp-pair-tls-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_095401_cpp-pair-tls-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,599,854.0 | 2,176,252.8 | 83.71% | 1.395 ms | 0.514 ms | 0.368x |
| 256 | 958,073.8 | 815,184.0 | 85.09% | 1.752 ms | 2.173 ms | 1.240x |
| 1024 | 289,438.8 | 247,209.0 | 85.41% | 1.698 ms | 2.002 ms | 1.179x |
| 65536 | 10,241.2 | 9,479.4 | 92.56% | 0.981 ms | 1.046 ms | 1.066x |
| 131072 | 6,514.0 | 5,642.6 | 86.62% | 1.132 ms | 1.047 ms | 0.925x |
| 262144 | 3,656.4 | 2,866.0 | 78.38% | 1.083 ms | 1.376 ms | 1.271x |

- throughput ratio 산술평균: **85.29%** — 완화 목표 90% 미달
- mean-latency ratio 산술평균: **1.008x** — 2.0x 상한 통과

관련 회귀 gate는 stale `build-contract` 바이너리를 현재 HEAD로 재빌드한 뒤
`test_cpp_contract_message`와 `test_cpp_contract_socket` 모두 통과했다. 기본 95%와 완화
90% 처리량 목표에 모두 미달하고 두 개선 pass에서 계약 안전한 후보가 없었으므로
**PAIR/tls는 보류**다.
