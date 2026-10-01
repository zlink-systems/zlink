# #1083 node-wire 3단계 보고

작업 위치는 `D:/worktree/zlink-1083-a3-node-wire`, 브랜치는 `framework-node/1083-a3-node-wire`입니다. 검증은 `/home/hep7/worktree/zlink-1083-a3-node-wire`의 WSL 사본에서 수행했습니다. 승인된 A/B 항목을 수정했으며 아래 B 보류·D 항목을 남겼습니다. 전체 작업 완료 또는 PR 승인으로 판정하지 않습니다.

## 기준과 변경 보호

구체적인 job 지시에 따라 기존 변경을 WIP commit `e0348a8c93`으로 보존한 뒤 fetch와 origin/main merge `4608bfb092`를 수행했습니다. 충돌은 없었으며 #1302 스펙을 읽기 기준으로 사용했습니다. push, branch 전환, stash, reset은 수행하지 않았습니다. 스펙 파일을 직접 수정하지 않았습니다.

`report.md`와 `report-r2.md`는 이 worktree와 `D:/project/zlink`의 파일 검색에서 찾지 못했습니다. 해당 보고의 내용을 추정하지 않고 현재 코드·스펙·수정 전 재현 로그를 판정 근거로 삼았습니다.

스펙 대조 검색어는 `close.*fail`, `닫기.*실패`, `close 실패`, `공백 문자`, `unwritten`, `미작성`, `InternalFailure`, `예약`, `reserved`, `compiler`, `컴파일러`입니다. stream connector와 HTTP client의 공통 및 언어별 exact 스펙을 검색했습니다. 주요 소유 조항은 다음과 같습니다.

| 결정 | 소유 문서와 조항 |
|---|---|
| 빈 이름·공백 이름 거부 | `framework/doc/framework/common/spec/stream-connector/32-stream-connector.ko.md:120`, §4.2 |
| 예약 prefix | 같은 문서 §4.6, `$zlink.` |
| request terminal 이후 미작성 frame 폐기 | 같은 문서 `:320`, §5.2 |
| 종료 확정과 close 실패 오류 event | 같은 문서 `:675`, §9 |
| one-shot 결과 우선순위 | `framework/doc/framework/common/spec/http-client/02-client-builder.ko.md:12` |
| application 및 일반 실행 오류 | `framework/doc/framework/common/spec/http-client/09-error-model.ko.md:15` |
| 자동 retry의 kind·streaming 제약 | `framework/doc/framework/common/spec/http-client/06-redirect-retry-cookie.ko.md:30` |
| action 일반 예외 그대로 전파 | 이번 job의 명시적인 감독자 판정 |

## 보류 항목과 검증 한계

