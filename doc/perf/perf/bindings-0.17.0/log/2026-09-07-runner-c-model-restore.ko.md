# perf 러너 C 기준 모델 복원 기록

- 일자: 2026-09-07
- branch: `main`
- Core: `release`, `/home/hep7/.cache/zlink/core-pinned/0.17.1`
- 목적: request/reply 앱 고정 window 제거, C turn 구조 복원, 역할별 poller 등록 정합
- 성능 수치는 판정 근거로 사용하지 않았다.

## 결과 요약

Go REQREP 경로를 제외한 변경 허용 범위에서 앱의 고정 outstanding 상한과 admission 재시도 규칙을 제거했다. C++·Java multi REQREP은 socket마다 turn당 한 건만 제출하고 같은 turn에서 완료를 drain하며, reply 완료 여부로 다음 제출을 막지 않는다. requester poller는 `POLLCOMPLETION`만 등록하고 모든 실제 runner poller 등록에서 `POLLOUT`을 제거했다.

수정 전/후 규칙 수: 앱이 `상한 + per-socket gate + backpressure 재시도`의 세 규칙을 소유하던 구조에서, `한 turn에 제출하고 완료만 회수`하는 한 규칙으로 줄였다. Go REQREP은 아래 충돌 때문에 예외로 남았다.

## 러너별 제거·복원

- C: `bindings/c/perf/run_comparison.py:4047`, `bindings/c/perf/single/run_comparison.py:1048`에서 존재하지 않는 REQREP 상한의 Effective Options 노출을 삭제했다. raw API의 `retained_payload`/`wait_token`은 변경하지 않았다.
- C++: `bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp:231,312-381`에서 cap, per-socket outstanding/gate, 앱 재시도를 제거하고 turn당 socket 한 건 제출 뒤 completion drain으로 복원했다. `:312-318`의 단일 공개 completion poller와 owner 이전은 유지했다. single은 `bindings/cpp/perf/single/common/perf_single_reqrep.hpp:393-469`에서 같은 상한/gate를 제거했다. 옵션 노출은 `bindings/cpp/perf/run_comparison.py:4049`, `bindings/cpp/perf/single/run_comparison.py:1011`에서 삭제했다.
- .NET: multi `bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/src/PerfMultiSocketReqRep.cs:235-403`, single `bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfReqRep.cs:402-523`에서 상한, slot outstanding, reply gate와 backpressure catch를 제거했다. multi requester도 하나의 completion-only poller에 모았다. 옵션 노출은 `bindings/dotnet/perf/multi/run_benchmarks.sh:1696`, `bindings/dotnet/perf/single/run_emit.py:853`에서 삭제했다.
- Java: multi `bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiSocketReqRep.java:214-281`, single `bindings/java/perf/single/Zlink.BindingBench/src/main/java/systems/zlink/perf/single/PerfSocketReqRep.java:182-237`에서 cap/gate/retry를 제거하고 completion-only poller로 페이싱했다. `bindings/java/perf/single/Zlink.BindingBench/src/main/java/systems/zlink/perf/single/PerfPair.java:129-134`의 stop 전송용 `POLLOUT` poller도 제거했다. 옵션 노출은 single `run_benchmarks.sh:702-724`, multi `run_benchmarks.sh:1779-1789`에서 삭제했다.
- Node: multi `bindings/node/perf/multi/perf_multi_socket_reqrep.ts:66-165`, single `bindings/node/perf/single/perf_socket_reqrep.ts:120-202`에서 cap/gate를 제거하고 completion-only poller를 추가했다. `wait(0)`은 turn당 한 번만 호출하고 `setImmediate`에 양보한다. routed reply helper의 자체 backpressure 재제출은 `bindings/node/perf/multi/perf_multi_runtime.ts:286-329`에서 제거했고, PUB stop용 `POLLOUT` poller는 `bindings/node/perf/multi/perf_multi_pubsub_server.ts:61-79`에서 제거했다. Effective Options는 `bindings/node/perf/common/perf_c_emitter.ts:310,344`에서 삭제했으며 `dist-tools`를 재생성했다.
- Rust: multi `bindings/rust/perf/multi/src/perf_multi_socket_reqrep.rs:247-329`, single `bindings/rust/perf/single/src/common.rs:753-833`에서 cap/gate를 제거했다. single sender/requester 등록은 `bindings/rust/perf/single/src/common.rs:159-164,770-774`에서 `POLLCOMPLETION` 단독으로 고쳤다. stop token은 `perf_dealer_router_reqrep.rs:93-98`, `perf_router_router_reqrep.rs:111-116`에서 앱 retry 없이 한 번 제출한다.
- Python: multi `bindings/python/perf/multi/perf_multi_reqrep_client.py:164-240`, single `bindings/python/perf/single/perf_single_reqrep.py:149-186`에서 cap/gate/retry를 제거했다. event loop는 completion poller `wait(0)` 뒤 `asyncio.sleep(0)`으로 양보한다. 옵션 노출은 `bindings/python/perf/perf_report.py:350,564`, single `run_benchmarks.py:317`, multi `run_benchmarks.py:1317`에서 삭제했다.
- Go: `bindings/go/perf/run_benchmarks.sh:581`, `bindings/go/perf/run_benchmarks_multi.sh:999`에서 Effective Options 노출만 제거했다. 금지된 REQREP 구현은 수정하지 않았다. 허용된 one-way SENDSEND는 `bindings/go/perf/internal/perfcommon/measurement.go:48-61`, `perf/multi/perf_multi_dealer_router.go:113-182`, `perf/multi/perf_multi_router_router.go:117-188,270-330`에서 managed send를 한 번 제출하고 deadline cancellation/inline reply로 완료하게 했다. 요청마다 reply goroutine을 만들던 앱 측 미완료 관리와 transient 재제출을 제거했다.

