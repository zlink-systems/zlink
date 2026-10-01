# Java 3단계

승인된 Java 구현과 focused·전체 unit/contract·format 검증을 마쳤습니다. 커밋·push·스펙 수정은 하지 않았습니다.

## 결정 소유와 정적 대조

원인 라인은 수정 전 기준이며 경로는 `framework/languages/<언어>` 기준입니다.

| 결정/스펙 | Java 원인 | Node | .NET | C++ | 수정 소유/분류 |
|---|---|---|---|---|---|
| 미작성 request 취소, stream32 §5.2:320 | SendChain.java:72, Connector.java:417 | FrameSender.ts:40/57, PendingRequests.ts:48 | OneWaySubmitQueue.cs:149–164, Connector.cs:588–596 기존 준수 | calls/zlink_stream_calls.cpp:651 | SendChain.cancelWrite/B |
| 이름 공백, stream32 §4.2:120 | Connector.java:714, WireProtocol.java:320 중복 | PacketNameValidator.ts:5/19, stream-wire/index.ts:665 | Connector.cs:639/645 | framing/header_codec.cpp:136, stream_runtime.cpp:1065 | WireProtocol.validatePacketName/B |
| close 오류 event, stream32 §9:675 | TcpTransportConnection.java:70 swallow, Lifecycle.java:176 종료 경계 | Lifecycle.ts:500/547 | Lifecycle.cs:651–680 | transport_connection.hpp:109 native error 무시 | Lifecycle.publishCloseFailure/B |
| HTTP close 우선순위, HTTP02:12 | RequestBuilder.java:263 swallow | request-builder.ts:97 | RequestBuilder.cs:184/208 masking | client.cpp:351–383 eager one-shot; client.hpp:114–117; connection_pool.hpp:25–39 noexcept RAII; http_client_runtime.cpp:29 default destructor | RequestClientLease.release/B; C++ close-error API 부재로 precedence N/A, one-shot lifecycle B 보류 |
| action 예외 identity, 사용자 판정 | Assert.java:64–73 변환 | Assertions.ts:30–51 | Assert.cs:102–105 | stream_assert.hpp:54–88 | StreamAssert.classify/B |
| HTTP 일반 실행 오류, HTTP09:15 | ProviderInputStream/ResponseBodyReader의 callback 오류 경계 | 단계 인자를 받는 R11 mapper | RetryPolicy.cs:64 일반 오류 미분류 | 담당자 report-r3-cpp.md 대조 | HttpClientErrors.fromExecutionFailure/B |

## 변경

G01은 기존 queue lock에서 미작성 항목을 제거하고 lock 밖에서 terminal을 완료합니다. 기존 request 사본 result Future를 제거해 queue 소유 Future 하나를 반환합니다. timeout·caller cancel은 같은 삭제 소유자를 사용하며 실제 native write 실패만 lifecycle을 종료합니다. 최종 LIFO 회귀는 생산 enqueueRequestDeferred와 PendingRequests.add 연결을 사용합니다.

PendingRequest의 기존 packetName 필드를 completion expression으로 교체했습니다. reply와 failure hook의 등록 snapshot은 public Future 완료 전에 얻고 hook은 완료 후 게시합니다. caller cancel은 hook을 게시하지 않습니다. snapshot 실패는 원 오류로 request를 끝내고 reply를 닫습니다. binding Message.java:22–25는 close 후 재사용을 금지하므로 폐기 후 getter 예외를 검사하지 않습니다.

R06은 wire 소유자가 null·blank·UTF-8 길이를 검사하고 connector가 위임합니다. reserved prefix 정책은 connector에 유지합니다. encodeHeader는 검증에서 만든 bytes를 재사용합니다.

R12 connector는 TCP close IOException을 원 cause가 있는 UncheckedIOException으로 전달합니다. lifecycle이 close 오류를 local 반환값으로 받아 기존 정리를 계속하고 state/reason 통지 후 단일 Disconnected event owner에 전달합니다. raw late cleanup도 같은 owner를 사용합니다. TCP connect의 선행 raw close 두 곳은 기존 result.whenComplete cleanup과 겹쳐 삭제했습니다. 이는 순수 중복 제거이며 native close 오류 2개가 재현되었다고 주장하지 않습니다.

HTTP lease release는 요청 실패를 우선하고 성공이면 close 실패를 전파합니다. typed status/decode까지 같은 execute에서 마친 뒤 release합니다. provider/sink는 실행 단계를 기존 오류 mapper에 전달합니다. 직접 coded 오류는 보존하고 임의 application inner coded 오류는 해석하지 않습니다. transport native wrapper만 typed cause를 복구합니다. Assert는 coded/timeout 이외 action 오류와 일반 wrapper의 최초 catch 객체를 그대로 전파합니다.

