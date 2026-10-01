# Node #1083 3단계 보고

## 범위와 판정

브랜치는 `framework-node/1083-a3-node-wire`입니다. Node stream connector, stream-wire, HTTP client와 해당 시험만 수정했습니다. 스펙, commit, push, branch, stash, reset 조작은 하지 않았습니다. 감독자가 정적 대조 뒤 G01(B), R06(A), R12 connector(A), R12 HTTP(A), assert(B)를 승인했습니다.

파일 경로의 `N`은 `framework/languages/node/`, `J`는 `framework/languages/java/`, `D`는 `framework/languages/dotnet/`, `C`는 `framework/languages/cpp/`입니다.

## 결정 소유 표와 교차언어 대조

| 결정 | 소유 계층·스펙 | Node 원인 → 수정 | 다른 세 언어 정적 대조 | 분류·판정 위치 전/후 |
|---|---|---|---|---|
| terminal request의 미작성 frame 제거 | connector frame write queue; `framework/doc/framework/common/spec/stream-connector/32-stream-connector.ko.md:320` | `N/packages/stream-connector/src/Runtime/ZlinkStreamFrameSender.ts:40`의 cancel이 Set 항목을 남겼습니다. 동일 cancel 위치의 `has`를 `delete`로 바꾸었습니다. pending timeout의 `ZlinkStreamPendingRequests.ts:48`은 기존 expiry promise를 통하여 이 소유자를 사용합니다. | J `zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamSendChain.java:72` pollFirst와 `DefaultZLinkStreamConnector.java:417`; D `src/Systems.Zlink.Stream.Connector/Runtime/OneWaySubmitQueue.cs:149` 취소 token 항목 제거, `ZlinkStreamConnector.cs:588` linked CTS; C `connector/core/src/runtime/calls/zlink_stream_calls.cpp:1068` queue 항목과 `:1178` advance | B, 1 → 1 |
| 공백·바이트 길이의 이름 구조 검증 | stream-wire; 동일 스펙 `:120` | connector `Protocol/ZlinkStreamPacketNameValidator.ts:5`와 wire `src/index.ts:665`에 사본이 있었습니다. wire `src/index.ts:669` validator를 `@internal` export하고 connector `:8`은 해당 검증의 오류 적응과 기존 예약 prefix 정책만 소유합니다. | J `ZLinkStreamWireProtocol.java:316` 길이와 `DefaultZLinkStreamConnector.java:714` isBlank; D `ZlinkStreamConnector.cs:633` 이름 검증; C `connector/core/src/runtime/protocol/header_codec.cpp:136` 이름 구조 검증 | A, 구조 2 → 1; 예약 prefix 1 → 1 |
| transport close 실패 전달 | connector lifecycle; 동일 스펙 `:675` | `ZlinkStreamConnectorLifecycle.ts:500`은 실패를 버리고 `:115` late close는 오류 상태를 보관했습니다. 수정된 `closeTransport:529` 한 곳이 `Disconnected` error event를 게시하며 teardown과 late close가 재사용합니다. 종료 사유·reconnect 경로를 변경하지 않습니다. `lateConnectCleanupError`와 close 오류 배열·AggregateError 분기를 제거했습니다. | J `ZLinkStreamConnectionLifecycle.java:803` close; D `ZlinkStreamConnectorLifecycle.cs:651` closeFailure 경로; C `connector/core/src/runtime/connector_runtime.cpp:1543` shutdown_and_close 호출, `runtime/transport/stream_connection.cpp:73` 및 `:157` native 종료 구현. C++ 수정 판정은 담당 agent가 별도 보고합니다. | A, close 실패 처리 3 → 1 |
| one-shot 결과 우선순위 | HTTP request builder; `framework/doc/framework/common/spec/http-client/02-client-builder.ko.md:12` | `packages/http-client/src/request-builder.ts:97`은 close 실패를 버렸고 raw/download가 별도 finally를 소유했습니다. `:94` execute가 typed status·decode를 포함한 요청 결과를 먼저 확정하고 요청 실패 우선, 요청 성공 시 close 실패를 결과로 냅니다. | J `zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:262` close catch; D `src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:171` Dispose와 `:184`, `:208` finally; C `http-client/include/zlink/http_client/contracts/client.hpp:114` 값 소유와 `src/runtime/connection_pool.hpp:25` RAII 종료, close 실패 표현은 담당 agent가 별도 보고 | A, 실행/cleanup 경로 2 → 1; client 소유권 1 → 1 |
| assert action 일반 예외 | assert helper; 사용자 감독 판정, 동일 stream 스펙 대기·assert 표면 | `ZlinkStreamAssertions.ts:30`은 일반 오류를 RemoteError로 변환하여 반환했습니다. 분류된 ZlinkStreamException만 해석하고 나머지는 원 값을 throw합니다. expectTimeout도 catch 안에서 판정하여 `throw undefined`를 성공 실행과 혼동하지 않습니다. | J `ZLinkStreamAssert.java:26`, `:46`; D `Contracts/ZlinkStreamAssert.cs:36`, `:65`; C `contracts/zlink_stream_assert.hpp:58`, `:92` | B, helper별 1 → 1 |

