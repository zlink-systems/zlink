---
title: "Framework 계약과 공통 E2E 정합성 결정 검토"
---

# Framework 계약과 공통 E2E 정합성 결정 검토

- **상태**: 결정 승인·정식 문서 반영 완료. Runtime 구현·package·process E2E 증거는 open gap이다.
- **작성일**: 2026-08-07
- **검토 기준**: `main`의 `09d34089c0956`
- **검토 범위**: Framework 공통 spec·internals, 다섯 언어 server exact interface와 공통 E2E
- **목적**: 서로 충돌하는 계약과 계약 근거가 없는 E2E를 구분하고, 정식 spec을 수정하기 전에 필요한 결정을 한곳에 모은다.

## 1. 결론

현재 문제는 구현이 정식 spec을 따르지 않은 경우만으로 설명할 수 없다. 정식 spec끼리
서로 다른 public surface를 요구하거나, 공통 E2E가 정식 spec에 없는 동작을 P0 조건으로
요구하는 항목이 함께 존재한다.

이 문서는 다음 원칙으로 수정 방향을 제안한다.

1. 공통 spec이 이미 public 동작을 정했으면 언어별 exact interface와 구현을 그 계약에 맞춘다.
2. 공통 spec이 명시적으로 금지한 public API를 다른 언어 구현이나 E2E만 근거로 추가하지 않는다.
3. E2E가 내부 구현 품질을 검증하려면 먼저 application이 관찰할 수 있는 결과를 공통 spec에 고정한다.
4. 같은 결과를 기존 public surface로 검증할 수 있으면 새 API를 추가하지 않고 E2E를 수정한다.
5. protocol과 runtime 내부 최적화는 언어 간 결과나 wire 호환성에 필요할 때만 공통 결정으로 올린다.

검토 결과는 17개 결정 항목으로 정리된다. 그중 4개는 정식 문서끼리 직접 충돌하고,
6개는 공통 계약을 보완해야 하며, 5개는 E2E를 현재 계약에 맞게 고치는 편이 적절하다.
나머지 2개는 internals의 미결정 사항과 중복된 exact declaration을 정리하는 항목이다.

### 1.1 동결 범위

이 검토에서 말하는 동결은 Phase 0에서 승인한 public contract의 signature, default, 닫힌 enum·error
집합, size·timeout 범위, ordering, ownership과 lifecycle 의미를 이후에 바꾸지 않는다는 뜻이다.
구현이 어렵거나 특정 언어 package가 늦게 따라온다는 이유로 정식 계약을 축소하지 않는다.

동결 뒤에는 다음 변경만 허용한다.

- 오탈자, 깨진 anchor와 같은 editorial correction. Public signature snapshot, contract hash와
  contract test 기대값은 바뀌지 않아야 한다.
- 구현, package, E2E와 internals를 동결된 계약에 맞추는 변경
- 정식 계약을 인용하는 문서의 링크와 설명을 같은 의미로 맞추는 변경

새 public API, 기존 API 제거, default·수치·오류·완료 의미 변경은 허용하지 않는다. §8의 gate는
동결 commit뿐 아니라 이후 구현, E2E와 설명 문서가 정식 계약에서 벗어나지 않는지 확인하는 데 적용한다.

## 2. 결정 요약

| ID | 기존 리뷰 | 문제 | 권장 결정 | 주 수정 대상 |
|---|---|---|---|---|
| DEC-01 | A-1, RM-C5, PS-C1, RL-D2, SM-D9 | Public message-flow observer와 runtime error sink 제공 여부가 충돌한다. | 다섯 언어에서 logger provider 대체 경로를 먼저 통과시킨 뒤 public callback과 sink를 제거한다. | 공통 tracing spec·E2E, C++·Java·Node exact interface |
| DEC-02 | A-2 | Diagnostics level 이름과 단계 수가 다르다. | `Off/Errors/Normal/Detailed` 네 단계로 통일한다. | C++ exact interface, 공통 E2E |
| DEC-03 | B-3 | I/O 대기가 CPU worker capacity를 점유하는지 계약이 없다. | I/O 대기 중 CPU execution slot을 점유하지 않는다고 공통 spec에 고정한다. 별도 I/O 설정은 추가하지 않는다. | 공통 async spec |
| DEC-04 | B-4 | Timer overrun policy가 언어 exact interface에는 있지만 공통 계약에 없다. | 세 policy와 tick 의미를 공통 계약으로 승격한다. | 공통 async spec |
| DEC-05 | B-5 | `framework-json-v1`이 internals에만 있다. | 사용자 payload의 언어 간 JSON profile을 public codec 계약으로 옮긴다. | 공통 message model spec |
| DEC-06 | B-6 | Server-only ClientServer 호출의 오류가 미정인데 E2E는 `NotFound`를 요구한다. | 필요한 Client role이 없으므로 `NotConfigured`로 고정한다. | ClientServer spec, CH-E2E-05 |
| DEC-07 | B-7 | STREAM 인증을 Framework gate처럼 검증하지만 현재 계약은 application 책임이다. | Application session callback이 인증 전 요청을 거부하는 시나리오로 고친다. | SM-D7 |
| DEC-08 | C-8, 기존 위치 조회 3건 | Paged object query 공통 계약과 언어 exact interface가 끊겼고 상태 의미도 모호하다. | 공통 상태 결과를 보완하고 다섯 언어 exact interface를 맞춘다. | Location spec, C++·.NET·Node exact interface |
| DEC-09 | C-9 | 같은 STREAM session의 send마다 다른 deadline을 표현할 API가 없다. | STREAM send call에 호출별 `Timeout(...)` modifier를 공통 계약으로 추가한다. | 공통 async spec, 다섯 언어 exact interface, SA-E2E-16 |
| DEC-10 | C-10 | E2E가 target 지정·Actor 단위 Relocate를 요구하지만 spec은 이를 금지한다. | Framework의 target 선택 책임을 유지하고 E2E를 host operation과 경합 검증으로 고친다. | IS-E2E-30, ST-G3 |
| DEC-11 | C-11 | Unbind 의미는 있으나 E2E가 호출할 public operation 이름이 없다. | 기존 logical disconnect operation이 exact binding을 unbind한다고 명시한다. 별도 `Unbind` API는 추가하지 않는다. | Session Actor spec·exact interface, TA-A4 |
| DEC-12 | C-12 | E2E가 public packet sequence와 inbound observer를 요구한다. | Public sequence를 추가하지 않고 correlation ID·packet name·message kind로 판정한다. | SM-D9 |
| DEC-13 | 기존 `messageFollow` 검토 | 중복 통지 억제 수명이 미결정 상태다. | Cache 안전 조건만 공통으로 고정하고 suppression은 구현 선택으로 둔다. | 공통 internals 06·12 |
| DEC-14 | 기존 C++ HTTP 검토 | `http_options_builder_t`가 두 문서에서 다르게 선언된다. | exact interface 한 곳만 declaration을 소유하고 `snapshot()`·`validate()`는 내부로 숨긴다. | C++ HTTP 문서 |
| DEC-15 | 기존 Java listener 검토 | Java 한영 계약의 동작 설명이 다르고 error kind가 미정이다. | 양쪽에 같은 의미를 기록하고 `NotConfigured`로 고정한다. | Java common-runtime exact interface |
| DEC-16 | 기존 C++ HTTP 검토 | Embedded HTTP 예제가 원본 handler의 constructor를 누락한다. | 중복 class declaration을 제거하고 정본 링크와 처리 흐름만 남긴다. | C++ embedded HTTP 문서 |
| DEC-17 | SF-F7 | E2E가 participant별 64 MiB 상한보다 큰 state의 성공을 요구한다. | 64 MiB 이하는 성공, 초과는 `StateIncompatible`로 고쳐 정식 relocation limit과 맞춘다. | SF-F7, relocation E2E 일괄 점검 |