## C 대조표

| 언어 | turn 구조 | requester poller | backpressure 처리 주체 | 미완료 관리 |
|---|---|---|---|---|
| C | socket당 한 건 제출, 같은 turn drain | `POLLCOMPLETION` | raw C slot의 `retained_payload`/`wait_token` | 완료 drain용 slot 상태만 유지 |
| C++ | C와 동일 | `POLLCOMPLETION` 단독 | binding awaitable | 전체 drain 수명만 유지, gate 없음 |
| .NET | C와 동일 | `PollCompletion` 단독 | binding task | task 회수만 수행, cap/gate 없음 |
| Java | C와 동일 | `POLLCOMPLETION` 단독 | binding future | future 회수만 수행, cap/gate 없음 |
| Node | C와 동일 | `PollCompletion` 단독 | binding Promise | Set은 완료 회수/drain 전용, cap/gate 없음 |
| Rust | C와 동일 | `POLLCOMPLETION` 단독 | binding Future | task collection은 완료 회수/drain 전용 |
| Python | C와 동일 | `POLLCOMPLETION` 단독 | binding awaitable | set은 완료 회수/drain 전용 |
| Go | one-way는 managed submit 단독 소유로 정리 | one-way receiver는 `POLLIN`; REQREP은 수정 금지 | binding managed send | one-way reply goroutine fan-out 제거; REQREP cap은 금지 충돌로 잔존 |

## poller 등록과 스핀 확인

- requester: C/C++/.NET/Java/Node/Rust/Python은 `POLLCOMPLETION` 단독이다. C++ completion-owner 이전을 유지했다.
- replier 및 one-way receiver: 실제 등록은 `POLLIN`이다.
- 실제 `POLLOUT` 등록: Java PAIR, Node multi PUB, Rust single 공통 sender에서 제거했다. 전수 검색 결과 non-C runner의 실제 등록에는 남지 않았다.
- C/C++/.NET/Java/Rust는 진전이 없으면 timeout이 있는 completion wait에서 블로킹한다.
- Node/Python은 turn당 `wait(0)` 한 번 뒤 event loop에 yield한다.
- Go one-way sender는 binding managed submit에서 대기하고 receiver는 timeout이 있는 `POLLIN` wait를 사용한다. SENDSEND server는 reply를 inline submit하므로 같은 socket의 send mutex에 대기하는 무제한 goroutine이 없다.

## smoke 결과

모든 실행 전에 `scripts/perf/wait-for-idle-perf.sh`를 호출했고 한 번에 한 suite만 실행했다. 조건은 `PERF_FAIL_FAST=1`, `--duration 1`, `--runs 1`, `tcp`, 64 B/65536 B다.