네 언어의 공백 집합은 Node `trim`, Java `isBlank`, .NET `IsNullOrWhiteSpace`, C++의 이름 구조 검증에서 완전히 같다고 단정할 수 없습니다. 예를 들어 ECMAScript trim은 U+00A0를 공백으로 보지만 Java isBlank는 그렇지 않습니다. 스펙 `32-stream-connector.ko.md:120`은 Unicode 공백 집합을 정하지 않습니다. 이 상세 정책은 D로 보고하며 임의 통일하지 않았습니다. 이번 Node 변경은 기존 stream-wire 소유자의 공백 판정을 모든 connector 이름 표면이 사용하게 합니다. 네 언어 결과의 동적 동일성 시험은 root의 취합 범위입니다.

## 검증 기록

로그 위치는 `.artifacts/node-wire-r3/node/`입니다. 빌드·시험은 WSL의 이 job 사본에서 실행했습니다.

| 시험 | 수정 전 | 수정 후 |
|---|---|---|
| G01 timeout/cancel | `node-r3-before.log`: G01 두 시험에서 `written`에 second frame이 남았습니다. | 최종 focused/module 로그에서 두 시험 통과 |
| R06 이름 구조 | `node-r3-r06-before.log`: tests 1, pass 0, fail 1, `Missing expected exception` | 최종 focused/module 로그에서 빈 이름·ASCII 공백·U+3000의 on/waitFor/send/request 거부 통과 |
| R12 수동 close와 HTTP raw/typed/download | `node-r3-before.log`: connector close 실패 throw와 HTTP 성공 시 close 실패 누락을 재현했습니다. HTTP 요청 실패 우선인 세 양성 대조는 기존 구현에서도 통과했습니다. | 최종 focused/module 로그에서 error event·원 cause·ClientClose 및 요청/닫기 실패 우선순위 통과 |
| R12 late close | `node-r3-late-close-before-final.log`: tests 1, pass 0, fail 1. 구 구현에서 late 연결 cleanup 오류가 connect/close 결과를 바꾸었습니다. | `node-r3-close-final.log`: tests 2, pass 2, fail 0. 연결 종료 오류·close 완료·Disconnected event·원 cause·ClientClose·close 1회 통과 |
| assert 일반 예외 | `node-r3-before.log`: 일반 Error action을 expectFailure가 변환했습니다. | 최종 focused/module 로그에서 Error·TypeError·문자열·undefined 원 값 전파 통과 |

첫 `node-r3-before.log`의 R06 실패는 잘못된 시험 API `onReceived` 호출이므로 결함 재현으로 세지 않습니다. 이름 시험을 실제 공개 API `on`으로 수정한 뒤 원본 validator를 WSL 사본에 적용하여 별도 `node-r3-r06-before.log`에 유효한 실패를 보존했습니다.

`node-r3-module.log`의 browser module 부재는 build 준비 실패입니다. 산출물 생성 후 `node-r3-module-final.log`에서는 tests 236, pass 235, fail 1이었으며 구 계약 concurrent close의 rejects 기대만 실패했습니다. 감독자가 #1302 §9에 맞춘 계약 시험 개정을 승인했고 단순 expectation 제거 대신 event·cause·ClientClose·close 1회를 확인했습니다.

최종 결과는 다음과 같습니다. 남은 Node 시험 실패는 없습니다.