## 검증 로그

| 단계 | 로그 | 요약 |
|---|---|---|
| 수정 전 | `.artifacts/node-wire-r3/java/before.log` | stream 회귀5/5 실패, HTTP application2/2 실패, BUILD FAILED 41s |
| reactive 취소 대안 결함 | `.artifacts/node-wire-r3/java/before-lifo.log` | user terminal callback이 first write를 해제하면 expired frame1회 발행, BUILD FAILED 14s |
| inner coded 추가 재현 | `.artifacts/node-wire-r3/java/before-nested.log` | application4건 중 nested coded1건 실패, BUILD FAILED35s |
| 수정 후 focused | `.artifacts/node-wire-r3/java/after-focused-final.log` | stream R3 10+pending1, HTTP application4 모두통과; XML focused-results 보존; BUILD SUCCESSFUL58s |

before-environment.log의 Maven 경로 오류는 환경 실패이며 결함 재현으로 계산하지 않았습니다. 읽기 전용 localPackageRoot 지정 뒤 실제 실패를 확보했습니다. 초기 신규 snapshotfailure 테스트의 폐기 후 Message getter 예외 assertion은 binding 계약에 없는 잘못된 가정이었습니다. 감독 승인으로 원 오류 identity·pending 제거를 검사하도록 정정했으며 기존 fixture/assertion을 완화하지 않았습니다.

## 판정 위치 수

| 결정 | 전 | 후 | 단일 소유자 |
|---|---:|---:|---|
| 미작성 request terminal과 queue 제거 | 2 | 1 | SendChain.cancelWrite |
| 이름 구조 | 2 | 1 | WireProtocol.validatePacketName |
| transport close 오류 분류 | 2 | 1 | Lifecycle.publishCloseFailure |
| TCP connect failure raw cleanup | 3 | 1 | result.whenComplete |
| one-shot 요청/close 우선순위 | 1 | 1 | RequestClientLease.release |
| action 오류 분류 | 1 | 1 | StreamAssert.classify |
| HTTP 실행 오류 분류 | 1 | 1 | HttpClientErrors.fromExecutionFailure |

새 persistent state·field·flag·timer·lock·retry·option을 추가하지 않았습니다. 기존 record 필드를 교체하고 existing Write 참조를 종료 시 비웁니다. timeout 증가는 없습니다.

## 마무리 점검

성능: request Future2개→1개, 이름 UTF-8 변환2회→1회. terminal Future→SendChain→Write 경로는 남지만 기존 supplier는 claim/cancel/reset에서, failure consumer는 nativefinish/queuedcancel/reset에서 비워 connector/frame을 장기 보유하지 않습니다. map completion expression은 map 제거 뒤 남지 않고 기존 cancellation/timeout callback은 CompletableFuture terminal completion stack에서 drain됩니다. cancel queue scan은 timeout/cancel 경로에만 있고 정상 hot path에 추가하지 않았습니다. 기존 lazy trace 경로를 유지합니다. 수치는 별도 benchmark가 아니라 allocation/closure 정적 근거입니다.

| supervisor §2 기준 | 결과 |
|---|---|
| 스펙 gap | 승인된 B 항목은 위 근거 사용. Unicode/timeout subtype D는 보존 |
| 불필요한 규칙 | 새 state/flag/timer/retry/option 없음; existing expression 교체 |
| 제어 분산 | 위 판정 위치 수 같거나 감소 |
| 동기화 | 기존 lock만 재사용; terminal과 user callback은 queue lock 밖, 별도 gate/인프라 wait 없음 |
| hot path | Future/UTF-8 사본 제거, terminal closure 참조 해제, trace off lazy 경로 보존 |
| 리팩토링 잔여 | 수명/중복 validation/raw close/사본 Future 정리; D 동작은 변경하지 않음 |
| 매직 값 | status 경계 FIRST_ERROR_STATUS와 기존 retry BACKOFF 소유 상수 명명; timeout prefix D 보존 |
| 계층 소유 | queue취소=sender, correlation/hook=pending, 종료=lifecycle, HTTP분류=mapper; Core/binding 제어 재구현 없음 |

리팩토링 점검: 성능3건·POSDDD3건·불필요 코드3건 발견, 고친 항목은 Future/UTF-8 사본·terminal 참조 수명·queue/validation/close 소유 통합·rawclose 중복·매직 값 명명, 넘긴 항목은 StreamAssert timeout subtype 메시지 분기(D; framework/languages/java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamAssert.java:70).

## 보류와 검증 한계

