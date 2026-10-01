# .NET node-wire 3단계

소유 계층은 Framework connector의 logical frame queue·이름 구조·종료 lifecycle과 HTTP request 실행·cleanup·오류 분류입니다. Core/binding 정책을 다시 구현하지 않았습니다. 스펙 파일과 public API를 수정하지 않았고 commit·push·branch 전환을 수행하지 않았습니다.

## 발견과 결정 소유

경로는 `framework/languages/dotnet/` 기준입니다.

| 항목 | 원인과 수정 위치 | 소유 조항·분류 | 판정 위치 전→후 |
|---|---|---|---|
| G01 미작성 request | `src/Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamConnector.cs:588` linked request token, `Runtime/ZlinkStreamOneWaySubmitQueue.cs:149` drain 취소 폐기. timeout/cancel 양쪽 회귀 통과, runtime 수정 없음 | stream-connector/32 §5.2:320, 기존 준수 | 1→1 |
| R06 공백 이름 | `Runtime/ZlinkStreamConnector.cs:639` 이름 구조 owner의 IsNullOrEmpty를 IsNullOrWhiteSpace로 수정. header codec도 기존 owner에 위임 | §4.2:120, B | 1→1 |
| R12 close 오류 | 원본 `Runtime/ZlinkStreamConnectorLifecycle.cs:198/651` closeFailure 저장·rethrow. 최종 `:763` CloseConnectionAsync가 모든 다섯 close 호출의 오류를 Disconnected event로 전달. closeException/closeFailure 제거, reason/reconnect 유지 | §9:675, B; 기존 시험 개정 A | 2→1 |
| R12 one-shot HTTP | 원본 `src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:184/208` finally Dispose가 요청 실패를 대체. 기존 cleanup 역할을 `:174` ExecuteOneShotAsync로 이동하여 요청 실패 우선, 성공이면 close 실패. typed status/decode도 `:257` 실제 async core에 포함 | http-client/02-client-builder.ko.md:12, B | 2→1 |
| assert action 예외 | 원본 Assert.cs:102~105 일반 IO/Argument 및 inner cause 분류. `Contracts/ZlinkStreamAssert.cs:90` direct coded/direct TimeoutException만 기존 의미로 사용하고 나머지는 원 객체 throw | 사용자 감독자 판정, B; 구 기대 개정 A | 분류 owner 1→1, inner 추정 경로 삭제 |
| HTTP 단계별 오류 | 원본 RetryPolicy.cs:64~94 transport type만 분류, 일반 실행은 미분류. `src/Zlink.HttpClient/Runtime/HttpFailureMapper.cs:13` 단일 분류 owner로 이동. performer `RequestPerformer.cs:63` Transport, provider `ProviderReadStream.cs:82` Application, sink `ResponseBodyReader.cs:45` Application, response body read `:51/:82` Transport 전달 | http-client/09-error-model.ko.md:15, B | 일반 분류 owner 1→1 |
| HTTP 재시도 | 원본 RetryPolicy의 coded RetryAdvice 분기와 공통 retry budget 분기. 최종 `RetryPolicy.cs:69` Kind Unavailable/DeadlineExceeded 및 streaming/budget만 판정. native OCE는 mapper가 원 객체를 넘기고 caller token을 아는 `:51/:55`가 취소/timeout 구분 | http-client/06-redirect-retry-cookie.ko.md:30, 09-error-model.ko.md:19, B | retry 수락 위치 2→1, OCE owner 1→1 |

CloseConnectionAsync는 transport close 오류를 event로 전달하며 session cancellation·pending completion 등 다른 terminal cleanup 오류의 기존 수집 경로를 보존합니다. 해당 오류 배열을 transport close 오류 보상 상태로 확장하지 않았습니다. lifecycle 종료 state/reason은 기존 gate 안에서 close/Terminate 수락 시 먼저 확정됩니다. 신규 회귀에서 실제 error callback 순간 Closed/ClientClose를 관측합니다.

