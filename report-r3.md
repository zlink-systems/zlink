# #1083 .NET 매직 값 리뷰 반영

## 발견과 수정

1. `ZLinkRuntimeMetrics`의 `CurrentTag`와 `PeakTag`는 tag 이름이 아니라 `state` tag 값이었다. 이름을 `StateTagValues.Current`와 `StateTagValues.Peak`으로 바로잡고, `current`, `peak`, `reserved`, `queued`, `in_use`, `running`, `paused`, `cumulative`를 하나의 상수 소유자에 모았다. 기존 검사가 metric 이름, instrument kind, unit, `state` key와 출력 label을 고정하고, 상수 값 일치 테스트가 여덟 wire 값을 확인한다.
   - 원인(수정 전): `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkRuntimeMetrics.cs:31-32,806-903`
   - 수정: 같은 파일 `:34-43,816-913`; 테스트 `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/RuntimeMetricsTests.cs:10-20`
   - 기존 테스트는 metric 이름, `state` key와 각 상태 label 조합을 고정한다(`RuntimeMetricsTests.cs:24-111,183-353`).

2. activation concurrency 기본 128과 descriptor pending capacity 기본 128은 같은 수치지만, 스펙은 별도 필드로 정의하고 관계를 정하지 않는다. C++·Java는 두 기본값을 별도 전달하고 Node는 activation concurrency를 pending capacity limit에 연결한다. 네 언어 동작이 달라 스펙 gap D로 분류하며 두 기본값을 통합하지 않았다.
   - 근거: `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md:439-440`은 `capacity` 투영과 `activationConcurrency`를 별도 descriptor field로 정의하지만, 둘 사이의 기본값 관계는 정하지 않는다.
   - .NET: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Configuration/ZLinkFrameworkRegistration.cs:388,458`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkServiceWireCodec.cs:2276-2280`; `framework/runtime/protocol/generated/dotnet/ServiceWireConstants.g.cs:21-22`
   - active capacity wire 기본값은 C++·Java·Node·.NET 모두 10000이다. `ZLinkSpotNodeRegistration.MaxActiveObjects`는 같은 생성 상수를 초기값으로 복사한 미사용 alias였고 production에서 읽히지 않았다. Codec이 `ActiveCapacityLimit`에 `NodeActiveCapacityDefault`를 직접 쓰므로 alias와 효과 없는 test initializer를 제거해 wire 기본값 owner 하나만 남겼다. `ZLinkObjectPlacementOptions.MaxActiveObjects`는 별도의 per-type placement 제한이다. 스펙은 Actor·Spot 전체 limit과 User·Instance Spot type limit의 기본값을 0(무제한)으로 정하며(`framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md:644-650`), 일반 object 전체 active 10,000 상한은 사용하지 않는다고 명시한다(`framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1740-1742`).
   - 원인(수정 전): `framework/languages/dotnet/src/Zlink.Framework/Runtime/Configuration/ZLinkFrameworkRegistration.cs:460-462`의 미사용 alias; `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/CapacityMonitoringProjectionTests.cs:49`의 효과 없는 initializer. wire value 소유자는 `framework/runtime/protocol/generated/dotnet/ServiceWireConstants.g.cs:21`이며 codec이 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkServiceWireCodec.cs:2276`에서 직접 참조한다.
   - 수정: `ZLinkFrameworkRegistration.cs:460-462` property와 `CapacityMonitoringProjectionTests.cs:49` initializer를 제거했다. `ZLinkObjectPlacementOptions.MaxActiveObjects` 사용은 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkRouteMeshRuntimeService.cs:562,571` 등 실제 per-type 경로에 남겼다.