1. Unicode 공백 집합은 stream 스펙 `32-stream-connector.ko.md:120`이 구체적으로 정하지 않았습니다. Node `trim`, Java `isBlank`, .NET `IsNullOrWhiteSpace`, C++ ASCII 공백 검증의 집합 차이는 D입니다. 공통 ASCII 공백 거부는 적용하되 상세 집합을 임의 통일하지 않습니다.
2. C++ `received_count` 및 `on`의 이름 오류 반환 방식은 `stream-connector/languages/cpp/03-stream-connector.ko.md:53`, `:95`의 exact 반환형과 `:323`의 result 오류 원칙 사이에 지정된 표면이 없습니다. result를 반환하는 표면은 수정하지만 이 두 표면에 새 오류 전달 정책을 만들지 않습니다(D).
3. C++ HTTP `client.cpp:351`의 one-shot lazy build·완료 후 종료는 B 누락입니다. 기존 reusable client와 one-shot이 같은 client 값으로 표현되어 이를 구분하려면 표현 변경이 필요합니다. 새 상태 금지 조건의 예외 승인을 요청했으며 답변 전에는 구현하지 않습니다. 현재 RAII 종료에는 닫기 실패를 결과로 반환하는 API도 없습니다.
4. Java와 .NET assert의 native TimeoutException을 ConnectTimeout 또는 RequestTimeout으로 정하는 세부 규칙은 지정되지 않았습니다. 이 D 항목은 기존 정책을 유지합니다. timeout 외 action 예외의 원 객체 전파는 별도로 수정합니다.
5. .NET exact 스펙의 회귀 설명 `stream-connector/languages/dotnet/03-stream-connector.ko.md:435` 및 `.en.md:510`은 반복 close/dispose의 같은 실패 관찰을 서술합니다. #1302 공통 §9의 close 실패 event 규칙과 설명이 일치하지 않습니다. 공통 §9를 종료 동작의 소유 조항으로 적용하고 보호된 문서의 잔여 설명은 수정하지 않습니다.
6. Java concrete transport 및 HTTP runtime은 close 실패를 주입할 공개 표면이 없습니다. private reflection이나 시험용 상태/API를 추가하지 않았습니다. connector close event owner의 원인 identity와 HTTP one-shot의 성공·status·decode 실패는 회귀로 확인했으나 실제 native close 실패와 요청 오류의 경쟁은 정적 대조입니다.
7. .NET의 Kind와 RetryAdvice가 모순되는 내부 예외를 HTTP 시험 assembly에서 구성할 수 없습니다. 새 friend/reflection을 추가하지 않았으며 Kind만 사용하는 최종 retry 조건은 정적으로 확인했습니다.
8. Node의 중간 전체 모듈 실패 로그는 실행 스크립트가 같은 경로를 재사용하여 유실했습니다. 같은 중간 생산코드와 당시 실패한 3개 시험을 재구성한 `node-r3-notification-before.log`에서 실패를 다시 확보했습니다. 최초 전체 실행 로그로 취급하지 않습니다. 수정 후와 origin/main에서 같은 3개 시험은 통과했습니다.
9. 같은 G01/공백/close 오류 상황을 네 언어 프로세스에 함께 실행하는 기존 contract는 없습니다. 기존 wire roundtrip smoke는 다른 시나리오이므로 이를 대신 실행하거나 동일성 검증으로 주장하지 않았습니다. 언어별 회귀 실행과 네 구현의 정적 대조를 구분합니다.

## 언어별 근거

상세 원인·수정 위치, 교차언어 대조, 수정 전 실패와 수정 후 검증 로그는 `report-r3-node.md`, `report-r3-java.md`, `report-r3-dotnet.md`, `report-r3-cpp.md`에서 취합합니다. 최종 판정은 agent 보고만으로 내리지 않고 변경된 모든 줄과 로그 요약을 감독자가 직접 확인합니다.

## 최종 결정 소유 표

경로 기준은 `framework/languages/`입니다. Java connector 파일은 `java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/`, Java HTTP 파일은 `java/zlink-http-client/src/main/java/systems/zlink/httpclient/`에 있습니다. .NET connector 파일은 `dotnet/src/Systems.Zlink.Stream.Connector/`, .NET HTTP 파일은 `dotnet/src/Zlink.HttpClient/`에 있습니다. 아래 표는 수정 후 위치이며 수정 전 원인은 각 언어 보고의 첫 표에 있습니다.

| 결정 | Node | Java | .NET | C++ |
|---|---|---|---|---|
| terminal 이전 미작성 frame 제거 | `node/packages/stream-connector/src/Runtime/ZlinkStreamFrameSender.ts:40` | `ZLinkStreamSendChain.java:73` | `Runtime/ZlinkStreamOneWaySubmitQueue.cs:149` 기존 준수 | `cpp/connector/core/src/runtime/calls/zlink_stream_calls.cpp:652` |
| 이름 구조 | `node/packages/stream-wire/src/index.ts:669` | `ZLinkStreamWireProtocol.java:315` | `Runtime/ZlinkStreamConnector.cs:639` | `cpp/common/include/zlink/detail/stream_packet_name.hpp:18` |
| close 실패 event | `node/packages/stream-connector/src/Runtime/ZlinkStreamConnectorLifecycle.ts:532` | `ZLinkStreamConnectionLifecycle.java:817` | `Runtime/ZlinkStreamConnectorLifecycle.cs:763` | `cpp/connector/core/src/runtime/connector_runtime.cpp:498` |
| HTTP one-shot 결과 | `node/packages/http-client/src/request-builder.ts:94` | `ZLinkHttpRequestBuilder.java:259` | `ZLinkHttpRequestBuilder.cs:174` | `cpp/http-client/src/client.cpp:351` B 보류 |
| assertion 일반 예외 | `node/packages/stream-connector/src/Runtime/ZlinkStreamAssertions.ts:30` | `ZLinkStreamAssert.java:60` | `Contracts/ZlinkStreamAssert.cs:90` | `cpp/connector/core/include/zlink/stream_connector/contracts/zlink_stream_assert.hpp:51` |
| HTTP 실행 오류 | `node/packages/http-client/src/runtime/request-performer.ts:124/:206` 기존 stage 전달 | `internal/HttpClientErrors.java:57` | `Runtime/HttpFailureMapper.cs:13` | `cpp/http-client/src/runtime/request_performer.cpp:35` |