HttpFailureMapper는 direct coded 오류를 원 객체로 유지합니다. Transport 단계의 native HttpRequestException이 직접 감싼 coded 오류만 복구하며, Application 단계의 generic wrapper는 외부 객체를 InternalFailure의 원인으로 유지합니다. native Transport OCE를 일찍 DeadlineExceeded로 바꾸지 않아 caller cancellation이 retry되지 않습니다. stage는 인수 enum이며 runtime field·flag·DTO를 추가하지 않았습니다.

## 교차언어 대조

아래 경로는 `framework/languages/` 기준입니다. 각 파일의 같은 결정 소유 경계를 확인했습니다. 타 언어 최종 실행 결과는 감독자의 root 보고에서 취합합니다. 동일 failure scenario를 함께 실행하는 네 언어 공통 contract는 없으며, 각 언어의 독립 unit·public contract 결과를 동적 동일성 증거로 확대하지 않습니다.

| 결정 | Node | Java | C++ | .NET |
|---|---|---|---|---|
| 미작성 terminal request | `node/packages/stream-connector/src/Runtime/ZlinkStreamFrameSender.ts:40` queue.delete | `java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamSendChain.java:72` queue 취소 owner | `cpp/connector/core/src/runtime/calls/zlink_stream_calls.cpp:652` write_id 미작성 삭제 | OneWaySubmitQueue.cs:149 linked token 폐기 |
| 공백 이름 구조 | `node/packages/stream-wire/src/index.ts:669` trim 구조 validator | `java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamWireProtocol.java:316` 구조 owner | `cpp/common/include/zlink/detail/stream_packet_name.hpp:18` ASCII whitespace 구조 validator | Connector.cs:639 IsNullOrWhiteSpace |
| close 오류/state | `node/packages/stream-connector/src/Runtime/ZlinkStreamConnectorLifecycle.ts:532` 종료state→close error→disconnected owner | `java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamConnectionLifecycle.java:180/:817` closeFailure→기존 상태 확정 후 event | `cpp/connector/core/src/runtime/connector_runtime.cpp:498` publish_close_error owner, calls.cpp:1388 완료state 확정 | Lifecycle.cs:763 owner, 기존 gate 상태 확정 유지 |
| HTTP close 우선순위 | `node/packages/http-client/src/request-builder.ts:94/:225` 실행과 typed decode | `java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:158/:259` 실행·release owner | `cpp/http-client/include/zlink/http_client/contracts/client.hpp:114` 값 소유; close 오류 결과 API 부재 N/A; eager one-shot 수명 누락 B의 구현 승인 미정 | RequestBuilder.cs:174/:257 실행과 typed decode |
| assert 일반 action 예외 | `node/packages/stream-connector/src/Runtime/ZlinkStreamAssertions.ts:30` 원 객체 전파 | `java/zlink-stream-connector/src/main/java/systems/zlink/stream/connector/ZLinkStreamAssert.java:20` 원 객체 전파 | `cpp/connector/core/include/zlink/stream_connector/contracts/zlink_stream_assert.hpp:51` 그대로 throw | Assert.cs:90 direct coded/timeout 외 전파 |
| Application 오류 | `node/packages/http-client/src/runtime/request-performer.ts:124/:206`, errors owner | `java/zlink-http-client/src/main/java/systems/zlink/httpclient/internal/HttpClientErrors.java:57`, provider :51/sink :46 단계 | `cpp/http-client/src/runtime/request_performer.cpp:34/:338/:395` callback owner, coded 재전파 | Mapper.cs:13, provider :82/sink :45 단계 |

Unicode 공백 집합은 trim/isBlank/IsNullOrWhiteSpace/ASCII 사이에서 차이가 있으며 공통 §4.2:120이 상세 집합을 정하지 않았습니다(D). ASCII 공백 거부만 스펙과 대조하여 적용했고 집합을 임의로 통일하지 않았습니다.

