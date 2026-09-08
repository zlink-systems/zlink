# core-rf-SD-3 결과 보고서

## 1. 결론

D-B291 판정에 따라 SD-2 최종 patch에서 B1을 제거하고 계측 app 수정, B2, D-f와 WS gate 수정만 남겼다. B1이 추가한 TCP readiness rearm과 TCP speculative read 금지 규칙은 모두 사라졌으며, TCP STREAM read는 다시 기존 `async_read_some()`과 bounded speculative drain을 사용한다. B2의 decoder/encoder 1-hit 2배 성장과 async/speculative read가 공유하는 `last_read_bytes` 단일 상태는 유지했다.

최종 구성은 dev 211/211, 관련 27-suite 3회, suppression 없는 TSan 27/27를 통과했다. 축소 strace는 성공 read 1.01072/msg, EAGAIN 1.00062/msg로 B2-only 특성을 재현했다. idle WS gate 1회는 ws 2.099380, wss 1.300809로 모두 0.80 이상이다.

- 누적 patch: `/home/hep7hep7/project/zlink-work/all-artifacts/SD-3.patch`
- base: `84d25131a6424031149ab7321e0678b65409bd75`
- report untracked 파일은 patch에 포함했고 사용자 소유 `SUPERVISOR-NOTE.md`와 hotpath reference는 제외했다. 최종 patch는 reverse apply check를 통과했다.

## 2. B1에서 되돌린 hunk

| 파일:행 | 되돌린 내용 |
|---|---|
| `core/src/runtime/engine/asio/asio_engine.cpp:442-519` | 성공한 TCP STREAM read 뒤 `async_wait(wait_read)`를 고르던 rearm 분기와 readiness 전용 handler 구성을 제거하고 기존 `async_read_some()` 호출로 복원했다. |
| `core/src/runtime/engine/asio/asio_engine.cpp:983-1027` | TCP speculative read를 막던 `should_speculatively_read_stream()` helper를 제거했다. 기존 capability와 full-read evidence를 직접 쓰는 bounded drain은 유지하되 read byte 상태만 `last_read_bytes` 하나를 쓴다. |
| `core/src/runtime/engine/asio/asio_engine.cpp:1676-1682` | restart 시 B1의 공통 drain 진입을 제거하고 기존 1회 speculative read 뒤 async rearm 흐름으로 복원했다. |
| `core/src/runtime/engine/asio/asio_engine.hpp:225-231` | B1 predicate 선언을 제거했다. `maybe_drain_stream_reads()`는 B2의 단일 read 상태를 읽으므로 인자 없는 형태만 남겼다. |
| `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:147-155` | TCP speculative 금지 predicate를 제거하고 transport capability 또는 기존 STREAM 진단 opt-in을 따르는 SD-1 정책으로 복원했다. B1의 `wait_for_readiness_before_rearm()`도 제거했다. |
| `core/src/runtime/engine/asio/i_asio_transport.hpp:79` | B1이 추가한 `async_read_some_when_ready()` virtual 전체를 제거해 파일을 base와 동일하게 복원했다. |
| `core/src/runtime/transports/tcp/tcp_transport.cpp:84,421` | B1의 공통 async helper, `async_wait(wait_read)` callback과 즉시 `read_some()` 구현을 제거해 파일을 base와 동일하게 복원했다. |
| `core/src/runtime/transports/tcp/tcp_transport.hpp:37` | readiness read override 선언을 제거해 파일을 base와 동일하게 복원했다. |
| `core/tests/unittest/unittest_asio_write_turn_policy.cpp:185-198,280-302` | readiness rearm unit과 TCP speculative 금지 expectation을 제거하고 TCP/IPC capability expectation을 복원했다. B2 1-hit 성장과 WS gather 검증은 남겼다. |

`async_read_some_when_ready`, `wait_for_readiness_before_rearm`, `should_speculatively_read_stream`과 TCP transport의 `wait_read` 잔존 참조는 0개다. `core/tests/perf/hotpath_reference.json`도 base와 동일하다.

## 3. 남은 변경 파일