| 언어 | single | multi | 보충 확인 |
|---|---|---|---|
| C | complete 14/14 (`perf_c_single_linux_20260907_211221.txt`) | complete 14/14 (`perf_c_multi_linux_20260907_211317.txt`) | - |
| C++ | complete 14/14 (`perf_cpp_single_linux_20260907_211359.txt`) | partial 12/14: STREAM client-ready 2건 | REQREP 4/4 complete (`perf_cpp_multi_linux_20260907_212705.txt`), 마지막 single REQREP 4/4 complete (`...214444.txt`) |
| .NET | complete 14/14 (`perf_dotnet_single_linux_20260907_211610.txt`) | partial: STREAM 64 B 실패 후 fail-fast | 마지막 multi REQREP 4/4 complete (`perf_dotnet_multi_linux_20260907_213201.txt`) |
| Java | complete 14/14 (`perf_java_single_linux_20260907_211737.txt`) | complete 14/14 (`perf_java_multi_linux_20260907_211809.txt`) | - |
| Node | complete 14/14 (`perf_node_single_linux_20260907_211938.txt`) | complete 14/14 (`perf_node_multi_linux_20260907_212041.txt`) | 마지막 single REQREP 4/4 complete (`...214754.txt`), multi REQREP 4/4 complete (`...214046.txt`) |
| Rust | complete 14/14 (`perf_rust_single_linux_20260907_212048.txt`) | complete 14/14 (`perf_rust_multi_linux_20260907_212252.txt`) | 마지막 single REQREP 4/4 complete (`...215004.txt`) |
| Python | complete 14/14 (`perf_python_single_linux_20260907_212425.txt`) | complete 14/14 (`perf_python_multi_linux_20260907_212529.txt`) | 마지막 single REQREP 4/4 complete (`...214150.txt`) |
| Go | 허용된 one-way 10/10 complete (`perf_go_single_linux_20260907_215924.txt`) | 허용된 non-REQREP 10/10 complete (`perf_go_multi_linux_20260907_215946.txt`) | REQREP은 명시적 금지로 실행/수정 대상에서 제외 |

## 빌드·단위·contract 검증

- C: runner policy Python test 61 passed. 관련 CTest 9개 중 8 passed, `test_c_contract_surface` 1 failed. 고정 package의 runtime patch version과 repository header version 비교가 일치하지 않는다.
- C++: 대상 perf binary build 성공, 관련 contract/application-ready test 5 passed.
- .NET: single/multi build 성공, test 232 passed.
- Java: `:perf-single:test :perf-multi:test` 성공(single은 NO-SOURCE, multi test passed).
- Node: incremental build/typecheck 성공, 관련 test 19 passed.
- Rust: single/multi `cargo check/test` 성공, `cargo fmt --check` 성공. 최종 single test는 14 passed.
- Python: `compileall` 성공. 환경에 `pytest` module이 없어 pytest는 실행하지 못했다.
- Go: `go test ./perf/...` passed. 전체 `go test ./...`는 기존 boundary의 `perf/internal/perfcommon/monotonic.go:C` 검출과 direct header version mismatch 두 건이 실패했다.
- 수정한 shell script의 `bash -n`, 전체 `git diff --check`가 통과했다.

## 감독자 판단 필요

1. 요구 사항의 “상한 전부 제거”와 “Go REQREP 경로 수정 금지”가 충돌한다. 금지를 우선해 `bindings/go/perf/single/perf_reqrep.go:39-50`, `bindings/go/perf/multi/perf_multi_socket_reqrep.go:210-322`의 상한/미완료 관리는 남겼다. 이 두 파일을 고치려면 명시적 범위 해제가 필요하다.
2. C++ multi와 .NET multi 전체 smoke의 실패는 모두 REQREP가 아닌 STREAM 준비/종료 경계다. 이번 변경의 REQREP 집중 smoke는 complete이며, 원인 변화 없이 전체 gate를 반복하거나 STREAM을 범위 외 수정하지 않았다.
3. 고정 0.17.1 package를 사용한 C/Go direct-header contract는 repository header와 runtime patch version이 달라 실패한다. Core/package/header를 이번 작업에서 수정하지 않았다.
4. Python pytest를 실행하려면 테스트 환경에 pytest 설치가 필요하다.