- Unicode blank: stream32 §4.2:120은 집합을 지정하지 않고 Java isBlank/Node trim/.NET IsNullOrWhiteSpace는 다릅니다. 공통 ASCII 공백 거부만 적용하고 정확한 집합은 D로 보존합니다.
- plain TimeoutException의 Request/Connect subtype: Java StreamAssert 기존 connect timed out prefix와 다른 언어 선택은 스펙에 정해져 있지 않습니다. 사용자 판정은 timeout 외 오류이므로 해당 세부를 D로 보존합니다.
- Java connector는 concrete TCP/WebSocket을 private 경계에서 생성하며 public fake transport 주입이 없습니다. close owner unit에서 원 오류 identity+Disconnected event를 시험했으나 실제 public connector close 오류 state/reason snapshot을 주입하지 못했습니다. 자연스러운 종료 경계의 순서는 production 정적 대조로 확인합니다. 사용자 disconnected/state handler가 새 연결을 시작하는 정상 재진입에서는 오류 event가 항상 Closed를 관측한다고 단정하지 않습니다.
- HTTP concrete runtime/lease 종료 경계에는 close 실패 주입 API가 없습니다. 새 API/state/reflection 없이 public one-shot typed성공/status500/invalidJSON 및 단일사용3건을 추가했습니다. close failure identity 동적 재현은 미검증입니다.

## 최종 수정 위치와 실행 결과

경로는 Java `zlink-stream-connector/src/main/java/systems/zlink/stream/connector`와 `zlink-http-client/src/main/java/systems/zlink/httpclient`를 기준으로 합니다.

| 항목 | 수정 위치 |
|---|---|
| G01 queue terminal | ZLinkStreamSendChain.java:33/:73; DefaultZLinkStreamConnector.java:418; PendingRequests.java:17/:76 |
| reply snapshot/실패 정리 | DefaultZLinkStreamConnector.java:317; PendingRequests.java:93(실패 close 정적근거) |
| 이름 구조 | ZLinkStreamWireProtocol.java:315; DefaultZLinkStreamConnector.java:721 |
| close 오류와 종료 경계 | ZLinkTcpTransportConnection.java:72; Lifecycle.java:180/:817, manual:140/server:166/receive:519; TCP rawcleanup:365 |
| HTTP one-shot/typed cleanup | ZLinkHttpRequestBuilder.java:158/:181/:197/:259 |
| assertion identity | ZLinkStreamAssert.java:50/:60/:93 |
| HTTP application 분류 | internal/HttpClientErrors.java:57; ProviderInputStream.java:51–52; ResponseBodyReader.java:46–47 |
| magic 값 명명 | RequestBuilder.java:28 FIRST_ERROR_STATUS; internal/RetryPolicy.java:25–27 |

최종 모듈 명령(WSL Java 디렉터리):

```sh
JAVA_HOME=/usr/lib/jvm/jdk-25.0.4.1+1 ./gradlew --no-daemon --max-workers=1 --continue -Pzlink.localPackageRoot=/home/hep7/.cache/zlink/packages/8da48e6102dec292/java :zlink-stream-connector:test :zlink-http-client:test -x :zlink-stream-connector:buildNodeStreamConnector
```

Node dependency build는 감독 승인으로 이미 검증된 같은 job의 Node 산출물을 사용했습니다. `.artifacts/node-wire-r3/java/module.log`의 `BUILD SUCCESSFUL in 1m 58s`, XML summary `zlink-stream-connector tests=183 failures=0 errors=0 skipped=0`, `zlink-http-client tests=60 failures=0 errors=0 skipped=0`가 최종 결과입니다. unit과 contract를 각 모듈 test task 전체로 한 번 실행했고 module-results에 원본 XML을 보존했습니다.

최종 format: WSL job root에서 JAVA_HOME과 PATH를 JDK25로 고정하고 `scripts/format/format.sh --check java`를 실행했습니다. `.artifacts/node-wire-r3/java/format.log`에 `== java`, 종료코드0이며 변경 대상/FAILED 출력이 없습니다. 전체 모듈 및 format에서 제품 실패가 없어 origin/main 대조 조건은 발생하지 않았습니다.

소유 계층: Framework frame sender·pending correlation·connection lifecycle·HTTP client owner입니다.
스펙 조항: stream32 §4.2/§5.2/§9, HTTP02 one-shot 종료 및 HTTP09 일반 실행 오류, 사용자 assertion 판정입니다.
교차언어 대조: 표의 네 구현을 정적 대조하고 공통 ASCII 공백·미작성 frame 취소·close 오류·원예외 기준을 맞추었습니다. 네 언어 동적 결과 최종 취합은 감독자의 report-r3.md가 소유합니다.
변경 분류: 위 승인된 구현은 B이며, Unicode 공백 집합과 native timeout subtype은 D로 보존했습니다.
