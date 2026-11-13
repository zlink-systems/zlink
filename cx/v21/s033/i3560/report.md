# #3560 — Core 1.24 wake-up 재측정

## 연결된 Core

Framework worktree는 `76a4d7850664c2cb32e8d35837dac45d9d2a4057`이다. Release bench는 private binding source build를 사용했고, Core prefix `.i3560-package/install/zlink-core/1.23.0`은 `/root/jobs/fx/cx/v21/s033/core124/prefix-1.24`를 가리킨다. 실행 중 `/proc/<pid>/maps`와 `ldd`가 이 prefix의 `libzlink.so.1.23.0`을 확인했다. provenance에는 package version/SONAME이 `1.23.0`으로 남아 있지만, source revision은 Core 1.24 recipe candidate `0005a4d6c07160bbf661a6f7aea3d8093880514e`이며 `dirty=false`다. `e851f37900`은 이 source의 조상이다 (`merge-base --is-ancestor` exit 0). Runtime SHA-256은 `2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64`다. 근거: `/root/jobs/fx/cx/v21/s033/core124/prefix-1.24/share/zlink/core-package-provenance.json:4–16`, `identity-check.txt`, `bench-configure-slot.log`, `bench-build.log`, `raw-condB/zlink-framework-cpp-run-{02,04}/core-maps-source.txt`.

## 측정 — condition B, request-backpressure 1024 B, 2 rounds

source는 CPU 0–7, target은 8–15에 고정했다. 각 round는 5초 warmup, 8초 active였다. C++ Release bench는 현재 Framework와 binding source로 빌드했고 `ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON`을 설정했다. Source/target의 모든 Core I/O TID를 매 run의 before/after TID union으로 더했다. 아래 값은 완료 request당 CPU µs이며, 같은 `IO/n` 이름의 중복 TID도 합산했다.

| 구현 | 역할 | IO/0 | IO/1 | IO/2 | IO/3 |
|---|---|---:|---:|---:|---:|
| binding | source | 5.063 | 0.0017 | 3.463 | 0.0017 |
| binding | target | 4.593 | 4.866 | 0.0016 | 0.0017 |
| Framework | source | 20.508 | 0.0067 | 17.486 | 0.0065 |
| Framework | target | 18.776 | 20.795 | 0.0036 | 0.0035 |

TID별 원자료: `raw-condB/zlink-{cpp,framework-cpp}-run-{01,02,03,04}/{source,target}-io-cpu.json`; 집계와 TID union: `measurement-condB-summary.json`, `capture-summary.json`.

**(a) Command executor 비용: 사라짐.** 두 Framework round의 source `IO/1` 합산은 **0.0067 µs/request**였고, 각 active 2초 BPF 창에서 `process_async_mailbox` 진입과 그 안의 command가 0회였다 (`@async`, `@executor_command` 맵 없음). 따라서 1.21에서 관찰한 별도 executor의 10.8 µs/request는 현재 Core steady state에 없다. Core는 completion poller registration을 async-command lease로 보지 않는다: `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:968–979`; executor 처리 진입/루프: `:360,1705–1724`. 원자료: `raw-condB/zlink-framework-cpp-run-{02,04}/wake-counts.txt`.

**(b) Signaler self-wake: 남아 있음.** Source 프로세스 기준 Core `signaler_t::send` **1.462/request**, `recv_failable` 안의 read **0.936/request**, send 안의 write **0.446/request**(read+write **1.381/request**)였다. Binding은 각각 **0.075**, **0.031**, **0.016**(합 **0.047/request**)였다. BPF는 active 중 2초를 계수하고 성공한 `zlink_request` 반환 수로 나눴다. 원자료: `raw-condB/zlink-{cpp,framework-cpp}-run-{01,02,03,04}/wake-counts.txt`, probe: 같은 디렉터리의 `wake.bt`.

현재 Core 경로는 reply publication `core/src/api/socket/socket_request_reply_internal.cpp:455–466` → coalesced `request_completion` command `core/src/runtime/sockets/common/socket_base_dispatch.cpp:325–338` → command batch readiness `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:559–562` → 등록 poller signaler `core/src/runtime/core/mailbox.cpp:472–487`이다. 해당 신호의 read/write 구현은 `core/src/runtime/core/signaler.cpp:200–280,424–505`에 있다. 이 제어·notification 동작의 수정 소유자는 **Core**다. Counts는 프로세스의 전체 Core signaler activity이며 각 호출 경로 단독 계수는 아니다.

## 검증 및 산출물

두 condition-B round 모두 binding/Framework에서 errors, abandoned, validation failures, rejected, contamination, drain-bound hit가 0/false였다. 결과 파일: `raw-condB/run{1,2}/zlink-{cpp,framework-cpp}-request-backpressure-1024/results.json`; pinning 원자료: `raw-condB/pinning.log`; runner: `raw-condB/runner.log`. 빌드 완료는 `bench-build.log`의 `[114/114]`다. 코드 변경은 없고 Core/Framework bench build 디렉터리를 제거했다.