3. `ZLinkLocationRows.cs`에서 이미 import한 `ZLinkSpotNodeRegistration`을 짧은 이름으로 사용하도록 고쳤다.
   - 원인(수정 전): `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkLocationRows.cs:122-127`에서 이미 import한 형식을 정규 이름으로 반복했다.
   - 수정: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkLocationRows.cs:120`

4. 요청 body의 form 및 multipart content-type 값을 이름 있는 상수로 옮겼다. wire 값은 바꾸지 않았다.
   - 원인(수정 전): `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:396,403`에 content-type literal이 있었다.
   - 수정: `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:396,403,435-436`

## 교차 언어 대조

| 결정 | C++ | Java | Node | 판정 |
|---|---|---|---|---|
| runtime metric `state` 값 | `framework/languages/cpp/framework/src/runtime/dispatch/host_capacity_runtime.hpp:200-247`; `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_monitoring.cpp:151-176` | `framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/ZLinkMicrometerMetricSink.java:158-267`; `framework/languages/java/zlink-framework-spring-boot-starter/src/test/java/systems/zlink/framework/spring/ZLinkMicrometerMetricSinkTest.java:158-272` | `framework/languages/node/packages/framework/src/runtime/diagnostics/runtime-metrics.ts:317-354` | .NET 및 세 구현이 같은 값 집합을 출력한다. |
| active capacity 기본값 | `framework/languages/cpp/framework/src/runtime/mesh/service_topology_registry.hpp:72` (`10000`) | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:7044` (`10_000`) | `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:133,193-194` (`10_000`) | .NET 생성 상수 `framework/runtime/protocol/generated/dotnet/ServiceWireConstants.g.cs:21`과 모두 10000이다. 미사용 .NET alias는 제거하고 생성 상수를 단일 owner로 유지했다. |
| pending capacity와 activation concurrency 기본값 | `framework/languages/cpp/framework/src/runtime/mesh/service_topology_registry.hpp:72-73`; `framework/languages/cpp/framework/include/zlink/framework/contracts/locations/rows.hpp:43-46`에서 별도 기본값 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:7021-7044`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/mesh/MeshNodeRegistration.java:89`에서 별도 기본값 | `framework/languages/node/packages/framework/src/contracts/Configuration/InternalDefaults.ts:10`; `framework/languages/node/packages/framework/src/runtime/spots/spot-node-runtime-manager.ts:340-341`에서 activation 기본값을 pending limit에 연결; `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:133,193-194`에서 descriptor에 반영 | 동작이 달라 스펙 gap D. |
| form/multipart content-type 값 | `framework/languages/cpp/http-client/src/client.cpp:512,534`에 literal | `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBodyEncoder.java:46,52`에 literal | `framework/languages/node/packages/http-client/src/runtime/text.ts:16-21`의 `HttpContentType`을 `request-builder.ts:299,305`에서 사용 | .NET 상수화만 수행; 값과 조립 결과는 동일하게 유지. |

metric 규칙 근거는 `framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:111-124,396`이고, HTTP request body content-type 근거는 `framework/doc/framework/common/spec/http-client/03-request-builder.ko.md:38-39`이다.

## 판정 위치와 분류

- metric 상태 label을 만드는 measurement 위치는 수정 전 13곳, 수정 후 13곳이다. pressure 상태를 `running` 또는 `paused`로 고르는 조건부 판정은 1곳에서 1곳으로 유지했다.
- form 및 multipart 선택 분기는 수정 전 2곳, 수정 후 2곳이다. 새 분기·상태·flag·timer·retry는 추가하지 않았다.
- pending capacity 기본값과 activation concurrency 기본값은 스펙이 관계를 정하지 않으므로 각각 1개 선언·전달 소유자를 유지했다(수정 전 1+1, 수정 후 1+1, D).
- active capacity 기본값 선언은 수정 전 2곳(`NodeActiveCapacityDefault`와 registration alias), 수정 후 1곳(`NodeActiveCapacityDefault`)이다. 이 중 wire codec이 사용하는 기본값 결정 위치는 1곳에서 1곳으로 유지했다. per-type `MaxActiveObjects` 판정 위치는 유지했다.
- 변경 분류: .NET metric 값 이름·소유권, HTTP content-type literal, 미사용 active capacity alias는 기존 코드 결함 B를 수정했다. pending capacity와 activation concurrency의 관계는 스펙이 정하지 않아 D로 보고하며 코드를 바꾸지 않았다.

