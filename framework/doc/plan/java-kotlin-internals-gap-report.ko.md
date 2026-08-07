# Java/Kotlin Framework 스펙 구현 Gap 리포트

## 1. 결론

현재 Java/Kotlin Framework는 공통 공개 spec, 언어별 exact interface와 common internals를 모두 충족한다고
판정할 수 없다. 단순한 API 누락이 아니라 package 경계, RouteMesh wire 계약, JSON 상호 운용성,
session 교체, relocation backlog, startup 상태 공개, 완료 확정 구조와 payload 소유권에서 실제
동작 차이를 확인했다. 재검토 결과 기존 11개 항목 가운데 근거가 없어 철회할 항목은 없었다.
다만 progress 항목은 실제 동작 위반까지 입증한 것처럼 판정해 강도가 과했으므로 `GAP`에서
`PARTIAL`로 낮췄다. 첫 재검토에서 세 항목을 추가했고, Sol high 독립 재검토에서 네 항목을 더
확인했다. 이번 message-size 재검토와 Ready Instance owner loss 계약 개정으로 현재 항목은 20개이며
session과 Message Follow 항목의 오류 의미도 바로잡았다.

가장 먼저 해결해야 할 문제는 다음과 같다.

1. 현재 cache에서는 Java/Kotlin API snapshot이 통과하지만 고정 bindings package의 clean consumer
   재현성을 아직 확인하지 않았다.
2. Java와 Kotlin exact interface가 같은 Java public type의 method 수를 서로 다르게 규정한다.
3. RouteMesh에 없어야 할 `maxMessageSize` public/runtime surface와 descriptor의 고정 4 MiB
   field가 남아 있다.
4. `framework-json-v1` golden fixture의 strict decoding 규칙을 generic JSON serializer가
   구현하지 않으며, fixture를 직접 소비하는 JVM test도 없다.
5. session 교체가 이전 owner의 cleanup 확인을 기다리지 않고 새 binding을 즉시 확정한다.
6. relocation 차단 이후의 pre-commit backlog에 1,024건/16 MiB 상한이 적용되지 않는다.
7. Java와 Kotlin STREAM session send call에 exact interface가 요구하는 per-call timeout이 없다.
8. object descriptor가 peer 연결과 Spot 준비보다 먼저 `SERVING`을 공개한다.
9. diagnostics level enum의 이름과 개수가 exact interface와 다르다.
10. 설정한 mailbox byte budget이 raw runtime에 전달되지 않고 owner별 회계도 적용되지 않는다.
11. Location exact lookup이 `UNAVAILABLE` 상태와 Store failure error kind를 구현하지 않는다.
12. 정상 Spot·Actor message마다 relocation frozen record를 미리 만들고 반복 복사한다.
13. Diagnostics `OFF`에서도 dispatch error event DTO를 먼저 할당한다.
14. STREAM 상한 초과를 `EMSGSIZE`로 기록하지 않고 segmented oversize 연결 종료를 보장하지 않는다.
15. 만료된 `Ready` owner authority를 Missing과 구분하지 않아 Instance cold activation 경로에 진입한다.

전체 audit은 2026-08-07의 `main` `425b9c2a8272` 구현을 기준으로 작성했다. 복구·머지 뒤 현재
`main` `e2119caeda`까지 Java/Kotlin Framework production source 변경이 없음을 확인했으므로 기존
구현 판정을 유지한다.

## 병렬 구현 세션 주의 사항

이 보고서는 다른 언어의 gap 작업과 동시에 진행할 수 있지만, 모든 작업은 현재 `main` checkout에서
수행한다. 별도 `git worktree`나 작업용 branch를 만들지 않으며, 작업 시작 시점의 `main` commit SHA를
작업 기록에 남긴다.

- 이 세션은 해당 언어의 production source·test·package 자료와 이 gap 문서만 수정한다. 다른 언어
  디렉터리, 다른 언어 gap 문서와 공통 spec·internals는 수정하지 않는다.
- `framework/runtime/protocol/`의 schema·generated 파일, cross-language fixture, 공통 검증 script처럼
  여러 언어가 함께 소비하는 파일은 통합 담당자 한 명만 수정한다. 변경이 필요하면 이 문서에 요구사항과
  예상 wire/API 영향을 기록하고 공용 선행 commit을 요청한다.
- 다른 세션의 변경을 원복하거나 포맷하지 않는다. Stage와 commit은 명시적인 경로 목록으로 제한하고
  `git add -A`를 사용하지 않는다.
- Gap 종결은 source 수정만으로 판단하지 않는다. Owner-layer regression, public API/exact snapshot,
  package 또는 clean-consumer, 관련 process E2E 증거를 각각 기록하고 통과한 항목만 종결한다.
- 언어별 작업이 `main`에 반영된 뒤 통합 담당자가 cross-language contract, service-wire fixture, 전체 문서
  검사와 process E2E를 다시 실행한다. 개별 성공을 전체 종결로 승격하지 않는다.

### 구현 중 리팩터링·checkpoint 규칙

Gap 하나 또는 서로 강하게 연결된 작은 작업 묶음의 동작과 회귀 test가 통과하면 다음 Gap으로 넘어가기
전에 리팩터링 checkpoint를 둔다. 마지막에 한꺼번에 정리하지 않는다.

- Production code는 POSD 관점에서 deep module과 information hiding을 강화하고, 의미 없이 인자를 전달하는
  pass-through 계층, 호출 순서에 의존하는 temporal decomposition과 중복 helper를 제거한다. DDD 관점에서는
  lifecycle·ownership·state transition·terminal error invariant를 해당 domain owner가 책임하게 정리한다.
- 같은 checkpoint에서 unit test도 POSD/DDD 관점으로 리팩터링한다. 반복 setup은 의도를 드러내는 fixture나
  builder 안에 숨기고, test 이름과 helper는 domain 용어와 observable behavior를 표현하게 한다. Production
  내부 구조를 그대로 복제하거나 실행 순서와 private 구현에 결합된 test는 제거하거나 계약 중심으로 바꾼다.