- `node-r3-focused-final.log`: tests 14, pass 14, fail 0.
- `node-r3-close-final.log`: tests 2, pass 2, fail 0.
- `node-r3-module-complete.log`: tests 237, pass 237, fail 0.
- `node-r3-build-late.log`: TypeScript compile 오류 없음. `node-r3-browser-build-final.log`: browser compile/bundle 오류 없음.
- `node-r3-format-final.log`: `All matched files use Prettier code style!`.
- `node-r3-origin-main-module.log`: origin/main의 같은 모듈 명령 tests 208, pass 208, fail 0. 새 회귀 14개와 이전 WIP 시험은 origin/main에 없으므로 총수가 다릅니다.
- `node-r3-origin-main-format.log`: origin/main에서도 `All matched files use Prettier code style!`.
- `git diff --check -- framework/languages/node`: 오류 없음.

모듈 명령은 `node --test packages/stream-connector/test/*.cjs packages/stream-wire/test/*.cjs test/contract/stream-connector*.test.js test/contract/stream-test-helper-adoption.test.js test/contract/http-client*.test.js`입니다. Format 명령은 repository root의 `bash scripts/format/format.sh --check node`입니다. 기존 불일치가 드러난 뒤 원인과 계약 시험이 바뀐 경계만 먼저 focused 확인하고 최종 모듈을 재검증했습니다. 전체 repository gate는 실행하지 않았습니다.

origin/main source snapshot은 이 job의 WSL 하위 `node-r3-origin-main`에서만 준비했습니다. 첫 browser build는 snapshot에 Unity source가 없어 중단되었고(`node-r3-origin-main-build.log`), origin/main의 Unity asset source를 같은 snapshot에 읽기 전용 복사한 뒤 `node-r3-origin-main-assets.log`에서 준비를 완료했습니다. 이 준비 문제는 제품 결함으로 세지 않았습니다. Windows의 source archive 임시 파일은 삭제했고 최종 source와 모든 Node 로그는 Windows worktree로 복사했습니다.

## supervisor-guide §2 여덟 기준

| 기준 | 점검 결과 |
|---|---|
| 스펙 gap | 동작 근거는 표에 기재했습니다. Unicode 공백 집합 상세 D는 구현하지 않았습니다. |
| 불필요한 규칙 | 새 상태·flag·timer·retry·option은 없습니다. late close error 상태와 오류 배열을 제거했습니다. |
| 제어 분산 | 미작성 frame 제거 1→1, 이름 구조 2→1, close 오류 3→1, HTTP cleanup 경로 2→1, assert helper별 1→1입니다. |
| 동기화 | handler/infrastructure 완료 대기를 추가하지 않았습니다. callback event는 기존 dispatch queue를 사용합니다. 시험은 promise 완료 신호로 순서를 확인합니다. |
| hot path | queue cancel은 Set.delete 한 번입니다. 큐 순회·lock·frame 복사·타이머를 추가하지 않았습니다. 일반 assert 예외 변환 객체 생성을 제거했습니다. HTTP reusable client에는 allSettled를 적용하지 않습니다. one-shot 종료에만 표준 promise outcome이 생깁니다. |
| 리팩토링 잔여 | 이름 검증 사본, late close 별도 오류 상태, HTTP cleanup helper 사본을 이 작업에서 정리했습니다. |
| 매직 값 | stream-wire가 기존 예약 prefix를 `@internal` 상수 하나로 소유하고 control 이름 네 개와 connector 판정이 재사용합니다. 값 사본 5→1, 판정 위치 1→1입니다. 해당 파일은 생성 파일이 아닙니다. |
| 계층 소유 | Framework connector의 logical request/frame queue와 lifecycle event만 수정했습니다. Core/binding reconnect·물리 연결 선택 정책을 다시 구현하지 않았습니다. |

리팩토링 점검: 성능 1건·POSDDD 3건·불필요 코드 3건 발견, HTTP reusable client outcome 할당·이름 검증 중복·close 오류 상태·HTTP cleanup 중복·assert 예외 변환·예약 prefix 사본을 수정했습니다. 범위 밖 이월 결함은 없으며 Unicode 상세 정책 D는 `32-stream-connector.ko.md:120`에 보고했습니다.


## 최종 추가 검증과 성능·종료 경계 점검