## 검증과 남은 제한

빌드·시험·format은 WSL의 이 job 사본에서 단일 worker로 실행했습니다. 로그는 Windows에서 열 수 있는 `.artifacts/node-wire-r3/dotnet/`에 보존합니다. `dotnet test <csproj> --no-restore -m:1 -- RunConfiguration.MaxCpuCount=1`을 해당 모듈 전체 명령으로 사용했습니다. 해당 모듈의 public contract 시험도 같은 csproj에 포함되어 있습니다. 서버 전용 Framework StreamContracts는 직접 호출 경로가 아니므로 실행 범위를 넓히지 않았습니다.

| 검증 | 로그 | 실제 요약 |
|---|---|---|
| 수정 전 stream R3 | dotnet-r3-stream-before-valid.log | 실패 4, 통과 2, 전체 6. G01 timeout/cancel 2건은 모두 통과 |
| 수정 전 HTTP R3 | dotnet-r3-http-before-valid.log | 실패 9, 통과 2, 전체 11 |
| 수정 후 stream R3 | dotnet-r3-stream-after-focused.log | 통과 6/6 |
| 수정 후 HTTP R3 | dotnet-r3-http-after-focused.log | 통과 11/11 |
| close 5개 계약+R3 | dotnet-r3-stream-close-focused.log | 통과 11/11 |
| 최초 stream 전체 | dotnet-r3-module-Systems.Zlink.Stream.Connector.Tests.log | 실패 1, 통과 258, 전체 259 |
| HTTP 전체 | dotnet-r3-module-Zlink.HttpClient.UnitTests.log | 통과 81/81 |
| origin/main 동일 stream 전체 | dotnet-r3-origin-main-stream-module.log | 통과 253/253 |
| 계약 개정 후 stream 전체 | dotnet-r3-final-stream-module.log | 통과 259/259 |
| 동작 불변 buffer 상수 변경 후 HTTP focused | dotnet-r3-final-http-focused.log | 통과 11/11 |
| 최초 format check dotnet | dotnet-r3-format-check.log | Checked 1153 files in 402460ms, rc0 |
| 최종 format check dotnet | dotnet-r3-final-format-check.log | Checked 1153 files in 403336ms, rc0 |

최초 stream 전체 실패는 TestHelperTests.cs:220의 일반 wrapper(inner HttpRequestException) 분류 기대였습니다. :231의 wrapper(inner Timeout) timeout 성공 기대도 같은 감독 판정에 반합니다. 감독 승인 A로 두 시험을 원 wrapper identity 전파로 강화했고 기존 direct timeout·coded·caller cancel 양성 시험을 유지합니다. 최초 실패 로그를 보존했습니다. origin/main 같은 전체 명령은 253건 모두 통과했고, 계약 개정 후 해당 stream 전체 259건이 통과했습니다. HTTP 전체 81건 통과 snapshot 이후 추가한 ReadBufferSize는 16384 두 literal을 같은 값의 소유 상수로 이동한 동작 불변 변경이며 incremental compile과 HTTP R3 focused 11건·최종 format check로 확인했습니다. 같은 HTTP 전체를 반복하지 않았습니다.

초기 새 stream 회귀 파일이 explicit Compile 목록에 없어 filter가 매칭되지 않았고 이후 byte memory 표현·접근 불가능한 예외 ctor에서 준비 compile 실패가 있었습니다. 이들은 결함 재현 또는 통과 결과로 세지 않았습니다. 표의 valid 로그만 실제 원본 결함 재현입니다.

RetryAdvice와 Kind가 모순되는 내부 예외 fixture는 HTTP test assembly가 FrameworkContracts friend가 아니어서 만들 수 없습니다. 새로운 friend/test 우회/reflection은 추가하지 않았습니다. 최종 retry 조건은 Kind만 참조하며 RetryAdvice를 읽지 않는 정적 근거로 제한을 보고합니다. native coded·application wrapper 원인·caller cancellation identity 및 재시도 0회 회귀는 실행으로 확인했습니다.