- 리팩터링 뒤 dead code, 사용하지 않는 wrapper·alias·fixture·dependency를 제거하고, hot path의 불필요한
  allocation·copy와 lock·queue contention도 함께 점검한다. 동작 변경이 있으면 owner-layer regression을
  먼저 추가하고 관련 unit test를 다시 실행한다.
- 관련 test가 통과한 의미 있는 checkpoint마다 해당 언어 경로와 이 문서만 path-limited staging하여
  commit하고 `main`에 push한다. Commit에는 닫은 Gap ID와 실행한 test를 남기고, 검증되지 않은 변경이나
  다른 언어의 변경을 섞지 않는다. Push한 commit SHA와 gate 결과를 이 문서의 해당 항목에 기록한 뒤
  다음 작업으로 진행한다.

## 2. 검토 범위와 판정 방법

공개 계약 기준은 다음 문서다.

- `framework/doc/framework/common/spec/`
- `framework/doc/framework/common/spec/server/languages/java/`
- `framework/doc/framework/common/spec/server/languages/kotlin/`

내부 구조 기준은 다음 영역이다.

- `framework/doc/framework/common/internals/01`부터 `12`까지

Public API와 사용자에게 보이는 동작은 정식 spec과 exact interface만 계약 근거로 사용한다. Internals는
그 계약을 구현하는 상태 표현, component 책임과 불변 조건의 차이를 판정하는 기준이며 공개 동작을
추가하거나 바꾸지 않는다. Package·process 실행이 필요한 항목은 source gap과 별도의 검증 증거로
관리한다.

구현은 `zlink-framework-core`, `zlink-framework-kotlin`의 public surface, runtime call path,
관련 unit test와 E2E를 확인했다. 이름이 같은 class나 method의 존재만 확인하지 않고, 설정값이
실제 socket과 wire descriptor에 전달되는지, 경쟁 경로가 하나의 terminal 결과로 모이는지,
queue 상한이 초과 시점에 적용되는지, payload가 hot path에서 다시 복사되는지를 추적했다.

판정은 다음 뜻으로 사용한다.

- **GAP**: 스펙과 다른 동작 또는 public surface를 source에서 직접 확인했다.
- **PARTIAL**: 핵심 구성은 있으나 스펙이 요구하는 구조나 실패 의미가 일부 빠져 있다.
- **UNVERIFIED**: source만으로 충분하지 않고 process 또는 package 검증이 필요하지만 아직 실행하지 못했다.

### 2.1 재검토에서 바로잡은 내용

첫 보고서의 internals 문서별 표는 문서 번호와 제목을 잘못 연결했다. `06`은 routing과 cache,
`08`은 object lifecycle, `10`은 liveness와 state다. 별도의 `08 backpressure` 문서는 없다. 아래 표를
실제 파일명에 맞춰 고쳤다.

또한 executor가 객체마다 존재한다는 사실만으로 progress 계약 위반을 확정했던 판정은 강했다.
Internals의 최종 관찰 기준은 자원 개수가 아니라 application 작업이 양보한 채 모두 대기할 때
infrastructure 작업이 진행하는지다 (`common/internals/03-progress-isolation.ko.md:117-121`). 따라서
`JVM-PROGRESS-001`은 구조상 차이가 확인된 `PARTIAL`로 낮추고 process 검증 전에는 실제 progress
failure로 단정하지 않는다.

Sol high 독립 reviewer에게 기존 14개 finding의 유지·수정·철회 여부와 누락 gap을 별도로 검토하게
했다. Reviewer가 제안한 항목은 그대로 반영하지 않고 common internals와 exact interface, 현재
working tree의 source sink를 다시 대조했다. 그 결과 철회할 기존 항목은 없었고, relocation error
판정 상향, progress 근거 축소와 네 개의 새 gap을 반영했다.

## 3. 우선순위별 Gap

### P0. Build와 exact interface

#### JVM-BUILD-001 — 고정된 bindings package의 provenance와 clean consumer가 확인되지 않았다

**판정: UNVERIFIED**

Framework는 `systems.zlink:zlink:0.10.1`을 사용하도록 고정한다
(`framework/languages/java/gradle/libs.versions.toml:1-5`). Raw MeshNode는
`RouterSocket.disconnectTransportPair(long, long)`을 호출한다
(`ZLinkJavaRawMeshNode.java:878-885`). 현재 local Maven artifact의 `RouterSocket`에는 이 public method가
있으며, JAR SHA-256은 `7182a178da52e2402aba187573bfe56d5fbb60215fa34202526dc4d02cb6c0be`다.

Commit `425b9c2a8272`에서 Java와 Kotlin용 `./scripts/verify_api_snapshot.sh`를 각각 다시 실행한 결과
두 명령 모두 compile과 snapshot 비교를 통과했다. Java snapshot은 2,912줄과 SHA-256
`b78f715d054f95f2b69c070938e8d36f5941ec90585ddc459f9815fbe0debb3c`, Kotlin snapshot은 3,421줄과
SHA-256 `2b1476e8e1c853c5a1fda3f356f0368cbeb388a5468b9f3882e91c1627e64c01`이었다.

다만 이 artifact를 만든 candidate manifest와 bindings source revision을 대조하지 않았고, 새 Gradle
cache와 독립 consumer에서도 같은 package provenance와 compile 결과를 확인하지 않았다. 현재 local
package의 snapshot 통과만으로 배포 package 재현성까지 완료로 판정하지 않는다.

**완료 조건**: clean Gradle cache에서도 Java/Kotlin API snapshot과 package consumer build가 같은
고정 version과 package provenance로 통과해야 한다.

#### JVM-CONTRACT-001 — Java와 Kotlin exact interface가 같은 public type을 다르게 규정한다

**판정: GAP (계약 충돌)**