HTTP owner는 `request-builder.ts:94`에서 operation closure 대신 실제 `Promise<T>`를 받습니다. raw/download는 요청 구조 검증을 client 생성보다 먼저 수행하고 `runtime.ts:27`과 `browser-runtime.ts:27`의 실제 async 실행 결과를 전달합니다. typed decode는 실제 async 메서드에서 수행하여 동기 decode 실패도 cleanup owner에 들어갑니다. reusable raw의 async wrapper 수는 전후 2→2, typed는 3→3이며 새 operation closure를 만들지 않습니다. one-shot에만 `allSettled` 결과 배열·outcome을 생성합니다. 이는 코드 경로 근거이며 할당량 실측 결과가 아닙니다.

Immediate close 오류 handler에서 상태를 관측한 회귀는 최초 `node-r3-state-before.log`에서 실패 2건·통과 1건이었습니다. 종료 상태 확정을 transport close 완료 owner로 이동했습니다. 늦게 반환한 연결은 `closeRequested`가 확정된 종료 이후에만 닫으며 이 값은 다시 false로 바뀌지 않습니다. 종료 후 새 연결을 수락하지 않으므로 다른 살아 있는 generation 상태를 Closed로 덮지 않습니다.

동기 transport close throw가 state handler의 connect 재진입을 허용하던 경계는 `node-r3-claim-before.log`의 1건 실패로 확인했습니다. 기존 disconnectTask를 transport 종료 실행 전에 native pending Promise로 선점하여 `node-r3-claim-after.log`에서 1건 통과했습니다. ES2022 계약 때문에 Promise.withResolvers를 쓰지 않고 Promise constructor의 resolve/reject 지역변수를 사용합니다. 새 runtime 필드는 없지만 드문 종료 경계에 pending Promise와 executor closure, teardown.then()의 결과 Promise가 추가됩니다. 기존 finally wrapper는 전후 모두 존재합니다. 재진입이 실제 종료 완료를 관측하게 하는 비용이며 성능 개선으로 주장하지 않습니다.

종료 상태를 먼저 확정하고 disconnected 게시를 나중에 하던 중간 구현은 module 239건 중 236건 통과·3건 실패했습니다. 최초 module 실패 파일은 실행 스크립트의 같은 경로 재사용으로 덮어써 유실되었습니다. 해당 중간 생산코드 경계를 이 job의 WSL source 임시 사본으로 재구성한 후 당시 실패한 같은 3건의 focused 명령으로 `node-r3-notification-before.log`에 실패 3건을 다시 확보했습니다. 최초 module 로그와 동일한 실행 기록으로 취급하지 않습니다. 기존 시험의 assertion은 바꾸지 않았습니다.

최종 close owner가 상태→close 오류 event→disconnected 게시를 완료하고 callback completion을 기다리지 않도록 수렴했습니다. closeOnce와 announceDisconnect의 게시 사본을 삭제했으며 connect 실패의 기존 게시 경로는 유지했습니다. 기존 disconnectedPublished 판정을 재사용했습니다. 종료 state 판정 2→1, 종료 notification 게시 2→1, teardown 진행 선점 판정 1→1입니다.

로그는 모두 `.artifacts/node-wire-r3/node/`에 있습니다.

| 최종 검증 | 로그 | 요약 |
|---|---|---|
| 추가 focused | node-r3-performance-focused.log | tests 16, pass 16, fail 0 |
| 전체 해당 모듈 unit·contract | node-r3-performance-module.log | tests 239, pass 239, fail 0 |
| format check node | node-r3-performance-format-check.log | All matched files use Prettier code style |
| 중간 결함 focused 재구성 | node-r3-notification-before.log | tests 3, pass 0, fail 3 |
| 해당 결함 수정 후 | node-r3-notification-after.log | tests 3, pass 3, fail 0 |
| origin/main 동일 focused | node-r3-notification-origin-main.log | tests 3, pass 3, fail 0 |

최종 리팩토링 추가 발견은 HTTP operation closure 할당(성능), 종료 상태·notification 판정 분산과 선점 시점(POSDDD), 오래된 설명과 중복 closeRequested 판정(불필요 코드)이었습니다. 모두 같은 작업에서 고쳤습니다. 기존 점검에 합산한 최종 결과: 리팩토링 점검: 성능 2건·POSDDD 5건·불필요 코드 5건 발견, HTTP closure 제거·종료 상태/notification owner 수렴·teardown 선점·중복 판정/설명 제거를 포함하여 수정, 넘긴 항목은 Unicode 공백 집합 D(`32-stream-connector.ko.md:120`)입니다.
