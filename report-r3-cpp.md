# C++ 3단계 최종 보고

브랜치: `framework-node/1083-a3-node-wire`. 스펙·commit·push·branch는 변경하지 않았다. 모든 빌드·시험·format은 WSL 사본에서 수행했다. local package `/home/hep7/.cache/zlink/pkg-1.12`를 읽기 전용으로 사용했다.

## 발견·수정·소유

아래 C++ 경로는 `framework/languages/cpp/` 기준이다. 원인 위치는 수정 전, 수정 위치는 최종 소스 기준이다.

| 발견 | 원인 file:line | 수정 file:line | 소유·스펙·분류 |
|---|---|---|---|
| G01 미작성 request 전송 | `connector/core/src/runtime/calls/zlink_stream_calls.cpp:651,1045,1550` terminal과 queue 삭제 분리 | 같은 파일 `:652–684`, 실패 terminal만 write_id로 삭제, active 보존 | connector terminal/queue, stream §5.2:320, B |
| R06 ASCII 공백 이름 | `protocol/header_codec.cpp:136`, `framework/src/runtime/streams/stream_runtime.cpp:1065` empty만 검사 | `common/include/zlink/detail/stream_packet_name.hpp:18` 단일 구조 validator, header `:138`/server `:1068`/named wait `calls.cpp:1727` 위임 | wire 이름 구조, §4.2:120, B |
| R06 typed/raw empty 우회 | 내부 packet_name_resolver compiler-name fallback, public raw send/request의 기본 이름 literal 2곳 | `contracts/zlink_stream_connector.hpp:276–281` public resolver 값 그대로 사용, 내부 resolver cpp/hpp·CMake/test 사본 제거; raw fallback 제거 | 이름 소유, §4.2:120, B |
| 예약 prefix 불일치 | `framework/src/runtime/streams/stream_runtime.cpp:1071`의 __zlink. | 공통 이름 header `:10`의 $zlink. 상수 재사용 | wire 구조, §4.6, B |
| R12 close 실패 폐기 | native TCP/TLS/WS sync/async와 late/raw connect close 오류 폐기 | `connector_runtime.cpp:498` publish_close_error 단일 owner. accepted `calls.cpp:1388`, late `:1232`, raw `:1282`, explicit `:1569` 위임. TLS/WS/WSS raw cancel도 동일 callback 사용 | connector lifecycle, §9:675, B; 내부 interface 계약 적응 A |
| assert 일반 예외 변환 | `contracts/zlink_stream_assert.hpp:54–88` action 예외 변환 | 같은 파일 `:51` action 예외 그대로 전파, returned failure/timeout 판정 유지 | 사용자 감독자 판정, B |
| HTTP application 분류·재실행 | request_performer 전역 transport 분류 및 reused catch-all | `http-client/src/runtime/request_performer.cpp:35` callback owner, coded 예외 `:40` 재전파, provider `:339`/sink `:396` 위임. stale retry `:266` 실제 transport 예외로 한정 | HTTP performer application 단계, 09-error-model:15, B |

## 네 언어 정적 대조

경로는 `framework/languages/` 기준이다. 아래 Node lifecycle·.NET HTTP mapper 위치는 이 C++ 진단 시점의 조회 위치이다. 해당 담당자의 후속 수정에 따른 최종 위치와 타 언어 검증 결과는 최종 root 보고를 참조한다.

| 결정 | Node | Java | .NET | C++ |
|---|---|---|---|---|
| request 취소/queue | `node/packages/stream-connector/src/Runtime/ZlinkStreamFrameSender.ts:40,212` | `java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamPendingRequests.java:34,74` | `dotnet/src/Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamFrameSender.cs:98` 취소 gate | `cpp/connector/core/src/runtime/calls/zlink_stream_calls.cpp:652` |
| 이름 구조 | `node/packages/stream-wire/src/index.ts:671` trim | `java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamWireProtocol.java:315` validatePacketName 최종 connector owner | `dotnet/src/Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamConnector.cs:633` 진단 시점 validator, 최종 root 보고 참조 | `cpp/common/include/zlink/detail/stream_packet_name.hpp:18` |
| close 오류 event | `node/packages/stream-connector/src/Runtime/ZlinkStreamConnectorLifecycle.ts:528` 진단 시점, 최종 root 보고 참조 | `java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamConnectionLifecycle.java:802` | `dotnet/src/Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamConnectorLifecycle.cs:651` 진단 시점, 최종 root 보고 참조 | `cpp/connector/core/src/runtime/connector_runtime.cpp:498` |
| assert action | `node/packages/stream-connector/src/Runtime/ZlinkStreamAssertions.ts:30` | `java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamAssert.java:20` | `dotnet/src/Systems.Zlink.Stream.Connector/Contracts/ZlinkStreamAssert.cs:24` | `cpp/connector/core/include/zlink/stream_connector/contracts/zlink_stream_assert.hpp:51` |
| HTTP application/coded | `node/packages/http-client/src/runtime/request-performer.ts:125,207`, `retry-policy.ts:70` coded 보존 | `java/zlink-http-client/src/main/java/systems/zlink/httpclient/internal/HttpClientErrors.java:57,59` | `dotnet/src/Zlink.HttpClient/Runtime/RetryPolicy.cs:57` 진단 시점 coded 보존/performer 단계 전달, 최종 mapper 위치는 root 보고 참조 | `cpp/http-client/src/runtime/request_performer.cpp:35,40` |
| HTTP one-shot | `node/packages/http-client/src/request-builder.ts:94,183` | `java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:262` | `dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:184,208` | `cpp/http-client/src/client.cpp:351–383` eager 값 소유 |