Java exact interface의 `ZLinkRouteMeshRuntimeOptions` snapshot은 `channel(String, String)`,
`mesh(String)`, `channel(String)` 세 method만 포함한다
(`server/languages/java/interfaces/channel-messaging.ko.md:248-252`). Kotlin exact interface는
같은 Java type을 직접 사용하며 `meshNode(String)`까지 네 method가 있어야 한다고 명시한다
(`server/languages/kotlin/interfaces/channel-messaging.ko.md:162-167`). 구현에는 Kotlin 설명과
같이 `meshNode(String)`이 존재한다
(`ZLinkRouteMeshRuntimeOptions.java:3-9`).

하나의 Java class file이 두 snapshot을 동시에 만족할 수 없다. 구현 변경 전에 공통 계약에서
`meshNode(String)`의 존속 여부를 확정하고 Java와 Kotlin exact interface를 같은 결론으로
갱신해야 한다.

#### JVM-CONTRACT-002 — public message-flow observer surface가 제거 계약 뒤에도 남아 있다

**판정: GAP**

Java exact interface는 application이 등록하는 message-flow observer, error sink와 raw event
DTO를 public contract로 두지 않는다
(`server/languages/java/interfaces/configuration-host.ko.md:533`,
`server/languages/java/interfaces/monitoring.ko.md:232-233`). Kotlin 계약도 같은 결론이다
(`server/languages/kotlin/interfaces/monitoring.ko.md:42`).

그러나 구현은 public `ZLinkMessageFlowObserver`와 두 `setMessageFlowObserver(...)` overload를
계속 제공한다
(`configuration/ZLinkMessageFlowObserver.java:5-6`,
`configuration/ZLinkDispatchOptions.java:8-12`). Runtime도 이 observer를 생성하고 호출한다
(`runtime/configuration/ZLinkDispatchOptionsRegistration.java:15-46`,
`runtime/diagnostics/ZLinkMessageFlowTracer.java:88-159`). 이는 snapshot 정리만 남은 문제가
아니라 public API와 실행 경로가 함께 남아 있는 상태다.

Observer 외에도 `traceLogFile`, `traceLabel`, `includeNativeDiagnostics`, `logFile`, `label`,
`effectiveMessageFlow`와 raw event/error DTO·enum이 public source에 남아 있다
(`configuration/ZLinkDispatchOptions.java:22-26`,
`configuration/ZLinkDiagnosticsOptions.java:13-24`). Kotlin의 `onMessageFlow` extension도 이 제거된
observer를 노출한다
(`zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkDispatchOptionsExtensions.kt:30-34`).
Exact interface가 허용하는 dispatch option은 level, sampling rate와 message size 포함 여부뿐이다
(`server/languages/java/interfaces/configuration-host.ko.md:410-420`). 따라서 observer overload만
제거해서는 이 gap이 닫히지 않는다.

#### JVM-CONTRACT-003 — Java와 Kotlin STREAM session send timeout이 public surface에 없다

**판정: GAP**

Java exact interface는 `ZLinkSessionSendCall.timeout(Duration)`을 요구한다
(`server/languages/java/interfaces/stream-session.ko.md:94-98`). 이 timeout은 socket send timeout과
per-call 값 중 짧은 값을 admission에 적용하며, 만료되면 `DEADLINE_EXCEEDED`로 terminal-once
완료해야 한다 (`:132-136`). Kotlin exact interface도 같은 modifier와 coroutine cancellation 의미를
규정한다 (`server/languages/kotlin/interfaces/stream-session.ko.md:75-79`, `:147-152`).

Java source의 `ZLinkSessionSendCall`에는 `metadata`, `compress`, `submit`만 있다
(`zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionSendCall.java:5-10`).
Kotlin contract와 Kotlin projection wrapper에도 timeout이 없다
(`zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/contracts/ZLinkOneWayContracts.kt:128-132`,
`zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:360-376`).
따라서 method만 빠진 것이 아니라 per-call deadline validation, socket timeout과의 최소값 계산,
timeout 뒤 admission·replay 금지까지 구현되지 않은 상태다.

#### JVM-CONTRACT-004 — diagnostics level enum이 exact interface와 다르다

**판정: GAP**

Java와 Kotlin 계약은 diagnostics level을 `OFF`, `ERRORS`, `NORMAL`, `DETAILED` 네 값으로 고정하고
생략 시 `ERRORS`를 사용한다
(`server/languages/java/interfaces/configuration-host.ko.md:461-468`, `:529-533`,
`server/languages/kotlin/interfaces/monitoring.ko.md:42-44`).

구현의 enum은 `OFF`, `ERRORS_ONLY`, `KEY_TRANSITIONS`, `VERBOSE`, `DIAGNOSTIC` 다섯 값이다
(`configuration/ZLinkMessageFlowLogMode.java:3-8`). 기본값과 tracer 비교도 각각
`ERRORS_ONLY`, `KEY_TRANSITIONS`, `VERBOSE`에 연결되어 있다
(`runtime/configuration/ZLinkDispatchOptionsRegistration.java:157`,
`runtime/diagnostics/ZLinkMessageFlowTracer.java:130-185`). 의미가 비슷한 값이 있다는 사실로는
enum name과 cardinality가 포함된 exact public contract를 충족하지 못한다. Enum과 실행 level
mapping을 함께 바꾸고 Java/Kotlin snapshot으로 고정해야 한다.

### P0. Wire와 상호 운용성

#### JVM-WIRE-001 — RouteMesh에 금지된 message-size 설정과 admission field가 남아 있다

**판정: GAP**

현재 common contract에서 RouteMesh ServerServer에는 Framework-level message-size 설정·협상·거부가
없다 (`common/spec/07-channel-topology.ko.md:609-634`,
`common/internals/12-service-wire-protocol.ko.md:91-110`). 따라서 raw MeshNode가
`maxMessageSize`를 적용하지 않고 RouteMesh 송수신 경로에 effective bound 검사를 하지 않는 사실은
gap이 아니다.