위 결정은 2026-08-07 정식 공통 spec, 다섯 언어 exact interface, internals와 공통 E2E 문서에
반영했다. 이 plan 자체는 public contract가 아니며 실제 구현·package·process E2E 완료 여부는
각 언어 실행 ledger에서 계속 추적한다.

## 3. 정식 문서끼리 충돌하는 항목

### DEC-01. Public message-flow observer와 runtime error sink

#### 현재 문제

[공통 message-flow spec](../framework/common/spec/26-message-flow-tracing.ko.md#6-구현-및-contract-test-검증-요구)은
public interface에 observer callback, runtime error sink와 raw event DTO가 나타나면 안 된다고
규정한다. [.NET exact interface](../framework/common/spec/server/languages/dotnet/interfaces/10-monitoring-errors.ko.md#4-diagnostics-경계)도
같은 경계를 사용한다.

반면 다음 exact interface는 callback이나 sink를 public API로 정의한다.

- [Java dispatch options](../framework/common/spec/server/languages/java/interfaces/configuration-host.ko.md)
  — `setMessageFlowObserver(...)`
- [Node.js dispatch builder](../framework/common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md)
  — `setMessageFlowObserver(...)`, `setRuntimeErrorSink(...)`
- [C++ monitoring](../framework/common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md)
  — `message_flow_event_t`, `message_dispatch_error_event_t`, observer와 callback

[RL-D2](../framework/common/e2e/config-5-resilience-lifecycle.ko.md#rl-d2-telemetry-provider-failure를-messaging에서-격리한다)와
[SM-D9](../framework/common/e2e/config-2-spot-service.ko.md#sm-d9-logger-provider가-stream-message-flow-결과를-기록한다)는
이 callback이 존재한다고 전제한다. 따라서 현재 상태에서는 다섯 언어가 같은 public surface로
같은 E2E를 실행할 수 없다.

#### 권장 결정

Public observer callback과 runtime error sink를 제공하지 않는다. Application은 diagnostics level과
sampling만 Framework에 설정하고, 기록은 application이 구성한 표준 trace·metric·logger provider가
받는다.

이 결정으로 application이 message-flow event를 전용 callback DTO로 받아 코드에서 직접 처리하는
기능은 제공하지 않는다. 같은 요구는 custom logger·telemetry provider가 structured record를 처리하는
방식으로만 충족한다. Callback thread, DTO lifetime과 재진입 규칙을 public contract에 추가하지 않는 대신,
application은 전용 observer보다 표준 provider integration을 구현해야 한다. 이 trade-off를 동결된
public boundary로 수용한다.

이 방향은 다음 정보를 Framework 내부에 숨긴다.

- event DTO의 메모리 수명과 하위 호환
- observer 호출 thread와 callback serialization
- observer 예외와 재진입 처리
- runtime error sink의 별도 failure policy

Provider failure가 message operation 결과를 바꾸지 않는다는 기존 격리 계약은 그대로 유지한다.
Framework는 같은 provider failure의 log 횟수를 제한하고 그 log를 다시 같은 provider로 보내지 않는다.

#### 수정 범위

1. 기존 observer와 sink를 유지한 상태에서 다섯 언어 logger provider가 아래 대체 E2E를 먼저 통과한다.
   이 증거가 없으면 제거 결정을 승인하거나 동결하지 않는다.
2. RM-C5는 application logger provider가 받은 `zlink.dispatch_error`의 정식 field로 판정한다.
3. PS-C1은 Classic fanout의 정상 `zlink.message_flow`를 만들지 않는다. Subscriber local dispatch에서
   handler가 없을 때만 `surface=classic_fanout`, `reason=no_handler`, `action=drop`인
   `zlink.dispatch_error`를 기록한다. 이 record를 publisher별 delivery 결과로 되돌리지 않는다.
4. RL-D2는 failing telemetry provider 뒤에도 normal request가 성공하고, 제한된 provider-failure log가
   별도 fallback logger 또는 process stderr에 남는지 검증하도록 바꾼다.
5. SM-D9는 DEC-12의 correlation 기반 trace 검증으로 대체한다.
6. 대체 E2E가 다섯 언어에서 통과한 뒤 C++·Java·Node exact interface와 구현에서 observer, sink와
   raw event DTO를 같은 변경에서 제거한다. Deprecated public facade는 남기지 않는다.

### DEC-02. Diagnostics level

#### 현재 문제

[공통 tracing spec](../framework/common/spec/26-message-flow-tracing.ko.md#4-application은-기록-범위를-어떻게-정하는가)과
[.NET exact interface](../framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md#8-dispatch-policy와-diagnostics)는
`Off`, `Errors`, `Normal`, `Detailed` 네 단계를 정의한다.

[C++ exact interface](../framework/common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md#2-메시지-흐름-진단)는
`off`, `errors_only`, `key_transitions`, `verbose`, `diagnostic` 다섯 단계를 정의한다.
공통 E2E도 [README의 관측 설정](../framework/common/e2e/README.ko.md#62-diagnostics와-메시지-흐름-기록-켜기-디버깅-1차-도구)과
[OBS-A5](../framework/common/e2e/config-11-observability-ops.ko.md#obs-a5-실행-중-tracing-level-변경을-적용한다)에서
C++ 이름을 공통 이름처럼 사용한다.

#### 권장 결정

공통 네 단계와 의미를 다섯 언어 exact interface에 그대로 투영한다.

| 공통 이름 | 기존 C++ 이름의 대응 | 처리 |
|---|---|---|
| `Off` | `off` | 이름만 언어 관례에 맞춘다. |
| `Errors` | `errors_only` | 공통 이름으로 맞춘다. |
| `Normal` | `key_transitions` | 공통 이름으로 맞춘다. |
| `Detailed` | `verbose`, `diagnostic` | 하나의 public 단계로 합치고 추가 내부 진단은 private 설정으로 둔다. |

공통 E2E는 `Normal → Off → Errors → Normal`만 사용한다. 언어별 runner가 별도 mapping을
해석하게 두지 않는다.

동결 전 C++의 `verbose`와 `diagnostic`이 서로 다른 field나 사건을 기록하는지 golden trace로 비교한다.
둘 중 하나에만 있는 application-visible 진단 정보는 `Detailed`에 합친다. Transport dump나 raw frame처럼
public tracing 계약에 허용되지 않는 정보만 private runtime 진단으로 남긴다.

### DEC-14. C++ HTTP builder declaration 소유권

#### 현재 문제

[HTTP hosting](../framework/common/spec/server/languages/cpp/60-http-hosting.ko.md#4-route-builder)과
[C++ configuration exact interface](../framework/common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md#41-http-hosting)가
같은 `http_options_builder_t`를 완전한 class declaration처럼 반복한다. 두 번째 문서에만
`snapshot()`과 `validate()`가 있다.

#### 권장 결정

`interfaces/02-configuration-host`가 정확한 public declaration을 단독 소유하고,
`60-http-hosting`은 동작과 사용 규칙만 소유한다. `60`의 전체 class declaration은 제거하고
정본으로 연결한다.

`snapshot()`과 `validate()`는 host 내부의 설정 적용과 검증을 위한 동작이므로 application이
직접 호출해야 할 이유가 없다. 두 member는 public contract에서 제거하고 runtime 내부로 숨기는
방향을 권장한다. Public 유지가 필요하다고 결정하면 반환 reference 수명, 호출 가능 시점과
실패 조건을 HTTP hosting 계약에 추가해야 한다.

### DEC-15. Java listener status의 한영 의미와 error

#### 현재 문제

[Java 한국어 exact interface](../framework/common/spec/server/languages/java/interfaces/common-runtime.ko.md)는
`listenerStatus(...)`가 advertised endpoint와 조회 시각을 반환한다고 설명하지만 영문판에는
같은 의미 설명이 없다. 한국어판도 실패를 `public error`라고만 적어 정확한 error kind를
고정하지 않는다.

#### 권장 결정

두 언어 문서에 다음 계약을 동일하게 기록한다.

- 이름과 kind가 가리키는 listener가 bind를 완료한 뒤 현재 advertised endpoint를 반환한다.
- Port `0`을 사용했으면 OS가 정한 실제 port를 반환한다.
- Wildcard bind 주소는 반환하지 않는다.
- Listener가 없거나 bind가 끝나지 않았거나 해당 role이 endpoint를 제공하지 않으면
  `NotConfigured`인 Framework error로 끝난다.
- `observedAt`은 조회가 결과를 만든 시각이다.

`NotConfigured`는 C++ listener 계약과 공통 ErrorKind의 “필요한 role이 등록되지 않음” 의미에
맞는다.

## 4. 공통 계약을 보완해야 하는 항목

### DEC-03. CPU worker와 비동기 I/O의 실행 자원

#### 현재 문제

[공통 async spec](../framework/common/spec/05-async-execution-policy.ko.md#12-worker-offload)은
CPU 작업과 비동기 I/O를 하나의 bounded worker scheduler에 제출한다고만 규정한다.
반면 [TD-C3](../framework/common/e2e/config-8-execution-turn.ko.md#td-c3-io-대기가-cpu-worker-capacity를-사용하지-않는다)와
[TD-C5](../framework/common/e2e/config-8-execution-turn.ko.md#td-c5-cpu-worker-saturation이-io-worker를-막지-않는다)는
두 실행 자원이 분리되어야 통과한다.

#### 권장 결정

공통 spec에 다음 observable contract를 추가한다.

> 비동기 I/O가 외부 completion을 기다리는 동안 CPU worker execution slot과 active CPU capacity를
> 점유하지 않는다. CPU 작업이 configured concurrency를 모두 사용해도 이미 admit된 비동기 I/O의
> completion과 continuation은 진행할 수 있어야 한다.

별도 I/O thread 수나 I/O queue 설정은 public API에 추가하지 않는다. 언어별 runtime이 event loop,
completion port 또는 coroutine executor로 이 결과를 만들도록 내부에서 선택한다. Public worker option은
CPU 계산의 concurrency와 bounded admission만 소유한다.

### DEC-04. Timer overrun policy

#### 현재 문제

공통 timer 계약은 callback 중복 실행 금지와 만료 병합 가능성만 규정한다. `SkipLateTicks`,
`CatchUpBounded`, `DelayNextTick`과 tick index는 .NET·Java·Node·C++ exact interface에 있으며,
Kotlin은 Java 정본 type을 재사용한다. 따라서 리뷰가 지적한 C++·Kotlin public surface 누락은
현재 `main`에는 해당하지 않는다. 실제 gap은 다섯 언어가 사용하는 의미가 공통 계약에 고정되지 않은
점이다.

[TD-D3](../framework/common/e2e/config-8-execution-turn.ko.md#td-d3-timer-overrun-중-callback을-겹쳐-실행하지-않는다)와
[SM-E4](../framework/common/e2e/config-2-spot-service.ko.md#sm-e4-timer-overrun-policy별-observable-sequence를-확인한다)는
이 policy를 공통 결과로 요구한다.

#### 권장 결정

세 policy와 다음 tick 정보를 [공통 async spec의 Spot timer](../framework/common/spec/05-async-execution-policy.ko.md#5-spot-timer)에
정식 계약으로 추가한다.

- `DeliveryIndex`: 실제 callback delivery 순서
- `ScheduledIndex`: 원래 due 순서
- `SkippedTicks`: 이번 delivery 전에 건너뛴 due 수
- `SkipLateTicks`: 겹친 due를 건너뛰고 다음 미래 due를 기다린다.
- `CatchUpBounded`: 설정한 최대 수까지만 연속 delivery하고 나머지는 건너뛴다.
- `DelayNextTick`: callback terminal부터 다음 period를 계산한다.

기본 policy와 `MaxCatchUpTicks` 범위도 공통 spec에서 고정한다. 기존 언어별 exact interface는
확정한 공통 의미와 default가 같은지 다시 검증한다. E2E는 scheduler의 정확한 nanosecond가 아니라
index, skip 수와 bounded callback count를 판정한다.

[RL-F12](../framework/common/e2e/config-5-resilience-lifecycle.ko.md#rl-f12-user-spot-queue와-timer를-relocation-뒤-복원한다)는
policy별 overrun 판정과 다른 책임을 가진다. 이 scenario는 relocation이 logical timer registration,
pending tick과 queue 순서를 target에 한 번 복원하는지 계속 검증한다. Timer policy를 공통 계약으로
올리는 과정에서 RL-F12를 삭제하거나 단순 callback 재등록 scenario로 바꾸지 않는다.
RL-F12 fixture는 `SpotWide` User Spot 또는 Instance Spot으로 한정한다. Entry Spot과 `PerActor` User
Spot의 Spot-level application timer는 이전 대상이 아니며 RL-F12의 성공 조건에 포함하지 않는다.

동결 전에는 다섯 언어의 현재 default policy, `MaxCatchUpTicks` 유효 범위와 overflow 처리를 public
package로 실행해 비교한다. 값이 다르면 구현의 우연한 최소 공통분모를 선택하지 않고, bounded burst와
starvation 방지 요구를 만족하는 하나의 범위를 승인한 뒤 contract test fixture로 고정한다.

### DEC-05. `framework-json-v1` 소유권

#### 현재 문제

사용자 DTO가 언어 경계를 넘을 때 지켜야 하는 JSON 규칙이
[service wire internals](../framework/common/internals/12-service-wire-protocol.ko.md#6-typed-application-message-json)에만
있다. [RC-B6](../framework/common/e2e/config-4-registration-codec.ko.md#rc-b6-다섯-언어가-json-application-값을-같게-복원한다)는
이를 공통 message model 계약으로 인용하지만 해당 spec에는 profile이 없다.

#### 권장 결정

`framework-json-v1`을 [공통 message model](../framework/common/spec/04-message-model.ko.md)의
public codec profile로 옮긴다. 다음 항목은 언어 간 호환과 application-visible failure를 바꾸므로
breaking contract로 관리한다.

- property·enum 이름의 대소문자
- duplicate·required·unknown property 처리
- nullable 조건
- 64-bit integer의 10진 문자열 표현
- 32-bit integer와 finite floating-point 표현
- RFC 4648 padded base64
- custom type의 암묵 변환 금지

Internals에는 JSON bytes가 service record에 들어가는 위치와 serialization ownership만 남기고
profile 규칙을 반복하지 않는다. RC-B6의 링크도 공통 message model로 유지한다.

동결 전에는 다섯 언어의 실제 배포 package로 golden fixture와 거부 fixture를 양방향 실행한다.
Padded Base64, signed 64-bit decimal string, duplicate·required property 처리가 한 언어라도 다르면
문서만 먼저 고정하지 않는다. 이미 배포한 wire 값을 기준으로 구현과 profile을 맞춘 뒤
fixture hash와 expected typed value를 함께 동결한다.

### DEC-06. Server-only ClientServer 호출 오류

#### 현재 문제

ClientServer 이름은 등록되어 있지만 local process에는 Server role만 있는 경우의 호출 결과가
정식 spec에 없다. [CH-E2E-05](../framework/common/e2e/config-12-channel-egress-routing.ko.md#ch-e2e-05-client-role이-없는-process는-clientserver-request를-시작하지-못한다)는
이를 `NotFound`로 단정한다.

#### 권장 결정

`NotConfigured`로 고정한다. ChannelName과 Server registration은 존재하지만 호출에 필요한 Client
role이 local process에 없기 때문이다. `NotFound`는 ChannelName 또는 target이 존재하지 않는 경우에
사용한다.

[ClientServer spec](../framework/common/spec/09-client-server-channel.ko.md)에 이 구분을 추가하고
CH-E2E-05의 기대 결과도 `NotConfigured`로 변경한다. Local handler를 직접 실행하지 않는다는
기존 검증은 유지한다.

### DEC-08. Paged public object query

#### 현재 문제

[Location runtime §6.4](../framework/common/spec/21-location-runtime.ko.md#64-운영-도구에서-현재-위치를-조회한다)와
[runtime monitoring](../framework/common/spec/24-runtime-monitoring.ko.md#4-object의-현재-위치-조회)은
ID별 현재 위치 조회와 object kind·stable type별 bounded page를 공통 계약으로 정의한다.

언어 exact interface는 다음처럼 갈린다.

- Java·Kotlin: Actor·Spot 개별 조회와 paged list를 제공한다.
- Node.js: paged list만 제공한다.
- C++·.NET: object location query를 제공하지 않는다.
- Java state는 `Creating`, `Ready`, `Unavailable`인데 Node.js는 `creating`, `ready`만 제공한다.
- Java 문장은 “Ready가 없으면 empty”와 “Creating entry를 반환”을 동시에 규정한다.
- Java exact interface 목차에는 `location-objects` 문서가 빠져 있다.

#### 권장 결정

공통 spec의 상태 결과부터 다음처럼 보완한다.

| 저장 상태 | ID별 조회 | Paged list |
|---|---|---|
| `Missing` | empty | 항목 없음 |
| `Creating` | `Creating` entry | `Creating` entry 포함 |
| `Ready` | `Ready` entry | `Ready` entry 포함 |
| Commit 뒤 owner를 사용할 수 없음 | `Unavailable` entry | `Unavailable` entry 포함 |
| Store 조회 실패 | `Unavailable` Framework error | Page 전체를 error로 끝내며 일부 page를 성공으로 반환하지 않음 |

그다음 다섯 언어에 다음 기능을 모두 투영한다.

1. Actor ID별 현재 위치 조회
2. Spot ID별 현재 위치 조회
3. Object kind가 필수이고 stable type·MeshName이 선택인 paged list
4. `Creating`, `Ready`, `Unavailable` 상태
5. Page size `1..1000`, encoded page 최대 4 MiB와 opaque continuation token

C++·.NET·Node의 누락은 implementation gap으로 기록하고 exact interface부터 추가한다. Java
`location-objects`는 exact interface 목차에 포함한다. SF-C5·SF-F6는 pagination과 concurrent scan
consistency를 계속 검증한다. ID별 조회와 page 결과의 상태 matrix는 새 SF-C5A로 분리한다. SF-C5A는
`Missing`, `Creating`, `Ready`, commit 뒤 owner를 사용할 수 없는 `Unavailable`과 Store 조회 실패를
각각 만들고, Store 실패에서는 일부 page를 성공으로 반환하지 않는지 확인한다.

`Creating`은 단순 진행 표시가 아니라 생성 중 process가 중단된 object를 운영 도구가 찾아 정리할 수
있게 하는 상태이므로 public query에 포함한다. 동결 전 page size `1..1000`, encoded page 4 MiB와
continuation snapshot 비용을 다섯 언어 clean consumer와 Store fixture에서 측정한다. 한계 안에서
worst-case entry를 encode할 수 있고 continuation이 opaque하게 유지되는지 확인한 뒤 수치를 확정한다.

### DEC-09. STREAM send별 deadline

#### 현재 문제

[SA-E2E-16](../framework/common/e2e/config-13-submit-admission.ko.md#sa-e2e-16-server-stream-send-순서를-유지한다)은
같은 session의 send마다 서로 다른 deadline을 설정한다. 현재 public contract는 socket send timeout과
일부 언어의 cancellation을 제공하지만 one-way STREAM send call별 timeout modifier는 제공하지 않는다.

Cancellation은 다섯 언어가 공유하는 Framework capability가 아니다. .NET의 `CancellationToken`은 .NET
async API의 명시적 waiter cancellation이고, Node.js의 `AbortSignal`도 해당 언어의 선택된 call에만
투영된다. Java `ZLinkSessionSendCall`과 C++ `stream_send_call_t`에는 대응 cancellation 인자가 없으며,
Kotlin STREAM send는 Java 정본 call을 재사용한다. Java·C++는 제출한 call의 completion과 STREAM
lifecycle로 terminal을 관찰하므로 별도 범용 cancellation token을 만들지 않는다. 이 차이는
public contract gap이 아니며 Java·C++에 cancellation API를 추가하지 않는다.

#### 권장 결정

STREAM send call에 호출별 admission timeout modifier를 공통 public contract로 추가한다. Request timeout과
같은 fluent 위치를 사용하되 reply 대기 시간이 아니라 해당 one-way send가 transport admission을
기다릴 수 있는 최대 시간을 뜻한다.

- Modifier를 생략하면 기존 STREAM socket SendTimeout을 사용한다.
- Modifier를 지정하면 socket SendTimeout을 연장하지 않고 더 이른 deadline만 선택한다.
- 값 범위와 millisecond 올림은 공통 send timeout의 `1..INT_MAX` 규칙을 그대로 사용한다.
- Deadline이 먼저 끝나면 `DeadlineExceeded`로 한 번 terminal 완료하고, 이후 capacity가 생겨도
  해당 send를 admission하거나 replay하지 않는다.
- 언어별 cancellation이 별도로 있는 경우 timeout과 경쟁해 먼저 확정된 terminal만 결과가 된다.

Exact interface에는 각 언어 관례에 맞는 `Timeout` 또는 `timeout` modifier를 추가하고 Kotlin은 Java
정본 timeout call을 관용적으로 투영한다. Cancellation 인자나 Framework 전용 token은 추가하지 않는다.
SA-E2E-16은 `1`, 짧은-timeout marker, `2`, `3`을 같은 session에 제출한 뒤 timeout terminal 이후
capacity를 열어 client가 `1,2,3`만 순서대로 받는지 검증한다.

공통 E2E도 cancellation을 다섯 언어의 동일한 public input처럼 요구하지 않는다. Exact interface가
언어 자체의 waiter cancellation을 정의한 경우에만 cancellation variant를 실행한다. 별도 cancellation
input이 없는 언어는 timeout, shutdown 또는 connection loss처럼 해당 operation에 이미 정의된 공통
terminal로 같은 terminal-once·no-replay 불변 조건을 검증한다. 특히 SA-E2E-17의 STREAM reply call에는
이 결정에서 추가하는 send call별 timeout modifier를 적용하지 않는다. Reply token의 one-shot 의미는
normal, socket send timeout과 shutdown으로 공통 검증하고 cancellation은 지원 언어의 추가 variant로만
둔다.

## 5. 현재 계약에 맞게 E2E를 수정할 항목

### DEC-07. STREAM 인증 gate

#### 현재 문제

[SM-D7](../framework/common/e2e/config-2-spot-service.ko.md#sm-d7-application-session-callback이-auth-전-request를-거부한다)은
unauthenticated connection의 업무 packet을 Framework가 dispatch하지 않는 것처럼 서술한다.
현재 STREAM 계약은 session callback이 client를 인증하며 일반 STREAM handler에는 Framework filter를
적용하지 않는다고 정한다.

#### 권장 결정

Framework-level auth state와 gate를 새 public contract로 추가하지 않는다. SM-D7을 다음 application
pattern 검증으로 바꾼다.

- Session open callback이 credential을 확인한다.
- 인증이 끝나기 전에 application session handler가 request를 거부하거나 connection을 닫는다.
- 유효한 인증을 완료한 session만 application handler가 업무 reply를 만든다.
- Framework는 application이 선택한 close/error 결과와 session lifecycle을 정확히 전달한다.

이 시나리오는 Framework가 인증 정책을 소유한다는 증거가 아니라, application callback과 session
lifecycle이 인증 pattern을 구현할 수 있다는 증거로 분류한다.

### DEC-10. Target 지정과 부분 Relocate

#### 현재 문제

[Graceful drain spec](../framework/common/spec/28-graceful-drain-handoff.ko.md)은 application이 MeshName이나
RID로 일부 component를 골라 종료 순서를 만들지 않으며 target은 Framework가 선택한다고 명시한다.

반면 다음 E2E는 금지된 제어 표면을 전제한다.

- [IS-E2E-30](../framework/common/e2e/config-14-instance-spot.ko.md#is-e2e-30-multi-mesh-concurrent-relocate):
  서로 다른 target을 지정한 Relocate 두 개
- [ST-G3](../framework/common/e2e/config-10-spot-actor-relocation.ko.md#st-g3-peractor-spot의-host-relocation):
  같은 Spot의 Actor 하나만 지정한 relocation

#### 권장 결정

Framework-owned target selection과 host-scoped Relocate를 유지한다.

- IS-E2E-30은 같은 host에 concurrent Relocate를 호출해 하나가 shared operation에 참여하거나
  `Blocked/OperationInProgress`로 끝나며 owner가 하나만 남는지 검증한다.
- ST-G3은 host relocation 뒤 `PerActor` mode의 Actor들이 서로 다른 location을 유지할 수 있는지,
  또는 explicit membership transition이 location을 바꾸는 현재 계약 범위로 재작성한다.
- “Actor A만 target B로 옮긴다”는 절차는 삭제한다.

Target hint와 Actor-scoped relocation은 추가하지 않는다. 특정 hot Actor만 drain해야 하는 운영 요구는
membership을 바꾸는 explicit Join 또는 Framework가 target을 선택하는 host relocation으로 처리한다.
Application이 destination node를 지정하거나 relocation unit을 선택하는 기능은 동결된 public contract의
범위 밖이다.

### DEC-11. Public Unbind

#### 현재 문제

[Session Actor spec](../framework/common/spec/20-session-actor-dispatch.ko.md)은 unbind와 disconnect가 exact
binding tombstone을 적용하고 Actor와 membership은 유지한다고 규정한다. 하지만 TA-A4는 이름이
`Unbind`인 별도 public operation이 있는 것처럼 절차를 적는다.

#### 권장 결정

별도 `UnbindAsync`를 추가하지 않는다. 각 언어의 기존 logical disconnect operation이 다음 결과를
소유한다고 공통 spec과 exact interface에 명시한다.

- Exact binding identity에 disconnect callback을 최대 한 번 전달한다.
- Callback terminal 뒤 해당 binding을 tombstone으로 제거한다.
- Physical STREAM connection은 유지할 수 있다.
- Actor와 Spot membership은 변경하지 않는다.

TA-A4는 언어별 `NotifyDisconnected`, `notify_disconnected` 또는 같은 의미의 public operation을
호출한 뒤 binding이 없어지고 direct Actor message는 계속 성공하는지 검증한다. Callback 없이 조용히
binding만 제거하는 별도 기능은 현재 계약에 추가하지 않는다.

Rebind도 callback 없는 우회 unbind로 사용하지 못하도록 다음 의미를 함께 고정한다.

- 하나의 exact binding identity를 다른 Actor나 다른 `ObjectGeneration`에 다시 사용하지 않는다.
- Rebind는 새 identity를 등록하고 이전 exact binding에 disconnect callback을 최대 한 번 전달한 뒤
  이전 identity를 tombstone으로 제거한다.
- Callback failure는 diagnostics에 기록하지만 이전 binding을 복원하거나 새 identity를 제거하지 않는다.
- 같은 `ObjectGeneration`의 relocation route 갱신은 rebind가 아니므로 disconnect callback을 실행하지 않는다.

SM-D4A와 RL-F2는 이전 exact binding의 callback 횟수, tombstone과 callback failure 뒤 새 binding 유지까지
검증한다. SM-D4B는 같은 generation의 relocation route 갱신만 수행하는 fixture에서 disconnect callback이
실행되지 않는지 확인한다.

### DEC-12. Public packet sequence

#### 현재 문제

SM-D9는 public inbound observer와 packet sequence field를 요구한다. Sequence는 replay와 reply correlation을
위한 wire 내부 값이고 공통 tracing attribute에는 포함되지 않는다. DEC-01을 적용하면 public inbound
observer도 제공하지 않는다.

#### 권장 결정

Sequence를 public attribute로 승격하지 않는다. SM-D9는 application logger provider가 받은
`zlink.message_flow` record와 handler evidence를 다음 값으로 대조한다.

- `correlation_id`
- `packet_name`
- `message_kind`
- `surface=stream`
- application이 payload에 넣은 고유 operation marker

Wire sequence의 wrap, generation과 replay fence는
[service wire internals §11](../framework/common/internals/12-service-wire-protocol.ko.md#11-request-terminal-identity)과
각 언어의 STREAM protocol contract test matrix가 소유한다. Matrix에는 wrap 전후 ordering, stale generation,
duplicate reply와 replay fence를 닫힌 test ID로 둔다. Application E2E는 transport sequence를 알 필요가 없다.

### DEC-17. Relocation state size E2E

#### 현재 문제

[SF-F7](../framework/common/e2e/config-6-store-failure-recovery.ko.md#sf-f7-large-state-relocation은-public-size-limit-안에서-복원한다)은
64 MiB보다 큰 application state도 logical relocation maximum 안이면 성공한다고 요구한다. 반면
[graceful drain의 unit gate](../framework/common/spec/28-graceful-drain-handoff.ko.md#7-relocation-unit과-실행량-제한)는
`PreserveStateWith` participant 하나의 encoded state가 64 MiB를 넘으면
`Blocked/StateIncompatible`이라고 고정한다. Store record 분할 가능 여부가 participant public size limit을
늘리지는 않는다.

#### 권장 결정

Participant별 64 MiB 상한을 유지하고 SF-F7을 다음 경계 검증으로 고친다.

- 64 MiB 이하의 state는 relocation 뒤 checksum과 logical length를 보존한다.
- 64 MiB를 한 byte라도 넘는 state는 source authority를 유지하고
  `Blocked/StateIncompatible`로 한 번 terminal 완료한다.
- Store record가 더 작은 chunk로 나뉘는지는 internals와 Store test가 검증하며 public E2E 성공 상한을
  바꾸지 않는다.

[RL-F13](../framework/common/e2e/config-5-resilience-lifecycle.ko.md#rl-f13-많은-large-state-units의-relocation을-bounded-terminal로-끝낸다)도
같은 boundary fixture를 사용하도록 맞춘다. SF-F7과 RL-F13이 서로 다른 oversize 결과를 요구하지 않게
한 번에 수정한다.

## 6. Internals와 중복 문서를 정리할 항목

### DEC-13. `messageFollow` 통지 중복 억제

#### 현재 문제

[Routing internals](../framework/common/internals/06-routing-and-cache.ko.md)은 suppression 방법을 구현을
구속하지 않는 후보라고 하지만, [wire protocol internals](../framework/common/internals/12-service-wire-protocol.ko.md)는
수명이 미결정이라고 적은 뒤 공통 검증을 요구하는 것처럼 읽힌다.

#### 권장 결정

다음 결과만 모든 runtime의 공통 불변 조건으로 둔다.

- Relay 뒤 source runtime에 `messageFollow`를 보낼 수 있다.
- 수신 cache가 통지의 source route와 같은 object·authority generation과 target을 가리킬 때만 지운다.
- 이미 더 새로운 route가 있으면 지우지 않는다.
- 통지가 유실되어도 cache lifetime이 끝나면 stale route가 만료된다.
- 통지 중복 여부가 application operation의 terminal 결과를 바꾸지 않는다.

세대마다 한 번만 보내기, in-flight merge와 marker 저장 위치는 구현 선택이다. `12`에서는
“아직 정하지 않았다”는 문장과 공통 후보를 제거하고 위 wire validation만 남긴다. `06`에는
성능 설계 예시로 남길 수 있지만 공통 완료 조건으로 사용하지 않는다.

### DEC-16. Embedded HTTP handler 중복 declaration

#### 현재 문제

[HTTP hosting의 handler](../framework/common/spec/server/languages/cpp/60-http-hosting.ko.md#3-handler-signature-형식)는
DI constructor를 포함한다. [Embedded HTTP](../framework/common/spec/server/languages/cpp/61-embedded-http-server.ko.md#7-binding-과-handler-통합)는
같은 handler signature를 그대로 사용한다고 하면서 constructor가 없는 class를 다시 선언한다.

#### 권장 결정

`61`의 중복 class declaration을 제거한다. `60`의 정식 handler signature로 연결하고, `61`에는 다음
server 처리 흐름만 남긴다.

1. Request DI scope를 만든다.
2. Handler와 dependency를 resolve한다.
3. `60`이 정한 우선순위로 `handle(...)`을 호출한다.
4. Typed result 또는 raw response를 HTTP response로 변환한다.

같은 C++ class를 두 문서에 복제하지 않으면 constructor와 dependency 목록이 다시 어긋나는 문제를
막을 수 있다.

## 7. 수정 순서

### Phase 0. 동결 후보와 수치 승인

다음 결정은 public API 제거 또는 추가를 포함하므로 구현 전에 승인이 필요하다.

- DEC-01 observer와 sink 제거
- DEC-02 diagnostics level 통일
- DEC-04 timer overrun 공통 계약
- DEC-08 object query exact interface parity
- DEC-09 per-send timeout 추가
- DEC-10 target 지정·부분 Relocate를 추가하지 않는 결정

DEC-04의 policy default·`MaxCatchUpTicks`, DEC-05의 JSON wire fixture, DEC-08의 page size·4 MiB와
DEC-17의 64 MiB는 source tree만 읽고 승인하지 않는다. 다섯 언어의 실제 배포 package와 Store fixture로
현재 wire·runtime 동작, memory bound와 clean consumer 결과를 확인한다. 승인한 signature, default,
닫힌 값과 수치는 §1.1의 범위로 동결한다.

승인 전에는 한 언어의 현재 구현이나 E2E만 근거로 정식 spec을 확장하지 않는다. DEC-01 제거 승인은
Phase 2의 대체 경로 E2E가 다섯 언어에서 통과한 뒤에만 확정한다.

### Phase 1. 정식 공통 계약 수정

1. Message flow의 logger provider record, Classic fanout dispatch-error 경계와 네 level을 확정한다.
   닫힌 `surface` 집합에는 `classic_fanout`을 추가하고, 이 surface에는 `channel_route_kind`를
   포함하지 않는 조건도 함께 고정한다.
2. Async worker의 I/O wait 불변 조건을 추가한다.
3. Timer overrun policy와 tick 정보를 추가한다.
4. `framework-json-v1`을 message model로 옮긴다.
5. ClientServer role 오류와 object query 상태 결과를 고정한다.
6. STREAM send별 timeout 의미와 값 범위를 추가한다.
7. Session logical disconnect와 rebind의 callback·tombstone 순서를 분명히 한다.

정식 exact interface는 target-first 원칙에 따라 observer·sink가 없는 목표 계약을 기록한다. 실제 package의
public symbol 제거는 Phase 2의 대체 diagnostics 경로를 증명하기 전에는 수행하지 않는다. 현재 구현과 목표
계약의 차이는 implementation gap으로 유지한다.

### Phase 2. Observer 제거 전 대체 경로 증명

Observer와 sink가 아직 존재하는 상태에서 RM-C5·PS-C1·RL-D2·SM-D9뿐 아니라 SM-B5·SM-E1·RL-D3의
대체 scenario도 logger provider와 structured record만 사용하도록 만든다. 다섯 언어에서 다음 결과를
실제 process E2E로 확인한다.

- RM-C5의 `no_handler` request·send 결과와 정상 후속 request
- PS-C1의 subscriber-local `zlink.dispatch_error`와 정상 fanout event
- RL-D2의 provider failure 격리와 제한된 fallback log
- SM-D9의 correlation·packet field와 handler evidence 일치
- SM-B5·SM-E1의 Actor·Spot `no_handler` dispatch error와 정상 후속 request
- RL-D3의 정식 dispatch-error field와 정상 handler 격리

하나라도 logger provider로 판정할 수 없으면 DEC-01을 승인하지 않고 observer·sink를 제거하지 않는다.

### Phase 3. 언어별 exact interface 정렬

1. Phase 2가 통과한 뒤 C++·Java·Node 구현과 package export에서 observer와 sink surface를 제거한다.
2. C++ diagnostics level을 네 단계로 맞춘다.
3. 다섯 언어 timer surface의 policy, default와 tick field가 공통 계약과 같은지 맞춘다.
4. C++·.NET·Node object query를 완성하고 Java 목차·상태 문장을 수정한다.
5. 다섯 언어 STREAM send call에 호출별 timeout을 추가한다. 언어별 cancellation 표면은 변경하지 않는다.
6. Java listener status 한영 의미와 error kind를 맞춘다.
7. C++ HTTP declaration 소유권과 embedded server 예제를 정리한다.

현재 구현이 목표 exact interface와 다르면 formal spec을 축소하지 않고 implementation gap에 기록한다.

### 문서 반영 결과

2026-08-07에 Phase 1, Phase 3의 exact-interface 문서 정렬과 Phase 4의 공통 E2E 문서 정합화를
완료했다. DEC-13~16의 internals·중복 문서 정리와 언어별 guide/reference 파급도 같은 변경에 포함했다.
첫 독립 리뷰에서 발견한 54개 citation drift, `messageFollow` 유실 경로 설명, observer 잔존 표현과
contract owner 오류를 수정했다. 후속 리뷰에서 발견한 regression matrix의 observer 조건과
RL-F7·ST-G2·ST-G4·ST-G6의 Actor/Spot-scoped Relocate 표현도 정식 public operation으로 교체했다.
Link, tab, 공통 산문의 언어 중립성, Instance Spot contract와 submit API 문서 gate가 통과했다.
같은 범위를 다시 검토한 독립 Codex Sol 리뷰는 남은 P0/P1 문서 gap이 없다고 판정하고 DEC-01~17과
공통 E2E 정합화 문서 반영을 최종 승인했다. 이 승인은 문서 계약과 E2E scenario 범위에만 적용한다.

Phase 2와 Phase 5는 문서 작성으로 대체할 수 없다. Application logger provider를 사용하는 다섯 언어
process E2E, 실제 public export 제거, contract test, package와 clean-consumer 증거는 open 상태다.

### Phase 4. 공통 E2E와 한영 drift 일괄 수정

[공통 E2E 인용과 표현 정합화 체크리스트](framework-e2e-citation-cleanup.ko.md)의 citation,
표현, 제목·역참조, fixture와 추가 계약 영향 항목을 이 Phase의 필수 작업으로 사용한다. 아래 DEC 직접 영향 항목만
수정하고 체크리스트의 나머지 config drift를 남기면 Phase 4는 완료가 아니다.

1. Observer 기반 RM-C5·RL-D2·SM-D9를 Phase 2에서 증명한 logger provider 판정으로 확정한다.
2. PS-C1은 정상 fanout trace가 아니라 subscriber-local dispatch error만 판정한다.
3. 공통 diagnostics level 이름을 사용한다.
4. STREAM auth를 application callback pattern으로 바꾼다.
5. CH-E2E-05 결과를 `NotConfigured`로 바꾼다.
6. SA-E2E-16은 짧은 per-call timeout으로 실패 send를 만든다.
7. IS-E2E-30과 ST-G3에서 target 지정·부분 Relocate 절차를 제거한다.
8. TA-A4는 logical disconnect operation을 사용한다.
9. SM-D9는 correlation ID와 packet field로 판정한다.
10. SF-F7과 RL-F13의 relocation state boundary를 같은 64 MiB fixture로 맞춘다.
11. RL-F12 fixture를 `SpotWide` User Spot 또는 Instance Spot으로 한정하고 queue·timer 복원 검증을
    유지한다. Entry Spot과 `PerActor` Spot-level timer는 성공 조건에서 제외한다.
12. 체크리스트의 54개 scenario citation과 spec 내부 stale anchor를 현행 spec heading 기준으로
    일괄 갱신한다.
13. SM-B5·SM-E1·RL-D3에서 public message-flow observer와 Framework-owned logging sink를 제거하고
    application logger provider가 받은 `zlink.dispatch_error`로 판정한다.
14. OBS-A1·OBS-A3·OBS-A5와 E2E README에서 C++ 전용 diagnostics level 이름을 공통
    `Off`, `Errors`, `Normal`, `Detailed`로 바꾸고 Framework diagnostics file path를 판정 수단으로
    사용하지 않는다.
15. RL-F4의 Server-only ClientServer 결과도 CH-E2E-05와 같은 `NotConfigured`로 맞춘다.
16. SM-D4A·SM-D4B·RL-F2에 DEC-11의 rebind callback, tombstone과 same-generation relocation 예외를
    반영한다.
17. RL-B1·RL-E4·SF-F11·TD-E2A·TD-F5·OBS-C12·SA-E2E-07·SA-E2E-17·SA-E2E-19의
    cancellation variant를 언어별 exact interface 적용 범위로 제한한다. 지원하지 않는 언어에 새
    cancellation API를 요구하지 않는다.
18. SF-C5A를 추가해 object query의 `Missing`, `Creating`, `Ready`, `Unavailable`과 Store 실패 시
    page 전체 실패를 검증한다. SF-C5·SF-F6의 pagination 책임과 섞지 않는다.
19. SM-E4는 callback 간격만 보지 않고 `DeliveryIndex`, `ScheduledIndex`, `SkippedTicks`를 policy별로
    검증한다.

수정 전 공통 E2E 전체에서 제거할 API 이름, C++ 전용 diagnostics level, per-send cancellation·deadline,
relocation target, object query state와 size limit을 검색해 영향 목록을 고정한다. 한국어 파일만 고치지 않고
영문 counterpart, README 완료 목록, feature map과 scenario index를 같은 변경에서 맞춘다. 나열한 scenario만
고친 뒤 같은 drift가 다른 config에 남는 부분 수정은 완료로 보지 않는다.
절 번호·anchor sweep은 기능 이름 검색과 별도로 실행한다. Config 8의 async spec 구판 절 번호,
Config 5·6의 Location·Relocation·Store·failover 인용과 spec 내부 link를 모두 확인하고, 존재하는 파일만
확인하는 데 그치지 않고 fragment가 현재 heading에서 실제로 생성되는지도 검증한다.

### Phase 5. 구현과 검증

각 결정은 다음 증거가 모두 있어야 닫힌다.

- 공통 spec과 한영 문서 의미 일치
- 다섯 언어 exact interface 또는 명시적인 언어 재사용 규칙
- public API snapshot과 contract test
- source package와 실제 배포 package의 export 일치
- 공통 E2E의 해당 scenario 통과
- 공통 E2E 인용과 표현 정합화 체크리스트의 모든 항목 완료
- 새 clone과 local package를 사용하는 clean consumer 검증
- 독립 리뷰에서 contract, implementation, package와 E2E gap이 없다는 판정

## 8. 문서 검증 장치

같은 종류의 drift를 다시 막으려면 다음 검사를 문서 변경 gate에 추가해야 한다.

1. 공통 diagnostics enum과 언어별 exact enum의 값·단계 mapping 검사
2. Exact interface 목차에 같은 디렉토리의 모든 계약 파일이 포함되는지 검사
3. 같은 언어에서 동일 public type을 여러 문서가 완전한 declaration으로 정의하는지 검사
4. 공통 E2E가 인용한 section과 identifier가 실제 spec에 존재하는지 검사
5. 공통 E2E가 public API 이름을 사용할 때 다섯 언어 exact interface에 대응 표면 또는 명시적인
   재사용 규칙이 있는지 검사
6. 한영 fenced signature와 enum 값뿐 아니라 error·timeout·completion 의미 문단의 대응 검사
7. 동결된 page·payload·timer·timeout 수치와 JSON fixture가 public package contract test에서 같은지 검사
8. 제거할 public symbol이 있으면 대체 경로의 다섯 언어 process E2E가 먼저 통과했는지 검사
9. STREAM send timeout parity를 검사하되 언어별 cancellation token의 존재 여부를 parity 대상으로
   오인하지 않는 검사
10. 공통 E2E의 cancellation variant가 exact interface에서 지원하는 언어만 필수 대상으로 삼는지 검사
11. Public object query contract test와 E2E가 상태 matrix와 Store 실패 시 partial page 금지를 모두
    검증하는지 검사

이 gate는 문서가 구현보다 먼저 목표 계약을 정한다는 원칙을 유지하면서도, 공통 spec과 E2E가
서로 다른 기능 집합을 요구하는 변경을 review 시점에 차단해야 한다.