| 파일:행 | 남은 변경 |
|---|---|
| `bindings/c/bench/with_stream/stacks/zlink/test_scenario_stream_zlink.cpp:87-103,208-265` | chunk-local 다중 frame 우회 파서를 제거하고 exact 1-frame zero-copy 또는 RID별 누적 조립만 남겼다. |
| `core/src/runtime/engine/asio/asio_engine.cpp:144-152` | connection 생성 때 protocol header, gather capability와 transport message-boundary를 한 번에 policy로 넘긴다. |
| `core/src/runtime/engine/asio/asio_engine.cpp:445-448,521-582,805-846,899,978-1027` | `last_read_bytes`를 async/speculative read가 공유하고 decoder/encoder target의 1-hit 성장을 적용한다. 기존 bounded drain과 async rearm 규칙은 유지한다. |
| `core/src/runtime/engine/asio/asio_engine.hpp:229-231` | 단일 pipeline 상태를 읽는 bounded drain 선언을 유지한다. |
| `core/src/runtime/engine/asio/asio_engine_pipeline.hpp:25-77` | partial-prefix와 decoder/encoder hit counter를 제거하고 read byte 상태를 `last_read_bytes` 하나로 합쳤다. |
| `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:24-31` | 동작하지 않던 STREAM gather env accessor 3개를 제거했다. |
| `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:136-195` | `protocol header && gather 지원 && (message-boundary || 기존 ZMP gather env)`라는 한 gather 선택 규칙을 connection snapshot에 둔다. RAW STREAM과 TCP ZMP 기본값은 변하지 않는다. |
| `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:337-404` | decoder/encoder가 target을 한 번 채우면 2배로 성장시키고 기존 max로 clamp한다. message-boundary encoder의 기존 축소 규칙은 유지한다. |
| `core/tests/unittest/unittest_asio_write_turn_policy.cpp:34-38,98-111,169-198,280-340,366-383` | B2 1-hit 성장, 기존 TCP/IPC speculative capability, env opt-in TCP gather, message-boundary ZMP 자동 gather와 RAW 비활성을 검증한다. |

## 4. 설계 비교와 선택

| 대안 | 규칙 | 판정 |
|---|---|---|
| SD-2 최종 patch에서 B1 hunk만 역편집 | B1과 B2가 섞인 pipeline 상태와 unit expectation을 매번 분리해야 한다. | 제외 |
| `SD-2-ablation-B2only.patch`에서 시작해 `last_read_bytes` 단일 상태와 WS hunk만 적용 | B1 transport/interface 파일이 처음부터 없고, B2와 WS의 소유 경계만 남는다. | 선택 |

B2-only patch가 보존한 별도 speculative byte 이름을 async/speculative 공통 `last_read_bytes`로 합쳐 상태 소유자를 하나만 남겼다. B1의 별도 readiness API, callback, predicate와 persistent state는 없다.

규칙 수는 SD-2 최종 구성의 7개에서 B1 readiness 규칙을 제거해 8개가 됐지만, SD-1의 13개보다는 5개 적다. B2는 두 hit counter와 partial-prefix 상태를 제거했고, WS는 opt-in과 message-boundary를 같은 gather predicate로 합쳐 새 실행 경로를 추가하지 않았다.

## 5. 검증

| 검증 | 결과 |
|---|---|
| `JOBS=4 scripts/build-core.sh dev` | 성공. 시작 ninja 0, available 10,619 MiB. |
| `unittest_asio_write_turn_policy` | 1/1 성공. |
| `ctest -R 'stream\|asio\|decoder\|ws'` 3회 | 27/27 × 3 성공, 15.05/14.83/14.64 s. |
| `ctest -E hotpath_gate` | 211/211 성공, 244.44 s. |
| 기존 GCC `core/build-tsan` 증분 build | 성공. `-fsanitize=thread`, PIE 구성을 확인했다. |
| `TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R ctest -R 'stream\|asio\|decoder\|ws'` | 27/27 성공, 22.80 s. suppression 미지정, TSan 보고 0건. |
| `git diff --check` | 성공. |

## 6. strace 축소셀