실제 gap은 Java public `ZLinkMeshNodeSocketConfig.maxMessageSize`와 registration 기본값 16 MiB,
`meshNode(...).maxMessageSize(value)` runtime setter가 남아 있고, node descriptor에는 설정과
무관한 `effectiveMaxMessageBytes = 4 MiB`를 기록해 M6A codec으로 송수신한다는 점이다. Kotlin도
Java public type을 그대로 사용하므로 같은 gap을 공유한다. RouteMesh의 설정·runtime setter와
descriptor/admission/wire field뿐 아니라 finite MeshNode max를 요구하는 Application HWM startup
validation도 제거해야 한다. Java exact interface와 이를 재사용하는 Kotlin 문서는 common contract에
맞게 RouteMesh 전용 setter와 negotiated-bound 설명을 제거했으므로 구현을 이 계약에 맞춰야 한다.
증거: `configuration/ZLinkMeshNodeSocketConfig.java:8-10`,
Java exact interface `interfaces/configuration-host.ko.md:87-122,326-331`,
`runtime/mesh/MeshNodeRegistration.java:969-986`,
`runtime/mesh/MeshNodeRegistration.java:493-507`,
`runtime/channels/ZLinkRouteMeshRuntimeOptionsRuntime.java:29-43`,
`runtime/internal/service/ZLinkServiceNodeDescriptor.java:18,40-42`,
`runtime/internal/service/ZLinkServiceM6AWireCodec.java:24-31,86-90`,
`runtime/binding/ZLinkJavaRawMeshNode.java:6434-6456`.

StreamNode의 크기 상한 적용 자체는 이 gap에 해당하지 않는다. Java/Kotlin이 공유하는 `ZLinkStreamSocketConfig`에
`maxMessageSize`가 있고 registration 기본값은 64 KiB다. 이 값은 Core STREAM socket과 receive
buffer로 전달되어 6-byte prefix를 제외한 inbound client→server complete message의
header+payload를 검사하며, server→client에는 적용하지 않는다. 다만 초과 시 server에
`EMSGSIZE`를 기록하는 계약은 `JVM-STREAM-SIZE-001`로 별도 추적한다. 증거:
`configuration/ZLinkStreamSocketConfig.java:10-13`,
`runtime/streams/StreamNodeRegistration.java:159-180`,
`runtime/streams/ZLinkStreamRuntime.java:266-270,783-787`,
`runtime/streams/ZLinkStreamReceiveBuffer.java:9-19,55-58`,
`runtime/binding/ZLinkJavaStreamSocket.java:107-108`.

#### JVM-STREAM-SIZE-001 — STREAM 초과의 `EMSGSIZE` 기록과 연결 종료가 보장되지 않는다

**판정: GAP**

Common spec과 Java/Kotlin exact interface는 상한 초과 message를 handler에 일부도 전달하지 않고,
server가 `EMSGSIZE`를 기록한 뒤 연결을 종료하도록 요구한다
(`common/spec/19-stream-session.ko.md:182-188`,
Java exact interface `interfaces/stream-session.ko.md:41-47`). 현재 receive buffer는 초과를 일반
`IllegalArgumentException("STREAM frame exceeds MaxMessageSize")`로 만들고 peer isolation 경로는 이를
일반 malformed input warning과 protocol-error isolation으로 기록하고 raw client에 protocol-error control을
보낸다. 이 Framework reassembler 경로는 peer를 ignored set에 넣을 뿐 명시적 transport disconnect도
호출하지 않는다. Core socket의 disconnect
monitor는 native code를 항상 `0`으로 전달하므로 `EMSGSIZE`를 보존하지 않는다. Handler 미전달은
맞지만 server-side 오류 종류와 segmented oversize의 연결 종료 증거가 계약보다 약하다. 증거:
`runtime/streams/ZLinkStreamReceiveBuffer.java:45-58`,
`runtime/streams/ZLinkStreamRuntime.java:574-585,737-774,992-1011`,
`runtime/binding/ZLinkJavaStreamSocket.java:117-130`.

#### JVM-WIRE-002 — `framework-json-v1` strict profile과 golden fixture 검증이 빠져 있다

**판정: GAP**

internals는 네 runtime이 `framework-json-v1` golden fixture에서 같은 value와 failure를 만들도록
요구한다 (`common/internals/12-service-wire-protocol.ko.md:526`). Fixture는 property 이름의
대소문자를 구분하고, duplicate property, 숫자로 표현한 64-bit 정수, padding이 없는 base64,
소수형 `int32`를 거부한다
(`framework/runtime/protocol/golden/framework-json-v1.json:1-24`).

기본 JVM mapper는 `ACCEPT_CASE_INSENSITIVE_PROPERTIES`를 켠다
(`runtime/messaging/ZLinkJsonMessageSerializer.java:29-37`). 일반 application DTO에는 duplicate
property 감지, 64-bit 정수의 문자열 표현, strict base64와 정수 범위를 하나의 profile로 적용하지
않는다. `ActorRef`와 `SpotRef`에는 별도 strict parser가 있지만 이는 generic DTO 계약을 충족하지
않는다. 기존 serializer test도 두 reference type의 일부 규칙만 검사한다
(`ZLinkJsonMessageSerializerTest.java:55-116`). JVM test source에서
`framework-json-v1.json`을 읽는 consumer는 확인되지 않았다.

Golden fixture를 test resource로 직접 읽고 valid/invalid case 전체를 동일한 DTO schema에 적용해야
한다. Java와 Kotlin은 같은 serializer를 사용하므로 두 package consumer 경로에서도 같은 결과를
확인해야 한다.

### P0. Backpressure와 mailbox 회계

#### JVM-BACKPRESSURE-001 — mailbox byte 설정과 owner별 회계가 적용되지 않는다

**판정: GAP**

Exact interface는 `mailboxMessageBudget`과 `mailboxByteBudget`을 owner별 application mailbox의 count와
byte 상한으로 적용하고, byte에 payload, metadata와 작업당 고정 비용을 포함하도록 규정한다
(`server/languages/java/interfaces/configuration-host.ko.md:339-346`). Internals도 각 실행 대기열에
count·byte reservation을 모두 두고 envelope, metadata와 queue node 비용을 세도록 정한다
(`common/internals/08-object-lifecycle.ko.md:238-247`, `:266-268`).