보호 문서의 구 시험 설명 `framework/doc/framework/common/spec/stream-connector/languages/dotnet/03-stream-connector.ko.md:435` 및 en:510은 repeated close/dispose에 같은 실패를 관찰한다는 표현이 남아 common §9:675와 다릅니다. 동작 소유는 common §9이며 문서는 수정하지 않았습니다.

## 마무리 리팩토링과 비용

HTTP reusable client는 ValueTask를 바로 반환하며 operation delegate/closure를 만들지 않습니다. client.Dispose method group과 operation.AsTask는 one-shot owner에서만 생깁니다. 원본 raw/download async wrapper 1개는 최종에도 1개이며 typed 원본 ExecuteTypedAsync+AsyncRaw 2개는 최종 ExecuteTypedAsync+PerformTypedAsync 2개입니다. 실제 async core가 sync 실행/decode 예외를 failed awaitable로 포착하여 cleanup 누락을 막습니다. one-shot AsTask의 추가 할당 가능성은 희소 cleanup 비용이며 실측 성능 개선을 주장하지 않습니다. BodyReader는 기존 async 메서드에 catch 경계만 두어 추가 async helper·lock·복사·native 호출을 만들지 않았습니다. trace 코드도 추가하지 않았습니다.

Backoff 1000/50/5는 RetryPolicy의 named const 하나씩으로 이동했습니다. typed status 400은 RequestBuilder owner 상수로 이동했습니다. BodyReader의 buffer 16384 두 사본은 `ResponseBodyReader.cs:15`의 ReadBufferSize 한 상수로 합쳤습니다. 모두 값과 동작은 그대로이며 판정 수는 증가하지 않습니다. 범위 밖 public default 사본은 `src/Zlink.HttpClient/ZLinkHttpClientBuilder.cs:21/:25`, `Runtime/HttpClientOptions.cs:17/:19`의 timeout3000/bodylimit16MiB이며 이번 오류·종료 범위를 넘어 수정하지 않았습니다.

| supervisor-guide §2 기준 | 결과 |
|---|---|
| 스펙 gap | Unicode 집합 D와 보호 문서 설명 불일치만 보고, 임의 정책 변경 없음 |
| 불필요한 규칙 | 새 state·flag·timer·retry·option 없음, closeFailure/closeException 상태 삭제 |
| 제어 분산 | queue1→1, 이름1→1, close2→1, HTTPcleanup2→1, 일반mapper1→1, retry2→1 |
| 동기화 | 기존 lifecycle gate와 callback queue 사용, execution gate 우회 lock/대기 추가 없음 |
| hot path | reusable ValueTask·async wrapper 수 유지, one-shot만 native outcome Task, 응답 읽기 새 async helper 없음 |
| 리팩토링 잔여 | close 상태·분류 사본·cleanup 중복·inner 추정을 같은 작업에서 삭제 |
| 매직 값 | backoff/HTTPstatus/buffer 상수 owner로 수렴; public defaults 중복은 범위 밖 file:line 보고 |
| 계층 소유 | Framework logical request와 종료 event만 수정, Core/binding의 선택·reconnect 정책 재구현 없음 |

불필요 코드 발견 7건은 inner 추정 1건, close 오류 상태 중복 1건, backoff/status/buffer 상수 소유 3건을 수정한 5건과 범위 밖 timeout/body-limit 기본값 중복 2건을 넘긴 합계입니다.

리팩토링 점검: 성능 1건·POSDDD 3건·불필요 코드 7건 발견, operation closure 제거·close/HTTPcleanup/오류분류 owner 수렴·inner 추정과 close 상태 삭제·backoff/status/buffer 상수화를 수정, 넘긴 항목은 public defaults 사본(`ZLinkHttpClientBuilder.cs:21/:25`, `HttpClientOptions.cs:17/:19`)과 Unicode 상세 정책 D입니다.