조건은 dev library, TCP, 64 KiB, CCU 20, client/server I/O thread 1, warmup 3 s, duration 5 s다. 시작 조건은 2026-09-08 23:44:11 KST, load1 0.22, ninja 0, available 10,646 MiB였고 `PERF_LOCK` 아래 한 번 실행했다.

| 항목 | 값 | message당 |
|---|---:|---:|
| server message | 11,380 | 1.00000 |
| `recvfrom` 전체 | 22,889 | 2.01134 |
| `recvfrom` error(EAGAIN) | 11,387 | 1.00062 |
| 성공 read | 11,502 | 1.01072 |

client는 2.272 kops, p50 4.391 ms, p99 6.374 ms였고 size mismatch와 server parse/protocol/send error는 0이다. 원자료는 `all-artifacts/SD-3-strace.summary`, `SD-3-strace-{client,server}.log`다.

## 7. WS/WSS gate 1회

현재 source로 Release library를 갱신한 뒤 2026-09-08 23:47:48 KST, load1 0.74, ninja 0, available 10,691 MiB에서 `/tmp/claude-1000/PERF_LOCK`을 잡고 실행했다. 조건은 100 clients, I/O thread 4, 1/64 KiB, TCP/WS/WSS, DEALER-DEALER 단방향과 DEALER-ROUTER 왕복 각 1회다. 12/12 cell이 완료됐고 skip/fail/unsupported는 0이다.

| transport | Q(1 KiB) | Q(64 KiB) | Q64/Q1 | 기준 | 판정 |
|---|---:|---:|---:|---:|---|
| ws | 0.521565 | 1.094963 | 2.099380 | ≥ 0.80 | PASS |
| wss | 0.589966 | 0.767434 | 1.300809 | ≥ 0.80 | PASS |

원자료는 `all-artifacts/SD-3-ws-gate/multi/report/perf_c_multi_linux_20260908_234748_SD-3.txt`다. load1 1.58이었던 사전 시도는 첫 TCP 셀 중 즉시 중단했고 `SD-3-ws-gate-invalid-load`로 분리해 판정에서 제외했다.

## 8. 소유 계층, 스펙과 분류

- 소유 계층: target 성장과 bounded drain은 Core Asio engine/policy가, WebSocket write 단위는 ZMP encoder와 message-boundary transport capability가 소유한다. benchmark app에는 wire parsing 보상 대신 계측용 RID별 조립만 있다.
- 스펙 조항: `core/doc/spec/core/systems/03-io-thread.ko.md` §3.2의 Proactor completion과 async rearm을 B1 제거로 다시 그대로 따른다. `core/doc/spec/core/socket/08-stream.ko.md` §5·§6.3·§10의 RAW STREAM part, bounded queue와 raw WS 경계를 바꾸지 않았다. `core/doc/spec/core/protocol/01-zmp.ko.md` §8·§9의 연속 ZMP byte 열과 준비된 byte만 bounded batch로 한 번 쓰는 WebSocket encoder 규칙을 유지한다. 어느 문장도 다른 동작이 되지 않았다.
- 교차언어 대조: C/C++/.NET/Java/Kotlin binding은 모두 같은 native Core engine/transport를 사용하므로 B2와 WS 수정은 언어별 상태나 우회 없이 공통 적용된다. app 수정은 측정 대상인 C benchmark에만 있다.
- 변경 분류: app 수정=B(기존 benchmark 결함), B2=B(기존 target 성장 결함), D-f=A(무효 compatibility 접근 제거), WS gate 수정=B(기존 message-boundary write 결함). B1은 D-B291 판정에 따라 제외했다.
- 공개면: `core/include/**`, `core/src/libzlink.vers`, public ABI/API와 정식 spec 문서는 변경하지 않았다.

보호된 spec `core/doc/spec/core/socket/08-stream.ko.md:405-407`에는 제거된 D-f env 3개를 “호환을 위해 읽는다”고 적은 문장이 남아 있다. 이번 patch에는 포함하지 않았으며 감독자가 승인된 문서 변경으로 함께 삭제해야 한다.