Registration은 byte budget을 보관하지만 (`runtime/mesh/MeshNodeRegistration.java:968-995`), startup은
message budget만 raw node에 전달한다 (`runtime/mesh/ZLinkMeshNodeRuntime.java:45-63`). Raw node는 설정과
무관하게 application byte budget을 64 MiB로 고정한다
(`runtime/binding/ZLinkJavaRawMeshNode.java:711-715`).

`ZLinkServiceMailbox`도 owner별 queue가 아니라 application domain 전체의 count와 byte를 먼저 검사한다
(`runtime/internal/service/ZLinkServiceMailbox.java:32-48`, `:208-224`). `retainedBytes()`는 frame byte만
더하고 source identity, metadata와 queue node의 고정 비용을 세지 않는다 (`:152-180`). 따라서 설정값이
무시되고 한 owner의 backlog가 다른 owner admission을 막을 수 있으며, 빈 payload와 metadata가 많은
작업의 memory를 실제보다 적게 계산한다.

### P1. Session과 relocation 연속성

#### JVM-SESSION-001 — session 교체가 이전 owner의 cleanup 확인 전에 완료된다

**판정: GAP**

internals는 새 owner가 이전 owner에게 cleanup을 요청하고 확인을 받은 뒤에만 새 binding 완료를
반환하도록 규정한다 (`common/internals/09-session-binding.ko.md:67-85`, `:115-121`).
Exact interface도 새 identity를 먼저 등록한 뒤 이전 callback을 최대 한 번 실행하고 이전 binding을
tombstone으로 확정하도록 규정한다
(`server/languages/java/interfaces/stream-session.ko.md:24-38`).

현재 `bindSession`은 token을 증가시키고 현재 binding과 source identity를 즉시 덮어쓴다
(`runtime/actors/ZLinkActorContextState.java:215-224`). 호출 경로는 location route까지 즉시
갱신한 뒤 token을 반환한다 (`runtime/actors/ZLinkActorRuntime.java:2558-2574`). 이전 owner 통지,
cleanup acknowledgement, acknowledgement 대기 중 기존 route 유지 단계가 없다. Token 비교는
늦게 도착한 disconnect가 새 binding을 지우는 문제만 막으며, 교체 중 두 owner가 동시에 session을
사용하거나 이전 exact binding callback을 누락하는 문제를 해결하지 않는다.

Public session 경로도 같은 차이를 보인다. 새 binding을 만들면 `replaceBinding`이 같은 Actor ID의
기존 항목을 `removeIf`로 즉시 지우고 새 항목과 route를 넣는다
(`runtime/actors/ZLinkSessionActorsRuntime.java:396-409`). 이 경로에는 제거된 binding의 disconnect
callback을 실행하거나 terminal을 기다리고 tombstone을 남기는 단계가 없다. 따라서 기존 보고의
Actor state 근거뿐 아니라 STREAM rebind public 동작에서도 계약 위반이 확인된다.

Native bind retry는 `CONFLICT`, `BUSY` 또는 errno 16을 `alreadyBound`로 분류하고
(`runtime/actors/ZLinkActorSubmitFaults.java:20-25`), exact Actor·generation·session identity가 같은지
확인하지 않은 채 성공으로 완료한다 (`runtime/actors/ZLinkActorRetryScheduler.java:184-215`,
`runtime/actors/ZLinkBoundSessionRuntime.java:152-162`). 이 경로도 이전 binding을 새 binding의 성공으로
오인할 수 있으므로 rebind state machine에서 identity 확인을 함께 수행해야 한다.

#### JVM-RELOCATION-001 — pre-commit handoff backlog에 명시된 상한이 적용되지 않는다

**판정: GAP**

relocation 차단 뒤 message 보관 한도는 이동 한 건당 1,024건/16 MiB이고, 초과 request는
`Unavailable`로 끝나야 한다 (`common/internals/05-relocation-continuity.ko.md:46-48`, `:135-143`).

구현은 같은 상수 두 개를 선언하지만 (`ZLinkActorTransferHandoff.java:24-27`), `begin`이 만든
pre-commit `ArrayList`에 `capture`가 count와 byte 검사 없이 계속 추가한다
(`ZLinkActorTransferHandoff.java:35-69`). 상한은 commit 이후 Message Follow source의
`tryAcquire`에만 적용된다 (`ZLinkActorTransferHandoff.java:391-399`). 따라서 이동 준비나 target
activation이 지연되면 source process memory가 설정된 한도와 무관하게 증가할 수 있다.

#### JVM-RELOCATION-002 — Message Follow 실패가 정식 error kind로 변환되지 않는다

**판정: GAP**

internals는 follow loop를 `Unavailable`, object generation 불일치를 `InvalidOperation`, 전달량 초과를
`CapacityExceeded`로 끝내도록 규정한다 (`common/internals/05-relocation-continuity.ko.md:77-81`).
구현의 post-commit queue는 count와 byte를 제한하지만 unavailable route, generation 불일치와 queue
초과를 모두 일반 `IllegalStateException`으로 완료한다
(`ZLinkActorTransferHandoff.java:241-257`). Public caller까지 가는 relay 경로에서 이 예외가
상황별 `ZLinkFrameworkErrorKind`로 변환된다는 근거도 없다. Loop/hop 초과, generation 불일치와
byte/count 초과 각각의 caller-visible error를 process test로 고정해야 한다.
또한 hop count가 8을 넘으면 notice 전송만 중단하고 caller operation을 `Unavailable`로 완료하지 않는다
(`runtime/actors/ZLinkActorRuntime.java:3009-3013`). 세 오류 의미가 source에서 직접 누락되었으므로
구조 일부만 부족한 `PARTIAL`이 아니라 caller-visible terminal 계약의 `GAP`이다.

### P1. Startup 상태 공개

#### JVM-LIVENESS-001 — 준비가 끝나기 전에 object descriptor가 `SERVING`을 공개한다

**판정: GAP**

