# ST 경량 게이트 요약

## 범위·보존

- 감독관이 main index에 적용한 ST-1/2/3 staged patch를 그대로 검증했다. pull·apply·stash·commit은 수행하지 않았다.
- staged 범위는 Core socket runtime 4개 소스, `core/tests/CMakeLists.txt`, 신규 `test_stream_concurrent_pull_send.cpp` 1개다. 최종에도 이 6개는 staged 상태로 남아 있다.
- 첫 release 시도는 재실행 메모리 규칙을 재확인하며 다른 job의 `ninja=1`을 발견해 49/86 뒤 중단했다(exit 130, 컴파일 오류 없음). 60초 뒤 `ninja=0`, available 10673 MiB에서 이어서 성공했다.

## 빌드·CTest

| 명령 | 결과 |
|---|---|
| `JOBS=4 scripts/build-core.sh dev` | PASS |
| 전체 `ctest --test-dir core/build-dev -j2 --output-on-failure` | 기능 **210/210 PASS**, 252.15초 |
| 전체 명령에서 함께 등록된 `hotpath_gate` | 기준보다 10.0~27.9% 낮은 Ir/msg라 보호 규칙상 FAIL; 기능 실패가 아니며 아래의 단독 LTO 측정으로 대체 |
| 변경 suite `stream|pipe|wake|hwm|flow|credit|poll|completion|router|dealer|pair|sub`, `until-fail:3` | **98/98 PASS**, 341.55초 |
| `test_stream_concurrent_pull_send`, `until-fail:20` | **20/20 PASS**, 3.92초 |
| `JOBS=4 scripts/build-core.sh release --lib-only` | PASS |
| `cmake --build core/build-gate --target hotpath_bench -j4` | PASS |

기능 CTest 실패는 없으므로 3회 재실행 대상도 없었다. dev 전체 명령에는 등록된 측정 테스트가 포함됐지만, 지시의 “hotpath_gate 제외 정상”에 해당한다.

## 공개 인터페이스

- staged/unstaged `core/include/**`, `core/src/libzlink.vers` diff는 모두 비어 있다.
- `zlink.h`, `zlink_enum.h`, `zlink_errno.h`를 Core와 C/C++/Go/Rust binding mirror로 `cmp`: **12/12 PASS**.
- `git diff --cached --check`: PASS.

## hotpath 5셀

`PERF_LOCK` 아래, 시작 load `0.47/2.31/3.90`, `ninja=0`, available 10628 MiB에서 1회 실행했다.

| cell | reference Ir/msg | 측정 Ir/msg | ratio | 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3230.922 | 3327.906 | 1.0300 | PASS |
| dealer_router_reqrep_inproc | 16455.383 | 16550.426 | 1.0058 | PASS |
| pair_inproc | 2348.457 | 2378.295 | 1.0127 | PASS |
| router_router_tcp | 2972.532 | 2988.456 | 1.0054 | PASS |
| stream_tcp | 13969.806 | 14186.494 | 1.0155 | PASS |

## 성능 관측

모든 실행은 local Release `libzlink.so.0.17.2`, `PERF_LOCK`, `ninja=0`, available ≥6000 MiB 조건에서 수행했다. runs=1 값은 계획 §7.4의 0.17.2 idle runs=3와 직접 판정용 비교가 아니라 관측치다.

### with_stream

시작 load `0.50/2.09/3.76`, 결과 디렉터리 `bindings/c/bench/with_stream/results/20260908_085903/`, mismatch 0.

| size | zlink kops/s | 0.17.2 idle | ratio | asio kops/s | zmq kops/s |
|---:|---:|---:|---:|---:|---:|
| 64 B | 285.52 | 290.7 | 0.9822 | 378.39 | 332.49 |
| 1024 B | 274.78 | 272.9 | 1.0069 | 342.19 | 304.11 |
| 65536 B | 34.43 | 34.7 | 0.9922 | 42.36 | 23.16 |

### perf/c multi, tcp 1024 B

시작 load `0.91/3.69/4.10`, 결과 `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_091013.txt`.

| cell | kops/s | §7.4 0.17.2 idle | ratio |
|---|---:|---:|---:|
| DEALER_ROUTER_REQREP | 233.090 | 170.8 | 1.3647 |
| ROUTER_ROUTER_SENDSEND | 250.754 | 181.2 | 1.3839 |
| PUBSUB | 1039.496 | 848.8 | 1.2247 |

## 제출 patch의 분류 메모

- 소유 계층: Core socket receive 상태와 socket 쪽 pipe 끝.
- 참조 spec: `core/doc/spec/core/systems/11-synchronization-model.ko.md` §3.1/§3.3/§3.4/§4/§5/§6 (ST-3 보고서의 대조 범위).
- 교차언어: Framework runtime 변경은 없으며 C/C++ Core 경로의 수정이다.
- 변경 분류: 제출 보고서 기준 **B(기존 결함 수정)**. 이 게이트는 코드 재판단·수정을 하지 않았다.
- 규칙 수(제출 보고서 기준): progress 발행 구현 2→1로 통합; receive 배타 형식은 2개로 유지했다.