소유 계층: metric projection은 .NET Framework diagnostics가 소유하고 request body content-type 조립은 .NET HttpClient builder가 소유한다. active/pending capacity 기본 wire 값은 생성된 service-wire 상수의 소유로 남고 activation admission limit은 별도 registration 소유다.
스펙 조항: metric 계약은 `framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:111-124,396`, HTTP content-type은 `framework/doc/framework/common/spec/http-client/03-request-builder.ko.md:38-39`, descriptor `capacity`와 `activationConcurrency` field 구분은 `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md:439-440`이다.
교차언어 대조 결과: metric의 여덟 label value와 HTTP content-type wire 값은 나머지 언어 구현과 일치한다. pending capacity와 activation concurrency의 기본값 관계는 Node와 C++·Java 구현이 달라 D로 남겼다.
변경 분류: metric 표기 및 값 소유권, HTTP content-type 상수화, 미사용 active capacity alias 제거는 B(기존 결함 수정)이며, pending/activation 기본값 관계는 D(스펙 gap, 미수정)이다.

## Supervisor guide §2 검토

| 기준 | 판정 |
|---|---|
| 스펙 gap | pending capacity와 activation concurrency 관계만 D로 남겼다. 나머지 값은 기존 스펙 계약을 유지한다. |
| 불필요한 규칙 | 상태 값과 content-type 값의 중복 literal, 미사용 active capacity alias와 test initializer를 제거했다. 새 규칙은 없다. |
| 제어 분산 | metric measurement 선택 위치와 HTTP 선택 분기 수가 늘지 않았다. |
| 동기화 | lock, queue, 대기 또는 execution gate 우회 변경이 없다. |
| hot path | 상수 참조 외 동작·할당·lock을 추가하지 않았다. |
| 리팩토링 잔여 | 건드린 두 모듈과 호출 경로에서 범위 안의 발견은 반영했다. |
| 매직 값 | metric state 값 8개와 form/multipart content-type literal을 각 소유자에 모았다. |
| 계층 소유 | metric 표기와 HTTP 요청 content-type 조립은 기존 .NET 소유 모듈에 남겼다. |

리팩토링 점검: 성능 0건·POSDDD 0건·불필요 코드 4건 발견, 고친 항목은 metric state 값 소유권, HTTP content-type 상수화, location row의 정규 이름 중복 제거, 미사용 registration alias 및 test initializer 제거이며, 넘긴 항목은 activation concurrency와 pending capacity 관계 D(`01-location-runtime.ko.md:439-440`)이다.

## 검증

