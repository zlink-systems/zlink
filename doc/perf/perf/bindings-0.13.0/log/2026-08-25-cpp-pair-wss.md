# C++ Single PAIR / wss — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch | `core-0.13.0-bindings-performance` |
| source/report commit | `7b45b44f97` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke

`PAIR / wss / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- C: `perf_c_single_linux_20260825_092958_cpp-pair-wss-core0130-smoke-c-20260825.txt`
- C++: `perf_cpp_single_linux_20260825_092959_cpp-pair-wss-core0130-smoke-cpp-20260825.txt`

## 최초 paired 기준선 — 3 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_093140_cpp-pair-wss-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_093323_cpp-pair-wss-core0130-before-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- size별 throughput ratio: 85.22%, 83.40%, 95.18%, 94.45%, 93.37%, 98.22%
- throughput aggregate: **91.64%**
- mean-latency aggregate: **1.019x**

## 자체 pass와 Sol read-only pass

WSS 인증서와 TLS client 설정은 측정 전 setup에서만 수행된다. active 구간의 PAIR
send/recv는 tcp/ws와 같은 `pair_socket_t` 공개 경로에서 `zlink_send_part`와
`zlink_recv_part`로 들어가며, WSS framing, 암복호화와 TLS record 처리는 고정된 Core
0.13.0 runtime이 소유한다. C++ 수신은 이미 caller-provided `message_t`와 빈 출력 메시지
fast path를 사용한다.

자체 검토와 Sol의 파일 수정 없는 독립 검토 모두 WSS 전용의 계약 안전한 binding 후보가
없다고 결론냈다. C7 재시도, submit mutex 또는 weak lifetime 검사 제거, C API/transport별
우회, large-message pool은 성능 근거가 없거나 공개 ownership/lifetime/error 계약을
위반하므로 적용하지 않았다. secure transport이고 3회 aggregate가 목표 경계에 가까워
계획서 규칙에 따라 5회 paired 결과로 최종 판정했다.

## 최종 경계 판정 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_093700_cpp-pair-wss-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_093945_cpp-pair-wss-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,949,930.8 | 1,823,802.2 | 93.53% | 56.092 ms | 59.729 ms | 1.065x |
| 256 | 691,459.8 | 668,363.2 | 96.66% | 35.223 ms | 40.715 ms | 1.156x |
| 1024 | 218,613.0 | 211,377.0 | 96.69% | 40.598 ms | 34.151 ms | 0.841x |
| 65536 | 7,915.2 | 6,841.6 | 86.44% | 14.039 ms | 17.774 ms | 1.266x |
| 131072 | 4,409.6 | 4,414.0 | 100.10% | 14.924 ms | 16.643 ms | 1.115x |
| 262144 | 2,559.0 | 2,294.4 | 89.66% | 11.726 ms | 13.778 ms | 1.175x |

- throughput ratio 산술평균: **93.85%**
- mean-latency ratio 산술평균: **1.103x** — 2.0x 상한 통과
- 65536B와 262144B는 개별 90% 미달 진단값이지만 계획의 aggregate 판정을 뒤집지 않는다.

기본 95% 목표에는 미달한다. 자체/Sol pass에서 새 WSS binding 후보가 없었으므로 계획서가
허용한 C++ 단순 one-way 완화 목표 **90%를 선택**한다. 최종 aggregate 93.85%가 이를
충족해 **PAIR/wss는 통과**다.