queue 취소는 active write를 취소하지 않고 미작성 항목을 terminal 전에 제외합니다. close 실패는 연결 종료 사유를 대체하지 않는 `Disconnected` event이며 기존 reconnect 정책을 유지합니다. one-shot cleanup은 typed status/decode를 포함한 요청 실패를 우선합니다. 일반 action 오류의 원 객체는 helper가 다른 오류로 바꾸지 않습니다. C++의 returned `result_t` 실패 판정은 유지합니다.

## 판정 위치 전/후

숫자는 코드 줄 수가 아니라 해당 결정의 소유 판정 위치 수입니다. 구조 검증과 connector 예약 이름 정책은 다른 결정이며 구조 검증 사본만 제거했습니다.

| 결정 | Node | Java | .NET | C++ |
|---|---|---|---|---|
| terminal/미작성 제거 | 1→1 | 2→1 | 1→1 | 2→1 |
| 이름 구조 | 2→1 | 2→1 | 1→1 | 2→1 |
| close 오류 처리 | 3→1 | 2→1 | 2→1 | 6→1 |
| HTTP cleanup/우선순위 | 2→1 | 1→1 | 2→1 | 보류 |
| assert 분류 owner | helper별 1→1 | 1→1 | 1→1 | 일반 예외 변환 3→0 |
| HTTP 실행 오류 owner | 기존 1→1 | 1→1 | 1→1 | 2→1 |

추가로 Node 종료 state와 notification은 각각 2→1, Java raw connect cleanup은 3→1, C++ accepted close 시작은 3→1·typed 이름은 2→1·raw 기본 이름 발명은 2→0, .NET retry 수락은 2→1입니다. 새 persistent state·flag·timer·retry·option·lock을 추가하지 않았습니다. C++ pending의 중복 request_seq 필드는 write_id로 교체했으며 Java pending의 packetName 필드는 completion expression으로 교체하여 필드 수를 유지했습니다.

## 감독자 리뷰

리뷰 범위는 main merge 이후 stage 3의 tracked diff와 새 source/test/report입니다. 앞 단계 WIP 보존 commit은 앞 보고가 없는 상태이므로 이 보고가 stage 1·2의 승인 기록을 대신하지 않습니다. stage 3 변경 줄과 수정 전 실패·수정 후 요약 로그를 직접 확인했으며, Java 전체 결과는 원본 JUnit XML 합계를 별도로 계산했습니다. 보호된 스펙 파일의 working-tree 수정은 없습니다.

| supervisor-guide §2 기준 | 판정 |
|---|---|
| 스펙 gap | 지정된 A/B만 구현. 상세 Unicode·C++ 조회 오류 표면·native timeout subtype D는 중단 |
| 불필요한 규칙 | 상태 사본·close 오류 보관·inner 오류 추정 제거. 새 persistent 제어 상태 없음 |
| 제어 분산 | 위 표의 판정 위치는 유지 또는 감소 |
| 동기화 | 기존 queue lock·strand·lifecycle gate 사용. gate를 우회하는 lock/queue/대기 추가 없음 |
| hot path | 성공 terminal queue scan 없음, reusable HTTP operation closure 없음, trace 추가 없음. Node 종료 pending Promise 및 one-shot outcome의 희소 비용은 보고 |
| 리팩토링 잔여 | touched 모듈의 중복 resolver·cleanup·분류를 제거. 범위 밖 .NET public defaults 사본은 아래 위치로 보고 |
| 매직 값 | 예약 prefix·backoff·status·buffer/chunk 상수 소유 수렴. Java native timeout 메시지 분기는 D로 보존 |
| 계층 소유 | Framework request queue·lifecycle·HTTP 오류 결정만 수정. Core/binding 제어를 재구현하지 않음 |

소유 계층: Framework frame write queue·wire 이름 구조·connector lifecycle·HTTP request execution/cleanup입니다.

스펙 조항: stream32 §4.2·§4.6·§5.2·§9, HTTP02:11–12·HTTP09:15·HTTP06:30 및 사용자의 assertion 판정입니다.