| 검증 | 로그 | 결과 |
|---|---|---|
| 수정 전 focused test | `.artifacts/1083-magic-r3/pre-fix-focused.log` | 의도한 컴파일 실패: `StateTagValues` 미정의 CS0117, 8 assert가 실행 전 컴파일에서 실패. |
| 초기 수정 후 focused test | `.artifacts/1083-magic-r3/post-fix-focused.log` | cache의 `Systems.Zlink.dll` 경로에서 CS0006. test 미실행. |
| origin/main focused 대조 | `.artifacts/1083-magic-r3/origin-main-focused.log` | 23 passed, 0 failed, 0 skipped. CS0006 재현 안 됨. |
| baseline 대조 후 수정 branch focused | `.artifacts/1083-magic-r3/post-fix-focused-after-baseline.log` | 중간 정리에서 property를 initializer보다 먼저 제거해 CS0117이 발생했다. origin/main의 같은 focused 명령은 위 baseline 로그에서 통과했고, 해당 snapshot의 fixture에는 이 initializer가 없었다. property를 임시 복구한 뒤 focused test를 통과시킨 다음, 최종 snapshot에서는 미사용 property와 no-op initializer를 함께 제거했다. 최종 snapshot의 unit 결과는 아래 본체 suite 행에 기록했다. |
| property 복구 후 focused test | `.artifacts/1083-magic-r3/post-fix-focused-after-registration-restore.log` | 24 passed, 0 failed, 0 skipped. |
| 최종 snapshot Framework.UnitTests join ingress 단독 suite | `.artifacts/1083-magic-r3/framework-unit-join-final.log` | 21 passed, 0 failed, 0 skipped; duration 23s. |
| Zlink.HttpClient.UnitTests | `.artifacts/1083-magic-r3/http-unit-final.log` | 70 passed, 0 failed, 0 skipped; duration 2s. |
| 최종 snapshot Framework.UnitTests 본체 | `.artifacts/1083-magic-r3/framework-unit-main-final.log` | 2,365 passed, 1 failed, 0 skipped. `StreamSessionForcedCleanupTests.Stream_node_shutdown_upper_bound_does_not_wait_for_terminal_cancellation_callback`가 `StreamSessionForcedCleanupTests.cs:1294`에서 cancellation callback 시작 대기 `TimeoutException`(10 s)으로 실패했다. 같은 실행 중 load가 22.51/20까지 올랐고 다른 worktree testhost도 실행 중이었다. 최초 실행에는 `.flow`가 없었다. |
| Flow 계측 진단 재실행(임시 test instrumentation) | `.artifacts/1083-magic-r3/branch-unit-main-diagnostic.log`, `.artifacts/1083-magic-r3/branch-unit-main-diagnostic.flow` | 2,366 passed, 0 failed, 0 skipped. trace에는 `TerminalCancellationMessage`의 `received → dispatched` 성공만 있고 shutdown/callback 단계는 flow event로 기록되지 않았다. 최초 실패를 통과로 대체 판정하지 않는다. |
| `origin/main` 같은 unit-main 명령 대조(SHA `357fac766eca0f53d4da57e9e0eae8a2c20ec840`) | `.artifacts/1083-magic-r3/origin-current-unit-main-diagnostic.log`, `.artifacts/1083-magic-r3/origin-current-unit-main-diagnostic.flow` | 2,362 passed, 2 failed, 0 skipped. target cancellation test는 통과했고 flow는 같은 `received → dispatched` 성공이다. 별도 실패는 문서 목록 회귀 검사 2건: expected list에서 빠진 `02-tour.ko.md`가 actual 목록에 존재한다. |
| Zlink.Framework.ContractTests | `.artifacts/1083-magic-r3/framework-contract-final.log` | 77 passed, 0 failed, 0 skipped. |
| Systems.Zlink.Stream.Connector.Tests | `.artifacts/1083-magic-r3/stream-connector-final.log` | 253 passed, 0 failed, 0 skipped. |
| Zlink.Framework.Locations.Redis.Tests | `.artifacts/1083-magic-r3/redis-tests-final.log` | 47 passed, 0 failed, 0 skipped. |
| scripts/format/format.sh --check dotnet (첫 실행) | `.artifacts/1083-magic-r3/format-final.log` | 1,152 files 검사 후 실패. 이 변경 파일의 mixed EOL 2건과 CSharpier 줄 접기 1건을 지적했다. |
| scripts/format/format.sh --check dotnet (수정 후) | `.artifacts/1083-magic-r3/format-final-rerun.log` | 1,152 files 검사, 오류 없이 통과(264,844 ms). 변경한 두 파일은 LF로 정규화했다. |

초기 branch 준비 시 fetch 및 origin/main merge를 완료했다. 이후 병렬 작업으로 origin/main ref가 `49f8b7f1fe`까지 전진했으며 해당 후속 변경은 이 worktree에 추가하지 않았다. `.artifacts/1083-magic-r3/origin-main-focused.log`는 이 snapshot에서 실행했다.