internals는 endpoint와 자기 주소를 게시한 다음 peer 수락, local handler와 object runtime 준비를
끝내고서 `SERVING`과 신규 target 선택을 공개하도록 정한다
(`common/internals/10-liveness-and-state.ko.md:72-92`).

Java startup은 location subsystem 직후 object descriptor를 `SERVING`으로 게시한다
(`runtime/host/ZLinkFrameworkRuntime.java:416-425`). 그 뒤에야 authority fence를 refresh하고 manual
object peer를 연결하며 Spot subsystem, retire recovery, authority route와 auto-connect를 시작한다
(`:426-463`). Public host readiness는 전체 chain이 끝난 뒤 `SERVING`으로 바뀌므로
(`:464-475`) host status 자체는 순서가 맞지만, peer의 target selection에 쓰이는 descriptor는 local
handler와 object runtime 준비 전에 노출된다. Recovery가 필요한 경우에만 첫 게시를 `PREPARING`으로
늦추므로 일반 startup이 계약과 반대다.

#### JVM-LOCATION-001 — object location 조회가 `UNAVAILABLE` 상태와 Store 오류를 구현하지 않는다

**판정: GAP**

Exact interface는 exact lookup에서 Missing, Creating, Ready와 commit 뒤 current owner를 사용할 수 없는
상태를 각각 빈 `Optional`, `CREATING`, `READY`, `UNAVAILABLE`로 구분한다. Store 조회 실패는 operation
전체를 `ZLinkFrameworkErrorKind.UNAVAILABLE`로 완료해야 한다
(`server/languages/java/interfaces/location-objects.ko.md:30-39`).

구현은 authority allocation이 `PENDING`이면 `CREATING`, 나머지는 모두 `READY`로 변환하므로
`UNAVAILABLE`을 만드는 분기가 없다
(`runtime/locations/ZLinkLocationRuntimeQueryService.java:151-163`, `:310-327`). Store read와 list stage
실패도 정식 Framework error kind로 변환하지 않고 provider failure를 그대로 전달한다. Public runtime은
이 query service를 추가 변환 없이 반환한다 (`runtime/host/ZLinkFrameworkRuntime.java:723-727`).

List query는 한 page를 반환하기 전에 Store cursor 끝까지 전체 matching entry를 메모리에 모으고 최대
64개 snapshot을 보관한다 (`ZLinkLocationRuntimeQueryService.java:166-214`, `:217-250`). 반환 page의
1,000건/4 MiB 제한은 적용하지만 조회 전체 크기에 비례하는 선행 memory 점유가 남으므로 bounded query
검증에는 이 경로도 포함해야 한다.

#### JVM-LOCATION-002 — Ready Instance owner loss가 cold activation으로 전환된다

**판정: GAP**

공개 계약인 failure spec §4.4는 이미 `Ready`인 Instance Spot의 owner process가 종료되거나 lease가 만료되면
그 authority를 Missing으로 바꾸지 않고 call을 bounded `Unavailable`로 끝내도록 요구한다. Relocation
Store의 activation record는 같은 target node와 lifecycle에서 끝나지 않은 initial cold activation만
재개하며, steady-state owner loss의 다른 node takeover나 queue recovery에 사용할 수 없다
(`common/spec/31-failure-failover-policy.ko.md`, Java exact interface
`server/languages/java/interfaces/spots.ko.md`, Kotlin은 같은 Java runtime 의미를 사용한다).

Internals 06·08·10은 이를 resolver의 닫힌 결과, activation state와 liveness 책임 분리로 구현하고,
12는 same-target initial recovery root만 허용한다. 이 구조 문서들은 공개 오류나 failover 범위를
추가하지 않는다.

현재 store resolver는 owner lease가 만료되면 cache를 무효화하고 `null`을 반환한다
(`runtime/locations/ZLinkStoreLocationResolvers.java:306-327`). Channel과 Spot outbound는 이 결과 또는
stale-owner `NotFound`를 Instance activation 경로로 전달한다
(`runtime/channels/ZLinkChannelSpotCalls.java:209-245`,
`runtime/spots/ZLinkDefaultSpotOutbound.java:354-381`). Unit test도 stale owner의 `NotFound` 뒤
`reactivated` 응답을 성공으로 기대한다
(`ZLinkChannelRuntimeTest.java:758-810`). Authority row가 남아 있으면 reserve conflict가 실제 replacement를
막을 수 있지만, cold activation 자체를 시작하지 말아야 한다는 계약을 충족하지 않는다.

**완료 조건**: resolver 결과에서 true Missing과 expired Ready owner를 구분한다. 후자는 Java와 Kotlin
모두 새 activation target을 선택하거나 factory를 실행하지 않고 `Unavailable`로 완료해야 한다. 기존
reactivation unit test를 반대 assertion으로 고치고, `IS-E2E-05`와 `IS-E2E-35`에서 owner process 종료,
lease invalidation, bounded `Unavailable`, 새 factory·handler·queue recovery 부재를 검증한다. 같은 target
node/lifecycle의 미완료 initial cold activation recovery는 별도 positive scenario로 유지한다.

### P1. Diagnostics 비용

#### JVM-DIAGNOSTICS-001 — diagnostics `OFF`에서도 dispatch error event를 할당한다

**판정: GAP**

Internals는 tracing이 꺼진 경로가 현재 level을 읽고 분기하는 것으로 끝나야 하며, 기록할 값이나
문자열과 객체를 먼저 만들면 안 된다고 규정한다
(`common/internals/10-liveness-and-state.ko.md:184-199`).

Dispatch error reporter는 mode를 확인하기 전에 `ZLinkMessageFlowEvent`를 생성하고 모든 field를 채운 뒤
`trace()`를 호출한다 (`runtime/diagnostics/ZLinkDispatchErrorReporter.java:38-55`). 실제 `OFF` 검사는
`trace()` 내부에서 그 뒤에 수행된다 (`runtime/diagnostics/ZLinkMessageFlowTracer.java:60-70`). 따라서
오류가 발생한 경로에서는 `OFF`여도 trace DTO allocation과 field 준비 비용을 지불한다. Reporter가
`enabled(ERROR)`를 먼저 검사한 뒤 event를 만들어야 한다.