## 판정 위치 수·성능

| 판정 범위 | 전 | 후 |
|---|---:|---:|
| terminal/미작성 request 제거 소유 | 2 | 1 |
| wire 이름 구조 validator | 2 | 1 |
| typed 이름 최종 결정 | 2 | 1 |
| raw empty 기본 이름 발명 | 2 | 0 |
| accepted close 시작(heartbeat/read/write) | 3 | 1 |
| native close 오류 정책(TCP/TLS/WS sync/async) | 6 | 1 |
| assert 일반 action 예외 변환 | 3 | 0 |
| HTTP provider/sink 실행 오류 분류 | 2 | 1 |
| download chunk 상수의 제어 판정 | 0 | 0 |

persistent 상태·flag·timer·retry·option·map·lock 추가 0. 기존 중복 pending_request.request_seq를 write_id로 전환하여 필드 수가 같다. 기존 request/write 카운터를 유지했다. 성공 terminal은 queue scan/post를 수행하지 않으며 실패 terminal만 기존 deque를 검색한다. 추가 index/할당이 없다. trace off 비용 추가가 없다. accepted close 시작을 connection_ended로 모았으며 explicit close 오류 event는 finalized closed 상태 이후 전달한다.

## 재현과 최종 검증

로그 위치는 `.artifacts/node-wire-r3/cpp/`이다. baseline source/header/library snapshot을 유지한 뒤 after source를 동기화했다.

| 범위 | 수정 전 로그·실패 | 수정 후 로그·결과 |
|---|---|---|
| G01/R06/assert/R12 | `before-connector.log:1,3,6,8,9` FAIL | `after-immediate-close.log` 모두 PASS |
| prefix | `before-prefix-status.log` 올바른 $zlink.internal 입력 exit5(stdout/stderr 없음) | `after-prefix.log` exit0 |
| typed empty | `before-refactor.log` static empty FAIL | `after-refactor.log` PASS, 최종 revision 포함 |
| raw empty | `before-raw-empty.log` send/request 2 FAIL | `after-raw-empty.log` PASS, 최종 revision 포함 |
| HTTP 일반 application | `before-http-callback.log:36` 2 failed, 일반 std sink 1 passed | `after-coded-callback.log` 5/5 PASS, callback 횟수1 |
| HTTP coded 예외 | `before-coded-callback.log` provider/sink 2 failed | `after-coded-callback.log` ProtocolError 보존 PASS |
| 모듈 전체 | `build-additional-after.log`, `build-immediate-close.log` build 성공 | `module-final.log:54` 100% tests passed, 0 tests failed out of25. 상세 `module-final-details.log` |
| format | — | `format-final.log`, `format-common-final.log` exit0 |
| download chunk owner 명명 증분 | 전체 25건 통과 이후 16384 값을 유지한 기계적 상수 명명 | `build-download-chunk-owner.log` build exit0, `after-download-chunk-owner.log` 기존 focused 5/5 PASS, `format-download-chunk-owner.log` cpp format exit0 |

최종 module 명령: `ctest --test-dir cpp-r3/build --output-on-failure -j1 -R "test_cpp_stream_connector|test_cpp_stream_revision|test_cpp_http_client|test_connector_|test_cpp_framework_stream_framework|test_cpp_framework_layout_contract"`. 25개는 install consumer/framework stream/layout contract를 포함한다. HTTP focused는 provider/sink transport 모양 오류·일반 std sink·coded provider/sink 5개이다. 수정 후 유효 시험 실패가 없어 사용자 조건인 origin/main 동일 명령 대조는 수행하지 않았다(`origin-main-comparison.log`).