교차언어 대조: 네 언어의 결정 소유를 정적으로 대조하고 각 언어 회귀를 실행했습니다. 전체 Unicode 동치와 네 언어 동시 failure contract 통과는 주장하지 않습니다.

변경 분류: 기존 누락 수정 B, #1302 종료 계약 및 내부 interface/회귀 기대 적응 A입니다. C 우회는 반려했습니다. D 및 C++ HTTP B 보류는 위 항목에 남겼습니다.

리팩토링 점검: 성능 6건·POSDDD 12건·불필요 코드 18건 발견, queue/이름/close/HTTP cleanup owner 수렴·중복 참조/상태/이름 resolver 삭제·상수 명명을 수정했습니다. 넘긴 항목은 .NET public defaults 사본 2건(timeout/body limit; `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpClientBuilder.cs:21/:25`, `Runtime/HttpClientOptions.cs:17/:19`) 및 위 D 항목입니다. 수치는 언어별 보고의 분류별 합계이며 한 수정이 여러 기준을 충족할 수 있습니다.

## 검증 결과

로그 기준 디렉터리는 `.artifacts/node-wire-r3/`입니다. 각 언어 보고에 수정 전 실제 실패 로그·첫 실패·개별 수정 위치가 있습니다. 결함을 재현하지 못한 Java native close 경쟁과 C++ HTTP 수명 보류는 통과로 계산하지 않았습니다. 초기 환경·시험 준비 오류와 잘못된 신규 시험 가정도 제품 결함 재현에서 제외했습니다.

| 언어 | 해당 모듈 unit·contract 전체 | 로그 |
|---|---|---|
| Node | 239건 통과, 실패 0 | `node/node-r3-performance-module.log:1439` |
| Java | stream 183건·HTTP 60건 통과, 실패/오류/skipped 0 | `java/module.log:62`, `java/module-results/` 원본 JUnit XML |
| .NET | stream 최종 259건·HTTP 81건 통과 | `dotnet/dotnet-r3-final-stream-module.log`, `dotnet/dotnet-r3-module-Zlink.HttpClient.UnitTests.log:652` |
| C++ | 25개 test case 통과, 실패 0. install consumer·framework stream·layout 포함 | `cpp/module-final.log:54` |

Node의 중간 notification 3건 실패는 수정 후와 origin/main의 같은 focused 명령에서 각각 3건 통과했습니다. .NET 기존 helper 기대 1건 실패는 origin/main 같은 stream 전체 명령에서 253건 통과한 뒤 새 action 예외 계약에 맞춰 원 객체 전파를 검증하는 기대값으로 수정했습니다. 그 결과 최종 stream 259건이 통과했습니다. direct timeout·coded·caller cancellation 양성 검사는 유지했습니다.

Java와 C++ 수정 후 유효 전체 모듈 실패는 없어 origin/main 대조 조건이 발생하지 않았습니다. C++ download chunk 상수 명명과 .NET ReadBufferSize 상수화는 각각 전체 모듈 뒤의 같은 값 기계적 변경이며 증분 build·HTTP focused·최종 format으로 별도 확인했습니다. C++ focused 5/5(`cpp/after-download-chunk-owner.log`), .NET focused 11/11(`dotnet/dotnet-r3-final-http-focused.log`)이 통과했습니다. 전체 gate 및 CI는 실행하지 않았습니다.

format 명령은 WSL job root의 `scripts/format/format.sh --check node|java|dotnet|cpp`이며 네 언어 모두 통과했습니다. 최종 로그는 Node `node-r3-performance-format-check.log`, Java `format.log`, .NET `dotnet-r3-final-format-check.log`(1153개), C++ `format-download-chunk-owner.log`입니다. C++ 새 common header는 추가 clang-format dry-run도 통과했습니다. 상세 명령·local package 경로·종료코드는 각 언어 보고와 로그에 보존했습니다.

모든 agent의 CPU 작업 종료와 Windows 로그 보존을 확인한 뒤 WSL job 사본의 realpath가 지정된 job root와 일치하는지 확인하고 해당 사본만 삭제했습니다. 읽기 전용 C++/Java local package는 수정하거나 삭제하지 않았습니다. Windows의 자체 origin/main archive도 제거했으며 source와 보고서 및 검증 로그는 보존했습니다.