### P1. 실행 구조와 완료 확정

#### JVM-PROGRESS-001 — infrastructure 실행 자원이 process 단위 lane으로 집약되지 않았다

**판정: PARTIAL**

internals는 infrastructure 자원을 process 단위로 배분하고 topology나 Spot 수에 따라 늘리지
않도록 규정한다 (`common/internals/03-progress-isolation.ko.md:83-86`). Java는 virtual thread를
사용할 수 있지만 infrastructure lane을 별도 executor에 연결해야 한다는 결정도 같은 문서에 있다.

현재 raw MeshNode마다 deadline executor와 application executor를 새로 만든다
(`ZLinkJavaRawMeshNode.java:156-164`). MeshNode가 topology별로 만들어지므로 이 두 자원은 process가
아니라 topology 수에 따라 증가한다. Client/server location, fanout location과 channel executor는 host
단위 singleton일 수 있어 topology 비례 증거에서는 제외했다. Per-MeshNode 구조는 process 단위 배분
결정과 다르지만 executor 개수만으로 실제 progress failure를 확정할 수는 없다. Process 단위
application/infrastructure lane으로 집약할지 검토하고, 우선 application handler가 모두 양보한 채
대기하는 process scenario에서 peer 관리, timeout과 relocation이 계속 진행하는지 확인해야 한다.

#### JVM-COMPLETION-001 — terminal winner를 정하는 방식이 한 completion primitive로 수렴하지 않았다

**판정: PARTIAL**

internals는 응답, timeout, cancellation, shutdown과 disconnect가 하나의 완료 자리를 두고
경쟁하고, 진행 중 표에서 항목을 원자적으로 꺼내는 한 방식을 사용하도록 정한다
(`common/internals/04-completion.ko.md:19-38`).

service request는 `ZLinkServiceOperationRegistry`를 사용하지만, raw Spot/Actor 응답 경로는 method별
`AtomicBoolean terminal`을 따로 만들고 (`ZLinkJavaRawMeshNode.java:4160-4163`, `:4466-4478`,
`:4646-4657`, `:5251-5263`), STREAM reply retry도 별도 `AtomicBoolean terminal`을 사용한다
(`ZLinkStreamSessionContextState.java:402-406`). 각 경로가 개별적으로 정확할 수는 있지만,
timeout·shutdown·late reply 정리 규칙을 한 곳에서 검증할 수 없다. 공통 primitive로 수렴시키고
경쟁 조합별 unit test와 process test를 같은 상태 전이로 검증해야 한다.

### P1. Payload 소유권과 복사

#### JVM-OWNERSHIP-001 — immutable payload accessor와 submit 경로가 전체 buffer를 반복 복사한다

**판정: GAP**

internals는 framework가 추가하는 buffer 복사를 0으로 줄이고, public immutable accessor와 별도로
runtime 내부 소유권 이전 경로를 두도록 정한다
(`common/internals/11-message-ownership.ko.md:32-43`, `:95-98`).

`ZLinkEncodedPayload.from`은 입력 배열을 복사하고 `bytes()`도 호출할 때마다 다시 복사한다
(`ZLinkEncodedPayload.java:13-20`). 기본 encode 경로는 이 accessor 결과로 다시 `Message`를 만든다
(`runtime/messaging/ZLinkPayloadEncoding.java:23-28`). Bound session submit은 `Message`를
`byte[]`로 바꾸고 원본을 닫은 뒤 같은 byte로 새 `Message`를 만든다
(`runtime/actors/ZLinkBoundSessionRuntime.java:248-263`). Channel retry도 같은
`Message -> byte[] -> Message` 변환을 수행한다
(`runtime/channels/ZLinkChannelRouteCalls.java:568-601`).

공개 불변성은 유지하되 package-private ownership transfer나 close-aware holder를 두고, enqueue와
retry가 동일 buffer owner를 안전하게 넘기도록 바꿔야 한다. 수정 뒤에는 accessor 호출 수가 아닌
실제 전체 buffer 복사 횟수를 size별 benchmark로 확인해야 한다.

#### JVM-OWNERSHIP-002 — 정상 message hot path에서 relocation record를 미리 만든다

**판정: GAP**

Internals는 relocation 대비 기록을 평상시 route message마다 만들지 않고 owner sealing이 시작된 뒤
대기열의 확정된 작업에서 생성하도록 정한다
(`common/internals/11-message-ownership.ko.md:47-57`).

Java raw runtime은 정상 remote Spot message를 받을 때 payload를 application message로 decode한 뒤 다시
wire payload로 encode하여 frozen record를 만든다
(`runtime/binding/ZLinkJavaRawMeshNode.java:4437-4455`). Actor message도 정상 수신마다 같은 record를
만든다 (`:5233-5242`). Relocation 발생 여부와 관계없이 모든 message가 이 encode와 allocation 비용을
지불한다.

생성된 record는 backend received object의 생성자와 accessor에서 다시 clone된다
(`runtime/internal/backend/ZLinkBackendReceived.java:22-28`, `:214-221`). Handoff packet도 생성과 접근
시점에 각각 clone한다 (`runtime/actors/ZLinkActorHandoffPacket.java:17-28`, `:51-52`). 기존
`JVM-OWNERSHIP-001`이 일반 payload accessor와 retry 복사를 다룬다면, 이 항목은 internals가 별도로
금지한 정상 경로의 relocation 대비 encode·복사를 다룬다.

## 4. Internals 문서별 판정