마지막 download_chunk_size 명명은 전체 25건 이후의 같은 값·같은 stack 배열 크기를 유지하는 정리이다. 별도로 -j1 HTTP 증분 빌드와 기존 focused 5개 및 cpp format을 통과했으며, 이 기계적 변경 때문에 전체 25개를 반복하지 않았다.

G01은 fake write completion/future로 순서를 제어했다. nativeclose는 시험 소유 POSIX descriptor만 무효화하며 close 사이에 fd를 만들지 않는다. Manual·Immediate 두 모드 handler 내부에서 public state=Closed/reason=ClientClose를 저장하여 직접 검사한다. Immediate는 strand delivery 완료 future를 기다리며 sleep을 사용하지 않는다. 처음 동기 즉시 관측은 delivery 완료 전 읽어서 실패한 시험 동기화 오류였고 production 변경 없이 completion 관측으로 수정했다.

## 미수정 항목과 wire contract 범위

- HTTP close 실패 우선순위는 **N/A**: `http-client/src/runtime/connection_pool.hpp:25–39`의 noexcept RAII 및 `http_client_runtime.cpp:29` default 소멸에는 close 오류 결과 API가 없다.
- HTTP lazy/one-shot 완료 후 닫기는 **확정 B 누락**: 공통 `02-client-builder.ko.md:11–12`와 `client.cpp:351–383` eager build가 다르다. `client.hpp:114–117`에서 일반 request와 one-shot 표현이 같아 무조건 release는 재사용 의미를 바꾼다. 새 variant/state/API 설계는 승인되지 않아 구현하지 않았다. 시간/package 부재로 검증을 생략한 항목이 아니다.
- Unicode 공백은 **D**: `32-stream-connector.ko.md:120`은 codepoint 집합을 정하지 않는다. Node trim은 U+00A0/U+FEFF 포함, Java isBlank는 둘 제외, .NET whitespace는 U+00A0 포함/U+FEFF 제외. C++의 ASCII 수정으로 전체 Unicode 동치를 주장하지 않는다.
- `received_count`(size_t), `on_packet`(subscription)의 invalid name 오류 반환 표면은 **D**: exact C++ no-throw 계약에서 ValidationFailed 전달 경로가 정해지지 않아 새 event/empty 반환 규칙을 만들지 않았다.
- 기존 wire smoke는 `cross-language/run_cross_language_smoke.sh:459,479,589`에 C++↔.NET 및 Node→C++ roundtrip이 있다. G01 queue timeout·nativeclose 실패·공백 이름의 동일 failure scenario contract는 없다. 감독자 판정에 따라 별도 roundtrip/codec 계약의 host build·smoke를 추가하지 않았다. 네 언어를 함께 실행한 동적 동일성은 미검증이며, 각 언어 회귀에서 확인한 공통 ASCII/terminal/event 결과와 구분한다. 전체 gate는 실행하지 않았다.

## supervisor-guide §2 여덟 기준

| 기준 | 점검 결과 |
|---|---|
| 스펙 gap | A/B 근거만 구현, Unicode/조회 D 및 HTTP B 누락 별도 보고 |
| 불필요한 규칙 | 새 persistent 상태/flag/timer/retry/option 0 |
| 제어 분산 | 위 판정 위치 수 증가 없음, 단일 소유 수렴 |
| 동기화 | production 새 lock/queue/wait 0, 기존 strand/mutex 사용 |
| hot path | 성공 terminal scan 없음, 실패만 기존 deque 삭제, trace off 추가비용 없음 |
| 리팩토링 잔여 | compiler-name resolver/rawliteral 중복 제거, finalized 상태 관측으로 close event 순서 정리 |
| 매직 값 | prefix 공통 상수1개, byte상한 numeric_limits, 기본이름 literal 제거, download_chunk_size 명명으로 16384 소유 명시(값사본1→1) |
| 계층 소유 | connector queue/lifecycle와 HTTP application 단계만 변경, Core/binding 제어 복제 없음 |

리팩토링 점검: 성능 0건·POSDDD 1건·불필요 코드 3건 발견, 고친 항목은 close 오류 event finalized 상태 관측(`connector_runtime.cpp:1569`), 내부 이름 resolver 중복과 raw literal fallback 제거(`zlink_stream_connector.hpp:276`), download chunk literal의 owner 상수 명명(`request_performer.cpp:32,385`), 넘긴 리팩토링 항목 없음. 미수정 계약 범위는 위 항목에 별도 명시했다.