| 문서 | 판정 | 근거와 남은 검증 |
|---|---|---|
| 01 layering | UNVERIFIED | Java runtime과 Kotlin projection 경계는 보이지만 clean consumer에서 package 경계를 검증하지 못했다. |
| 02 serialization | UNVERIFIED | bounded serial queue 구현과 unit test는 있으나 module unit test를 다시 실행하지 않았다. |
| 03 progress isolation | PARTIAL | MeshNode별 executor가 있으나 실제 progress 위반은 process scenario로 확인해야 한다. |
| 04 completion | PARTIAL | service registry가 있으나 여러 runtime 경로가 독립 terminal flag를 사용한다. |
| 05 relocation continuity | GAP | pre-commit backlog가 무제한이고 Message Follow error kind가 정식 오류로 수렴하지 않는다. |
| 06 routing and cache | GAP | Location object query와 Instance resolver가 expired Ready owner를 `UNAVAILABLE`로 구분하지 않는다. |
| 07 dispatch loop | UNVERIFIED | raw receive와 mailbox에 batch budget이 있으나 fairness와 control progress를 process로 확인하지 못했다. |
| 08 object lifecycle | GAP | mailbox byte 설정, owner별 상한과 metadata·고정 비용 회계가 적용되지 않는다. Idle eviction process 결과는 별도 미검증이다. |
| 09 session binding | GAP | 이전 owner cleanup acknowledgement와 exact callback 없이 새 binding을 즉시 확정한다. |
| 10 liveness and state | GAP | object descriptor가 local 준비 전에 `SERVING`을 공개하고 diagnostics `OFF`에서도 error event를 할당한다. |
| 11 payload ownership | GAP | accessor·retry 복사와 정상 message별 relocation record encode·clone이 남아 있다. |
| 12 service wire | GAP | RouteMesh에 금지된 상한 surface·wire field, STREAM 초과의 EMSGSIZE 진단과 JSON golden profile이 계약과 다르다. |

`UNVERIFIED`는 구현 완료를 뜻하지 않는다. 해당 문서의 관찰 결과를 실제 process에서 확인해야
판정을 바꿀 수 있다.

## 5. 검증 결과와 실행하지 못한 Gate

Commit `425b9c2a8272`에서 다시 실행한 명령은 다음과 같다.

```bash
cd framework/languages/java
./scripts/verify_api_snapshot.sh java
./scripts/verify_api_snapshot.sh kotlin
```

두 명령은 모두 통과했다. Java는 2,912줄과 SHA-256
`b78f715d054f95f2b69c070938e8d36f5941ec90585ddc459f9815fbe0debb3c`, Kotlin은 3,421줄과 SHA-256
`2b1476e8e1c853c5a1fda3f356f0368cbeb388a5468b9f3882e91c1627e64c01`의 snapshot을 확인했다. Kotlin은
incremental cache 충돌 뒤 compiler fallback으로 성공했으므로 cache 안정성은 별도 확인 대상이다.
다음 결과는 아직 확보하지 못했다.

- JVM module unit test
- local package를 사용하는 clean consumer build
- Java/Kotlin common E2E와 cross-runtime process scenario
- timeout/cancellation/shutdown 경쟁, relocation overflow와 session 교체의 실제 terminal 결과

Source inspection 결과만으로 확인된 GAP은 위 목록에 그대로 유지한다. 반대로 source에 관련
class나 test가 있다는 이유만으로 `UNVERIFIED` 항목을 완료로 올리지 않는다.

## 6. 권장 수정 순서와 완료 조건

1. 새 Gradle cache와 clean consumer에서 고정 Java bindings package의 provenance와 compile을 재현한다.
2. `ZLinkRouteMeshRuntimeOptions.meshNode(String)`의 message-size runtime option을 제거하고
   Java/Kotlin exact interface를 동시에 고친다. Placement·channel option은 유지한다. Observer public
   surface를 제거하고 diagnostics enum을 네 정식 값에 맞춘다.
   STREAM session send call에는 Java/Kotlin timeout modifier와 admission deadline 동작을 함께 추가한다.
3. RouteMesh startup registration의 `maxMessageSize`, runtime setter와 descriptor/admission의
   `effectiveMaxMessageBytes`를 제거한다. Mailbox count·byte 설정은 별도 startup 경로로 전달하고
   owner별 reservation에 metadata와 고정 비용을 포함한다. STREAM 초과는 handler 미전달·연결 종료를
   유지하면서 server 진단에 `EMSGSIZE`를 보존한다.
4. JVM JSON mapper에 `framework-json-v1` profile을 적용하고 shared fixture 전체를 직접 소비하는
   test를 추가한다.
5. Object descriptor는 peer, handler와 object runtime 준비가 끝난 뒤에만 `SERVING`으로 게시한다.
6. Location exact lookup에 current-owner availability projection을 추가하고 모든 Store read/list failure를
   `UNAVAILABLE` Framework error로 변환한다. Instance call resolver도 expired Ready owner를 true Missing과
   구분해 cold activation을 시작하지 않게 한다. Page 생성도 전체 결과를 먼저 보관하지 않게 제한한다.
7. Diagnostics `OFF` 여부를 event DTO 생성 전에 검사하고 제거 계약에 포함된 observer, file·label option과
   raw DTO·enum을 public surface에서 함께 제거한다.
8. Session replacement를 이전 owner cleanup acknowledgement와 exact callback/tombstone을 포함한 하나의
   state machine으로 만들고 bind conflict의 exact identity를 확인한다. Relocation pre-commit과
   post-commit queue에는 같은 count/byte 회계를 적용하고 모든 follow limit을 정식 error kind로 완료한다.
9. Process 단위 execution lanes와 completion primitive로 분산된 executor와 terminal arbitration을
   집약하되, progress process test를 먼저 추가해 실제 실패 조건을 고정한다.
10. Payload owner를 이동할 수 있는 내부 경로를 추가하고 relocation frozen record는 sealing 뒤에만
    만든다. Java/Kotlin package consumer에서 copy count와 memory 상한을 함께 검증한다.
11. 마지막으로 Java/Kotlin API snapshot, unit test, clean package consumer, common E2E,
   cross-runtime process matrix를 순서대로 통과시킨다.

이 항목들은 focused unit test 하나로 닫지 않는다. 각 GAP은 exact interface, runtime 의미,
package 경계와 실제 process 결과 중 해당하는 근거가 모두 확보되어야 완료로 판정한다.
