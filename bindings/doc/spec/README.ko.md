---
title: "바인딩 API 정책"
---

<!-- bindings-nav:start -->
[스펙 목록](README.ko.md)
<!-- bindings-nav:end -->

# 바인딩 API 정책

> **이 장이 정의하는 것** — `bindings/` 전체에 적용되는 공개 API 정책. 언어별
> 문서(`c/`, `cpp/`, `java/`, `dotnet/`, `node/`, `python/`, `go/`, `rust/`)가
> 이 정책을 기준으로 정렬한다.

> request-reply, SPOT routed, Actor dispatch 구현 기준은
> `core/include/zlink.h` 의 현재 공개 계약을 따른다.
> Actor dispatch는 SPOT처럼 service layer의 독립 공개 기능이며,
> 언어별 문서에 적힌 공개 표면도 이 공통 계약을 기준으로 정렬한다.
> 언어별 인터페이스 시그니처와 사용 예는
> `c/`, `cpp/`, `java/`, `dotnet/`, `node/`, `python/`, `go/`, `rust/` 를 참조한다.

| 절 | 다루는 내용 |
|---|---|
| [목적](#목적) | 이 문서의 범위와 Required/Target 표기 의미 |
| [바인딩 계약 범주 정책](#바인딩-계약-범주-정책) | contract 카테고리 분류 |
| [바인딩 런타임 범주 정책](#바인딩-런타임-범주-정책) | runtime 카테고리 분류 |
| [Actor/Spot Route 표면](#actorspot-route-표면) | route 조회 결과 타입과 Actor 대상 send/request |
| [고성능 바인딩 정책](#고성능-바인딩-정책) | hot path 제약 |
| [Substrate 와 공개 바인딩 표면](#substrate-와-공개-바인딩-표면) | part substrate와 aggregate 공개 표면의 경계 |
| [`*_part` Substrate 사용 의무 (Required)](#_part-substrate-사용-의무-required) | aggregate 구현이 `*_part` API를 써야 하는 이유 |
| [Spot Get-Or-Create 매핑](#spot-get-or-create-매핑) | `zlink_spot_node_spot_get_or_new` 매핑 규칙 |
| [공개 vs 내부 API 경계](#공개-vs-내부-api-경계) | contract/runtime 분리 원칙과 판정 기준 |
| [코어 정렬 규칙](#코어-정렬-규칙) | 코어 계약과의 정렬 규칙 |
| [Actor Dispatch 바인딩 계약](#actor-dispatch-바인딩-계약) | Actor dispatch 공개 표면 |
| [문서 해석 규칙](#문서-해석-규칙) | Required/Target 표기 해석법 |
| [핵심 원칙](#핵심-원칙) | 전체 정책을 관통하는 핵심 원칙 |
| [Monitor Ready 계약](#monitor-ready-계약) | monitor readiness 의미 |
| [POSD 구조 정책](#posd-구조-정책) | 깊은 모듈·낮은 변경 파급 구조 기준 |
| [공개 표면 규칙](#공개-표면-규칙) | operation 이름·builder·terminator 규칙 |
| [도메인 객체 정책](#도메인-객체-정책) | 값 타입 vs 인터페이스 판정 기준 |
| [소켓 타입 능력 정책](#소켓-타입-능력-정책) | socket family별 노출 능력 |
| [언어별 스펙 파일 준수 규칙](#언어별-스펙-파일-준수-규칙) | 언어별 문서와 이 문서의 관계 |
| [서비스 계층 정책](#서비스-계층-정책) | SPOT/Actor 서비스 계층 공개 계약 |
| [코어 API 추가 사항](#코어-api-추가-사항) | 최근 추가된 코어 기능의 바인딩 반영 |
| [옵션 정책](#옵션-정책) | socket/context 옵션 노출 규칙 |
| [성능 정책](#성능-정책) | 전체 바인딩 공통 성능 기준 |
| [경계 비용 정책](#경계-비용-정책) | FFI/marshalling 경계 비용 기준 |
| [Peer 가중치 정책](#peer-가중치-정책) | peer weight 계약 |
| [Monitor 정책](#monitor-정책) | monitor 이벤트·snapshot 계약 |
| [오류 정책](#오류-정책) | 오류 표현과 도메인 매핑 |
| [길이와 범위 경계 정책](#길이와-범위-경계-정책) | 값 검증과 경계 상한 |
| [소유권 정책](#소유권-정책) | 메시지·핸들 소유권 규칙 |
| [네이밍 정책](#네이밍-정책) | 언어 간 공통 네이밍 규칙 |
| [호환성 정책](#호환성-정책) | 호환 shim·deprecated wrapper 금지 |
| [언어 간 정렬](#언어-간-정렬) | 언어 간 일관성 검증 방법 |
| [테스트 정책](#테스트-정책) | 테스트 범위와 기준 |
| [Test Matrix](#test-matrix) | 언어 × 기능 커버리지 표 |
| [샘플 정책](#샘플-정책) | 샘플 코드 기준 |
| [Perf 정책](#perf-정책) | perf 러너 기준 |
| [스크립트 위치 정책](#스크립트-위치-정책) | 테스트/샘플/perf 스크립트 위치 |
| [리뷰 체크리스트](#리뷰-체크리스트) | PR 리뷰 시 확인 항목 |
| [POSD 기반 구현 완성 정책](#posd-기반-구현-완성-정책) | 구현 완성 판정 기준 |
| [구현 리뷰 체크리스트](#구현-리뷰-체크리스트) | 구현 완료 선언 전 확인 항목 |
| [바인딩 요구사항](#바인딩-요구사항) | 바인딩이 반드시 만족해야 할 요구사항 |
| [API 레퍼런스](#api-레퍼런스) | API 레퍼런스 문서 생성 기준 |
| [Routing ID로 Peer 끊기](#routing-id로-peer-끊기) | routing id 기반 peer disconnect 계약 |
| [관련 문서](#관련-문서) | 관련 문서 링크 |
| [Core API Surface 6.0.0 정렬](#core-api-surface-600-정렬) | 6.0.0 코어 API 표면 정렬 현황 |
| [Spot Route Bridge API](#spot-route-bridge-api) | route bridge API 계약 |

## 목적
이 문서는 `bindings/` 전체의 public API 정책을 정의한다.

이 문서의 목적은 각 언어 바인딩이 제각각 다른 표면과 예외 규칙을 갖는 것을
막고, `core/include/zlink.h`를 기준으로 설명 가능하고 일관된 공통 계약을
강제하는 데 있다.

이 문서는 현재 모든 바인딩이 이미 같은 상태라는 뜻이 아니다. `.NET` binding의
contract/runtime 분리와 파일 입도를 표준 target으로 삼아, 나머지 wrapper binding을
단계적으로 정렬하기 위한 기준이다. `Required`로 표시된 항목은 현재 리뷰에서 바로
적용하고, `Target`으로 표시된 구조와 표면은 해당 바인딩을 정렬하는 작업의 목표로
적용한다. C binding은 native ABI baseline이므로 별도 예외 규칙을 따른다.

`c/`, `cpp/`, `java/`, `dotnet/`, `node/`, `python/`, `go/`, `rust/` 아래 문서는
각 바인딩 구현이 실제로 외부에 제공해야 하는 public API contract를 정의한다.
이 문서들이 규정하는 것은 공개 타입, 메서드, 시그니처, 반환값, 오류 의미이며,
바인딩 구현이 노출하는 public 인터페이스는 이 계약과 달라지면 안 된다.
또한 C를 제외한 wrapper binding은 public contract와 runtime implementation을
분리한다. 이 분리는 역할 기준으로 고정되지만, 물리 디렉터리와 package/module
path는 언어 관례에 맞게 정한다. 언어별 README가 지정한 실제 경로가 각 바인딩의
구현 기준이다.

이 문서는 단순 스타일 가이드가 아니다. 다음을 위한 설계 기준 문서다.
- public API 설계 기준
- 리뷰 기준
- 리팩터링 기준
- 샘플과 테스트 기준

이 문서의 의도는 다음과 같다.
- 언어별로 이름만 비슷하고 의미가 다른 API를 없앤다.
- 같은 능력을 여러 방식으로 중복 노출하는 얕은 표면을 없앤다.
- raw option bag, 불필요한 편의 래퍼, 암묵적 ownership, 숨은 오류 경로를
  줄인다.
- binding 사용자가 internal sequencing, native 세부사항, hidden transport
  switch를 알지 않아도 되게 만든다.
- POSD 원칙에 맞는 깊은 모듈과 낮은 변경 파급(change amplification) 구조를 유도한다.
- correctness뿐 아니라 비용 모델, 샘플 품질, 테스트 가능성까지 공통 기준으로
  묶는다.

기준은 항상 `core/include/zlink.h` 이다. 각 바인딩은 코어 계약을 따르되,
표현 방식은 언어 관례에 맞게 선택할 수 있다. 다만 의미 계약은 바뀌면 안 된다.

이 문서는 "각 언어가 어떻게 보일 수 있는가"보다 "각 언어가 무엇을 보장해야
하는가"를 정의한다.

## 바인딩 계약 범주 정책

모든 바인딩은 public contract를 같은 의미 범주로 나누어야 한다. 실제 폴더,
package, namespace, module 이름은 언어 관례에 맞게 조정할 수 있지만, 어떤
public 타입이 어떤 범주에 속하는지는 바인딩마다 같은 기준으로 판단해야 한다.

이 정책의 목적은 파일 배치를 보기 좋게 맞추는 것이 아니다. 사용자가 한 언어에서
배운 개념을 다른 언어에서도 같은 위치와 같은 의미로 찾을 수 있게 만드는 것이다.
따라서 contract 하위 분류는 구현 파일 구조가 아니라 public API의 개념 경계를
따른다.

| 범주 | 목적 | 포함 대상 |
|------|------|-----------|
| `core` | 라이브러리 전체의 기반 계약 | `Context`, `ContextOptions`, `RoutingId`, 버전/기능 확인처럼 특정 socket이나 service에 묶이지 않는 public 타입 |
| `messaging` | 메시지 데이터와 수신 결과 계약 | `Message`, `Received`, topic message, subscription event, multipart payload helper처럼 socket 종류와 독립적인 payload 타입 |
| `sockets` | socket 종류와 socket 작업 계약 | `PairSocket`, `DealerSocket`, `RouterSocket`, `PubSocket`, `SubSocket`, `StreamSocket`, socket interface, send/recv/publish/request/reply builder, socket option |
| `eventing` | 대기, 이벤트 소스, 관찰 계약 | `Poller`, `PollEvent`, timer, monitor socket, monitor event, monitor snapshot |
| `service` | core service layer 계약 | Spot, Actor dispatch처럼 service domain에 속하는 public 타입 |
| `errors` | public 오류와 실패 표현 계약 | base exception, bind/connect/send/recv/submit/config/request exception, public error code/result mapping |

### 계약 범주 규칙

- `contract`와 `runtime`의 분리는 public API와 구현 세부사항의 분리다.
  contract 하위 범주는 runtime 내부 구조를 그대로 반영하면 안 된다.
- `core`는 작게 유지한다. 특정 도메인을 알아야 설명되는 타입은 `core`에 두지
  않고 해당 도메인 범주에 둔다.
- `service`는 `spot`, `actor` 같은 하위 도메인을 둘 수 있다.
  하위 도메인은 사용자가 독립 개념으로 배워야 할 때만 만든다.
- `eventing`은 monitoring만 뜻하지 않는다. poller, timer, monitor처럼 이벤트를
  기다리거나 관찰하는 public 계약을 함께 담는다.
- `errors`는 여러 도메인에 걸쳐 공유되는 오류 표면을 담는다. 특정 socket 작업의
  결과값처럼 도메인 의미가 강한 타입은 해당 도메인에 둔다.
- `enums` 같은 표현 형식 기준의 범주는 canonical category로 만들지 않는다.
  enum, flags, result는 그 값을 해석하는 public 개념의 범주에 둔다.
- operation, result, callback 보조 타입은 그 의미를 정의하는 도메인에 둔다.
  예를 들어 send/request/reply 결과와 callback은 messaging 계약에 두고, actor
  join/session/management 결과와 callback은 service 계약에 둔다. snapshot entry는
  그 snapshot을 반환하는 service 모델과 함께 둔다.

### 핸들러 등록 네이밍 정책

callback/handler 등록 함수 이름은 실제 동작을 드러내야 한다. 이벤트가 발생했을 때
호출되는 함수처럼 보이는 이름을 등록 함수에 쓰면, 사용자가 직접 구현해야 하는 hook
인지 handler를 저장하는 API인지 헷갈릴 수 있다.

- 한 subject에 handler 하나를 저장하거나 기존 handler를 교체하는 public API는
  `set...Handler` 계열 이름을 쓴다. 언어 관례에 따라 `Set...Handler`,
  `set...Handler`, `set_..._handler`처럼 표기한다.
- public binding의 `set...Handler`는 같은 subject에 활성 handler를 하나만 둔다.
  같은 setter를 다시 호출하면 현재 handler를 교체한다. raw native attach 충돌이나
  recv mode 충돌은 별도 오류로 보고할 수 있지만, public setter 이름은 누적 등록을
  뜻하지 않는다.
- 여러 handler를 누적 등록하는 public API만 `add...Handler` 또는
  `register...Handler` 계열 이름을 쓴다.
- `on...` 계열 이름은 이벤트가 발생했을 때 호출되는 protected/internal hook이나
  framework-level handler method에만 쓴다. handler 등록 함수의 canonical 이름으로
  쓰지 않는다.
- topic 구독처럼 프로토콜 상태를 바꾸는 API는 `subscribe` / `unsubscribe`를 쓸 수
  있다. 단순히 callback을 저장하는 함수에는 `subscribe...Handler`를 쓰지 않는다.
- callback을 `null`/`None`으로 설정해서 해제하는 표면은 만들지 않는다. 해제가
  필요하면 close/lifecycle 규칙으로 처리한다.

대표 canonical 의미 이름은 아래와 같다.

| 의미 | canonical 이름 |
|------|----------------|
| send ready handler 등록 | `setSendReadyHandler` |
| raw STREAM packet handler 등록 | `setPacketHandler` |
| SPOT dispatch event handler 등록 | `setDispatchHandler` |
| SPOT routed receive | `recvRouted` |
| SPOT Actor lifecycle receive | `recvActorLifecycle` |

대표적인 enum/result/flags 배치 기준은 아래와 같다.

| 타입 예 | 범주 | 이유 |
|---------|------|------|
| `SendFlags`, `RecvFlags`, `SubmitResult`, `RecvResult` | `sockets` | socket 작업의 입력 또는 결과를 설명한다. |
| `PollEventFlag`, `PollSourceKind`, `MonitorEventType` | `eventing` | 이벤트 대기와 관찰 결과를 설명한다. |
| `SpotDispatchEvent`, `SpotPeerKind` | `service.spot` | Spot service domain 안에서만 의미가 정해진다. |
| `ConfigResult`, `ErrorCode` | `errors` | 여러 도메인에서 공유되는 실패 의미를 설명한다. |

모든 wrapper binding은 같은 아키텍처 지도를 공유한다. 이 지도는 특정 언어의
폴더명을 그대로 복사하기 위한 기준이 아니라, 한 언어에서 배운 개념을 다른 언어의
코드에서도 같은 책임 위치에서 찾을 수 있게 만드는 기준이다. 실제 파일명,
디렉터리명, package/module/import path는 언어별 관례와 공개 API 안정성을 따른다.

공유 아키텍처 지도는 아래와 같다.

```text
contracts/
  core/
  messaging/
  sockets/
  eventing/
  service/
  errors/

runtime/
  native/
  sockets/
  messaging/
  eventing/
  service/
  errors/
  buffers/
  options/
  handles/
```

이 지도에서 `contracts`는 사용자가 먼저 읽는 public contract surface다.
`contracts`에는 public interface, public value object, public result/flag/error
type, public builder/facade처럼 사용자가 직접 의존하는 타입을 둔다. 구현 세부사항은
`runtime`에 숨긴다.

다만 public interface를 남발하지 않는다. `Message`, `RoutingId`, `Received`,
`TopicMessage`, enum/result/flags 같은 값 객체나 단순 데이터 타입은 별도 interface로
쪼개지 않는다. interface는 사용자가 다형적으로 받아야 하는 역할에만 둔다. 예를
들어 socket 공통 역할, poll target, monitor target, codec, handler/callback,
SPOT client 역할이 이에 해당한다.

`runtime`은 public contract를 실행하는 구현 영역이다. socket send/recv 흐름,
message materialization, poller/timer/monitor loop, service runtime, native
interop, buffer/handle/error mapping 같은 구현 결정을 숨긴다. runtime 타입은
public API로 권장하지 않으며, contract surface를 통하지 않고 사용자가 직접 의존하면
안 된다.

### 인터페이스 / 구현 분리 정책

C를 제외한 wrapper binding은 `.NET` binding처럼 공개 인터페이스/계약과 기본 구현을
분리하는 방향을 따른다. 여기서 `.NET`처럼 분리한다는 말은 모든 언어가 `IContext`나
`Contracts/Runtime` 같은 이름을 그대로 복사한다는 뜻이 아니다. 사용자가 보는
계약과 native 호출, handle owner, callback bridge, request pump 같은 구현 세부사항을
서로 다른 책임 영역에 둔다는 뜻이다.

분리 기준은 아래와 같다.

- 사용자가 의존하는 타입, interface, trait, protocol, abstract role, factory,
  builder 시작점, DTO, value object, enum, error/result type은 public contract
  source에 둔다.
- native handle을 직접 소유하거나, core helper substrate를 호출하거나, callback
  trampoline과 request progress를 관리하거나, marshalling을 수행하는 타입은 runtime
  또는 native bridge source에 둔다.
- `Context`, socket, poller, timer, SpotNode, Spot, Actor처럼
  사용자가 구현체보다 역할에 의존하는 편이 자연스러운 resource type은 언어가
  지원하는 방식으로 contract role과 default implementation을 분리한다.
- `Message`, `RoutingId`, `Received`, `TopicMessage`, snapshot DTO, enum/flags/result
  같은 값 중심 타입은 의미 없는 interface/trait/protocol로 감싸지 않는다. 값 타입은
  concrete public type으로 두고, 내부 native-backed storage가 필요하면 구현 세부사항을
  타입 내부에 숨긴다.
- runtime concrete class가 public으로 노출되어야 하는 언어라도, 사용자가 이해해야
  하는 동작 계약은 public contract source에서 먼저 설명되어야 한다.
- sample, perf, framework adapter는 runtime concrete type이나 native bridge가 아니라
  public contract projection을 기준으로 작성한다.

이 분리는 이름만 나누는 것이 아니다. `contracts` 쪽 파일은 native handle, native
function 이름, request pump, callback trampoline, buffer marshalling 순서를 몰라도
읽을 수 있어야 한다. 반대로 `runtime` 쪽 파일은 public contract를 구현하지만,
그 자체가 사용자가 import해야 하는 public surface가 되면 안 된다.

파일 구조에서도 같은 기준을 적용한다.

- category aggregate 파일은 작은 re-export barrel이나 factory wiring에만 쓴다.
  `sockets`, `service`, `eventing` 같은 category 파일 하나가 여러 public resource의
  실제 동작을 모두 담고 있으면 contract/runtime 분리가 된 것이 아니다.
- native-backed resource 구현은 resource별 파일에 둔다. 예를 들어 socket family,
  poller, timer, SpotNode, Spot, Actor는 각각의 구현 파일을
  가져야 한다. 파일명은 언어 관례에 맞추되 resource 이름이나 operation 이름을
  드러내야 한다.
- 공통 helper 파일은 public resource 구현의 대체 장소가 아니다. helper 파일에는
  native call wrapper, handle validation, marshalling helper, 오류 매핑처럼 여러
  구현이 공유하는 하위 기능만 둔다. `Context`, `RouterSocket`, `SpotNode`, `Poller`
  같은 resource의 동작 본문을 helper 파일에 모아두면 안 된다.
- contract 파일과 runtime 파일은 서로 1:1일 필요는 없지만, public resource 하나를
  찾을 때 contract owner와 runtime owner가 각각 명확해야 한다.

### 파일 입도 정책

모든 wrapper binding은 파일을 나누는 입도도 비슷하게 맞춘다. 목표는 파일 개수나
파일명을 1:1로 복제하는 것이 아니라, 어느 언어를 읽어도 같은 개념 묶음을 비슷한
크기의 파일에서 찾을 수 있게 하는 것이다.

기준은 `.NET` binding의 `Contracts` 정리 수준을 따른다. 한 파일은 하나의 독립된
public 개념이거나, 같은 변경 이유를 공유하는 작고 강하게 붙은 계약 묶음을 담는다.

파일 분할 기준은 아래와 같다.

- `Context`, socket family, `SpotNode`, `Spot`, `Actor`, poller, timer처럼 사용자가
  직접 찾는 resource 계약은 얇어도 독립 파일로 둘 수 있다.
- `Message`, `Received`, `TopicMessage`, `RoutingId`처럼 소유권, 저장소, 값 검증,
  비용 모델이 중요한 타입은 단독 파일로 둔다.
- marker interface, delegate, 작은 enum, 한 줄 record처럼 단독으로 변경 이유가 약한
  타입은 가장 가까운 계약 파일에 합친다. 예를 들어 socket marker role은 socket
  base 계약과, stream packet handler delegate는 stream socket 계약과 함께 둔다.
- send/request/reply 같은 staged operation builder 계약은 같은 도메인 변경 이유를
  공유하므로 한 operation contract 파일로 묶을 수 있다.
- Actor join, actor management, SpotNode snapshot model처럼 서비스
  하위 도메인이 뚜렷한 묶음은 도메인별 파일로 둔다. 단, 모델 파일이 너무 커져서
  peer/status/socket/actor snapshot처럼 서로 다른 변경 이유가 생기면 그때 나눈다.
- runtime 구현 파일도 같은 원칙을 따른다. 구현 파일 하나가 여러 native-backed
  resource의 lifecycle, send/recv/request 흐름, callback 등록, snapshot mapping을
  동시에 담고 있으면 너무 넓다. 그런 파일은 resource 구현 파일과 shared helper
  파일로 나눈다.
- category barrel은 작아야 한다. 대략적인 기준으로, re-export와 단순 factory wiring을
  넘어 수백 줄의 resource implementation이 들어가면 분리 실패로 본다. 실제 기준은
  줄 수보다 변경 이유다. 서로 다른 public resource를 수정할 때 항상 같은 파일을
  열어야 한다면 분리해야 한다.
- `Enums`, `Types`, `Models`, `Common`, `Utils`처럼 표현 형식이나 포괄 이름만 기준으로
  파일을 만들지 않는다. 파일명은 사용자가 찾는 도메인 개념이나 변경 이유를 드러내야
  한다.
- 각 언어는 casing, suffix, package convention을 따른다. 예를 들어 C#은
  `OperationContracts.cs`, Rust는 `operation_contracts.rs`, TypeScript는
  `operation-contracts.ts`처럼 표현할 수 있지만, 같은 책임 묶음이라는 점은 유지한다.

이 기준을 적용할 때는 언어별 README가 선언한 정렬 방식을 우선한다. 언어별 README가
breaking alignment를 선언하면 기존 public surface 유지보다 canonical surface 정렬을
우선한다. 파일 이동은 가능한 한 namespace, package export, crate re-export, package
`exports`, generated declaration surface를 깔끔하게 재정렬한다. 파일 구조를 맞추기 위해
새 public wrapper나 얕은 compatibility shim을 만들지 않는다.

언어별 적용은 다음 원칙을 따른다.

- `.NET`은 `Contracts/<Category>`와 `Runtime/<Category>`를 표준 구조로 삼는다.
  public interface와 public value object는 `Contracts`에 두고, 구현 class와 native
  interop helper는 `Runtime`에 둔다.
- Java는 package가 public API에 가깝기 때문에 `systems.zlink.contracts.<category>`
  아래에 public interface/value object를 두고, 구현 class와 native bridge는
  `systems.zlink.runtime.<category>` 또는 `systems.zlink.runtime.nativeapi` 아래에
  둔다. 단순히 method 목록을 보기 위한 `FooContract` interface는 만들지 않는다.
- C는 native ABI baseline이므로 별도 contract/runtime 폴더를 만들지 않는다.
  header 파일, header section, 문서 섹션으로 같은 범주를 표현한다.
- C++, Go, Rust, Python, Node는 각 언어의 module/package/export 관례를 따르되,
  public-facing surface와 runtime implementation을 구분해야 한다. 언어가 interface를
  자연스럽게 지원하지 않으면 문서와 export surface에서 같은 구분을 명확히 한다.

기존 바인딩에 `monitoring` 또는 `Monitoring` 범주가 있으면 canonical category는
`eventing`으로 본다. monitor API만 있던 시기에는 monitoring 이름이 충분했지만,
poller와 timer까지 함께 다루는 public 계약에서는 eventing이 더 넓고 정확한
개념이다. 구조 정리 시에는 새 문서와 새 파일을 `eventing` 책임으로 설명한다.
이미 공개 import/export 경로가 된 `monitoring` 이름은 해당 언어별 README가
호환성 유지를 명시한 경우에만 임시 alias로 둘 수 있다. breaking alignment를 선언한
바인딩에서는 `eventing`으로 정리하고 `monitoring` alias를 남기지 않는다.

`enums` 같은 표현 형식 기준 폴더는 공유 아키텍처 지도의 최상위 범주가 아니다.
enum, flags, result, literal union은 그 값을 해석하는 도메인 범주에 둔다. 예를 들어
`RecvFlags`는 `sockets`, `PollEventFlags`는 `eventing`, `SpotPeerKind`는 `service`
계약에 속한다.

## 바인딩 런타임 범주 정책

wrapper binding은 public contract와 runtime implementation을 분리한다. runtime은
contract surface 뒤에서 실제 동작을 수행하는 구현 계층이다. public contract는
사용자가 무엇을 호출할 수 있는지 보여 주고, runtime은 그 호출을 native substrate와
언어별 실행 모델에 맞게 처리한다.

runtime 하위 범주는 contract 하위 범주와 1:1로 반드시 같을 필요는 없다. 하지만
구현 책임과 변경 이유가 분명해야 하며, public contract를 단순 반복하는 얇은
pass-through class를 늘리면 안 된다.

권장 runtime 범주는 아래와 같다.

| 범주 | 책임 |
|------|------|
| `native` 또는 언어별 동등 이름 | P/Invoke, JNI, FFI, native 함수 선언, ABI 타입 변환, native symbol loading |
| `handles` | native handle 소유권, dispose/close, lifetime, reference tracking |
| `messaging` | native message part 조립, multipart 처리, message 변환, request progress |
| `sockets` | socket operation 실행, send/recv/publish/request/reply 흐름 |
| `eventing` | poller, timer, monitor, event dispatch loop |
| `service` | Spot, Actor service runtime |
| `options` | public option 검증, native option mapping |
| `errors` | native errno/result를 public exception/result로 변환 |
| `buffers` | byte buffer, direct buffer, pooled buffer, pinned memory, copy/borrow 정책 |

언어 예약어 때문에 이름이 달라질 수 있다. 예를 들어 Java는 `native`가 keyword이므로
`runtime/nativeapi`를 사용할 수 있다. 이름은 달라도 책임은 설명 가능해야 한다.

### 런타임 범주 규칙

- runtime은 public API 안정성을 약속하지 않는다. public 계약은 contract 문서와
  언어별 public surface가 정의한다.
- runtime 구현 class는 public contract interface 또는 public facade 뒤에 숨긴다.
  사용자가 runtime class를 직접 생성하거나 호출해야 하면 contract 설계가 새고 있는
  것이다.
- contract interface가 runtime 구현과 메서드 목록만 1:1로 반복되면 얕은 모듈 위험
  신호다. interface는 역할 추상화가 있을 때만 만들고, 값 객체에는 만들지 않는다.
- runtime 범주는 변경 이유를 기준으로 나눈다. 예를 들어 native symbol 추가는
  `native`, send/recv 흐름 변경은 `sockets`, message ownership 변경은 `messaging`
  또는 `buffers`가 바뀌어야 한다.
- `core`, `common`, `utils`, `internal`, `misc` 같은 포괄 이름은 canonical runtime
  category로 쓰지 않는다. 이런 이름은 서로 다른 변경 이유를 한곳에 섞기 쉽다.

기존 runtime에 `monitoring` 또는 `Monitoring` 범주가 있으면 contract와 마찬가지로
canonical category는 `eventing`이다. monitor 구현만 담긴 파일이라도 poller, timer,
event dispatch loop와 같은 변경 이유를 공유한다면 `eventing` 아래에 둔다.

POSD 관점에서 이 기준은 public surface의 가독성과 구현 정보 은닉을 함께 얻기 위한
것이다. contract는 사용자가 배워야 할 작고 명확한 표면을 제공하고, runtime은 native
호출 방식, handle ownership, buffer pooling, 오류 매핑 같은 구현 결정을 아래로
흡수한다. 단, 추상화가 실제 역할을 줄이지 못하고 메서드 목록만 반복하면 오히려
복잡성이 늘어나므로 만들지 않는다.

## Actor/Spot Route 표면

모든 바인딩은 core의 Actor route와 Spot route 결과를 손실 없이 노출해야 한다.
언어별 타입 이름은 달라도 아래 의미는 유지한다.

- Actor route는 Actor ref의 node rid, current Spot rid, current Spot kind를
  노출한다.
- Spot route는 조회한 Spot rid, owner node rid, Spot kind를 노출한다.
- Spot kind는 Entry Spot, user Spot, invalid 값을 구분한다.
- 바인딩은 `router -> actor` 또는 `actor -> router` direct API를 새로 만들지 않는다.
  사용자는 route 조회 결과를 기존 Spot routed API와 조합한다.

## 고성능 바인딩 정책

zlink는 고성능 메시징 라이브러리다. 바인딩은 언어별 편의성을 제공하더라도
hot path의 비용 모델을 숨기거나 악화시키면 안 된다. 공개 API와 내부 구현은
아래 원칙을 따라야 한다.

- 메시지 send/recv, publish/subscribe, request/reply, dispatch callback,
  poller, timer 경로에서는 reflection 기반 동적 호출을 사용하지 않는다.
  언어 런타임이 reflection을 필수로 요구하는 경우에도 초기화나 바인딩 등록
  단계로 제한하고, 메시지 처리 루프에서는 사용하지 않는다.
- reflection은 누락 API를 맞추기 위한 우회 수단이 아니다. 고성능 바인딩은
  typed facade, 직접 native downcall, 직접 내부 bridge를 사용해야 하며,
  public 계약을 맞추기 위해 hot path에 reflective lookup을 추가하면 안 된다.
- 불필요한 메모리 할당을 만들지 않는다. 반복 호출에서 같은 크기의 임시 배열,
  wrapper, closure, boxing 객체를 매번 새로 만들면 안 된다.
- 불필요한 복사를 만들지 않는다. core에서 받은 메시지 part는 가능한 한 바로
  언어별 `Message` 소유 객체로 옮기고, decode나 사용자 요청 없이 byte buffer를
  다시 복사하지 않는다.
- hot path에 전역 lock, coarse lock, 불필요한 mutex 경합, shared executor
  직렬화 지점을 두지 않는다. 필요한 동기화는 subject별 상태를 보호하는 최소
  범위로 제한한다.
- callback, dispatch, poller, timer, request completion 진행 경로에서 숨은
  blocking wait, sleep, busy wait, thread join을 수행하지 않는다. 명시적으로
  blocking API로 문서화된 호출만 대기할 수 있다.
- binding은 core의 `*_part` substrate를 사용해 part 단위로 언어 객체를 구성한다.
  aggregate native 배열을 만든 뒤 다시 언어별 collection으로 변환하는 이중
  materialization은 금지한다.
- 성능 검증용 perf, sample, test도 public binding entrypoint만 사용하면서
  위 비용 모델을 깨지 않아야 한다.

이 절은 구현 세부 최적화 권고가 아니라 public binding 적합성 조건이다.
리뷰에서 reflection hot path, 불필요한 할당/복사, 스레드 경합, 숨은 대기가
확인되면 해당 바인딩은 정책 미준수로 본다.

## Substrate 와 공개 바인딩 표면

bindings 구현은 core가 제공하는 helper substrate C API(`*_part` 계열) 위에 올라간다.
bindings 사용자에게 노출되는 public API는 그 helper 시그니처를 그대로 따라야 한다는
뜻이 아니다. 다만 내부 구현이 어떤 core 함수를 호출하는가는 아래 규칙으로 고정한다.

이 문서는 아래 경계를 기준으로 해석한다.

- `core/include/zlink.h` 의 `*_part` helper substrate 계약은
  bindings 구현이 반드시 사용해야 하는 native substrate다.
- `doc/spec/bindings/` 아래 문서는 각 언어 binding이 외부에 제공하는
  **public convenience contract**만 정의한다.

즉 binding public API는 helper substrate와 모양이 달라도 된다. 그러나 내부에서
core를 호출하는 방식은 달라서는 안 된다.

예를 들면 아래 구조가 요구된다.

- core substrate는 `*_part`, `has_more`, caller-provided `zlink_msg_t`
  같은 primitive한 표면을 가진다.
- Java, `.NET`, `Go`, `Rust`, `Python`, `Node`, `C++`, C binding은 그 위에
  `Received`, `Message`, collection, request/reply convenience 같은
  언어 친화적 public API를 올린다.
- public API 내부에서 core를 직접 호출하는 경로는 반드시 `*_part` substrate를 사용한다.
  aggregate 형태의 core 함수(`zlink_send`, `zlink_recv`, `zlink_publish` 등)를
  binding 내부에서 호출하면 안 된다.

아래 조건은 반드시 지켜야 한다.

- binding public API의 의미 계약은 core 계약으로 설명 가능해야 한다.
- helper substrate에만 있는 low-level 세부사항을 binding 사용자에게 직접 노출하지
  않는다.
- `RecvPart`, `RecvRoutedPart`, `SubscribePart`, `recv_part`,
  `recv_routed_part`, `subscribe_part`처럼 part 단위 수신을 public binding
  API로 노출하지 않는다. part loop, `has_more`, part별 envelope metadata는
  binding runtime 내부에서 aggregate 결과 저장소로 흡수한다.
- `doc/spec/bindings/` 문서는 helper substrate 시그니처 자체를 public contract로
  문서화하지 않는다.
- helper substrate는 bindings 구현과 성능 최적화를 위한 기반 계층으로만 취급한다.

즉 bindings 정책 문서의 기준은 "helper가 어떻게 생겼는가"가 아니라,
"binding 사용자가 최종적으로 어떤 public contract를 보게 되는가"이다.

## `*_part` Substrate 사용 의무 (Required)

send, request, reply, publish, subscribe 계열 함수의 내부 구현은 반드시
core의 `*_part` helper substrate를 사용해야 한다. 이는 `Required` 규칙이다.

### 적용 대상

아래 계열에 해당하는 모든 binding 내부 구현 경로에 적용한다.

- send (단일 part, 복수 part, routed 포함)
- recv (단일 part, 복수 part, routed 포함)
- request (dealer, router, SPOT 계열 포함)
- reply (router, SPOT 계열 포함)
- publish
- subscribe (SPOT subscribe 포함)

### 이유

core가 aggregate 함수와 `*_part` substrate를 모두 제공하던 시기에는 aggregate 함수를
직접 호출하는 것이 허용됐다. 그러나 이 구조는 아래 비용을 만든다.

- core가 먼저 native aggregate (parts 배열) 를 구성한다.
- binding이 그 aggregate를 다시 언어별 객체(`Message[]`, `Received`, value object)로
  변환한다.
- 결과적으로 "native aggregate 생성 → 언어 객체 aggregate 생성"이 연속으로 일어나며,
  이 이중 변환 비용이 hot path의 실질적인 병목이 된다.

`*_part` substrate를 직접 사용하면 binding이 part 하나씩 언어 객체로 직접 변환할 수
있고, native aggregate 생성 단계를 완전히 제거할 수 있다. 이는 특히 Java, .NET처럼
객체 materialization 비용이 큰 언어에서 측정 가능한 성능 차이를 만든다.

이 규칙은 구조 정리 목적이 아니라 **런타임 성능 비용을 실질적으로 줄이기 위한** 요구사항이다.

### public API 형태는 유지

이 규칙은 내부 구현 기반에 관한 것이다. binding 사용자가 보는 public API 형태는
이 규칙과 무관하게 각 언어 spec이 정한 대로 유지한다.

- 사용자는 `send(List<Message>)`, `recv()`, `request(...)` 같은 언어 친화적 API를 그대로 쓴다.
- `*_part` 호출 시퀀스는 binding 내부 구현 세부사항이며, 사용자에게 노출하지 않는다.
- public binding의 receive 표면은 `recv`, `subscribe`, `recvRouted` 같은
  aggregate 결과 저장소 API만 제공한다. `RecvPart`/`SubscribePart` 계열은
  성능 최적화 substrate의 이름일 뿐 public contract 이름이 아니다.

## Spot Get-Or-Create 매핑

Core는 "routing id로 local logical Spot을 가져오거나, 없으면 생성한다"는 원자 계약을
위해 `zlink_spot_node_spot_get_or_new(...)`를 제공한다.

모든 상위 바인딩은 자신의 public get-or-create SpotNode API를 그 C 함수에 직접
매핑해야 한다. 같은 동작을 모방하기 위해 `spot_lookup()` 과 `create_spot()` 을
조합해서는 안 된다. 그렇게 하면 core의 원자성 계약을 잃고 lookup/create race가
다시 들어오기 때문이다.

언어별 이름은 다음과 같다.

- C++: `spot_node_t::get_or_create_spot(...)`
- .NET binding: `SpotNode.GetOrCreateSpot(...)`
- Java: `SpotNode.getOrCreateSpot(...)`
- Node: `SpotNode.getOrCreateSpot(...)`
- Go: `SpotNode.GetOrCreateSpot(...)`
- Rust: `SpotNode::get_or_create_spot(...)`
- Python: `SpotNode.get_or_create_spot(...)`

각 wrapper는 소유된 `Spot` facade와 이 호출이 logical spot을 생성했는지 여부를
함께 반환한다. 반환된 facade는 해당 언어의 일반적인 Spot lifetime 규칙을 따른다.

### 준수 확인

구현 리뷰와 검증 단계에서 아래를 확인한다.

- binding 소스에서 aggregate 심볼(`zlink_send`, `zlink_recv`, `zlink_send_rid`,
  `zlink_publish`, `zlink_subscribe`, `zlink_router_recv`, `zlink_dealer_request`,
  `zlink_router_request`, `zlink_router_reply`, `zlink_spot_send_*`,
  `zlink_spot_request_*`, `zlink_spot_reply_*`, `zlink_spot_subscribe` 등)을
  직접 호출하는 경로가 없어야 한다.
- 대신 대응하는 `*_part` 심볼(`zlink_send_part`, `zlink_recv_part`,
  `zlink_send_part_rid`, `zlink_publish_part`, `zlink_subscribe_part`,
  `zlink_router_recv_part`, `zlink_dealer_request_part`, `zlink_router_request_part`,
  `zlink_router_reply_part`, `zlink_spot_*_part` 등)을 사용해야 한다.
- 미준수 시 리뷰에서 차단된다.

## 공개 vs 내부 API 경계

각 binding은 public contract와 internal implementation surface를 분리해야 한다.
이 문서와 각 언어별 README는 public API의 경계와 라이브러리 모양을 정의한다.
정확한 함수, 메서드, 타입 목록은 C를 제외한 wrapper binding의 언어별 README가
지정한 public contract source가 소유한다. C++와 .NET은 이 위치가 실제
`Contracts/` 폴더이고, Java, Node, Python, Go, Rust는 각 언어 README가 지정한
package, module, export surface가 그 역할을 한다. 설치 헤더, package entrypoint,
`.d.ts`, `__init__.py`, `lib.rs` re-export는 이 계약을 사용자에게 노출하는
projection이다. C는 예외로, `core/include/zlink.h`가 public C ABI의 단일 기준이다.

아래 원칙은 모든 binding에 공통으로 적용한다.

- 언어별 public contract source에 포함되지 않은 타입, 함수, 메서드, 모듈,
  패키지, 네임스페이스는 모두 internal implementation detail로 본다.
- 언어별 README는 모든 public member를 반복해서 나열하지 않는다. 대신
  public contract source 위치, source layout, API 변경 절차, runtime/internal
  경계, 성능 정책을 정의한다.
- internal API는 이름만 internal처럼 보이게 두면 충분하지 않다. 가능한 언어는
  패키지 export, module export, assembly visibility, crate re-export,
  package `exports`, `internal/` directory 같은 언어 고유 경계를 사용해
  실제로 접근을 제한해야 한다.
- perf, sample, test도 원칙적으로 public binding entrypoint만 사용해야 한다.
  같은 저장소 안에 있다고 해서 internal helper를 직접 import하거나 참조하면
  안 된다.
- public contract 검증은 배포된 binding consumer가 보게 되는 entrypoint를
  기준으로 한다. 소스 트리 내부에 internal symbol이 존재한다는 이유로 public으로
  간주하지 않는다.
- C++처럼 설치 헤더와 컴파일된 바인딩 라이브러리를 함께 배포하는 바인딩은
  설치되는 `include/` tree 안에 공개 `Contracts/`를 두고, 구현은
  `bindings/cpp/src/Runtime/` 아래 비공개 파일로 숨긴다. aggregate 헤더는
  유지할 수 있지만, public class를 찾는 유일한 진입점이 되어서는 안 된다.
- internal 구조를 리팩터링할 자유는 보장하되, 그 자유는 public contract를
  유지하는 범위 안에서만 허용된다.

즉 이 문서의 목적은 public API 경계와 라이브러리 모양을 정의하는 것뿐 아니라,
public이 아닌 API를 public처럼 사용하지 못하게 경계를 강제하는 것까지 포함한다.

### 언어별 계약/런타임 분리

C를 제외한 wrapper binding은 public contract와 runtime implementation을 분리해야
한다. 단, 분리 방식은 그 언어의 package, module, import path 규칙을 따라야 한다.
언어별 README는 실제 repository path와 실제 package/module path를 함께 지정해야
한다. `Contracts`와 `Runtime`은 공통 logical category 이름이며, 모든 언어에서
그 단어를 그대로 public package 또는 import path로 만들라는 뜻이 아니다.
C++는 C++20 바인딩이며, 공개 계약 root는 `bindings/cpp/include/zlink/Contracts/`이고
런타임 구현 root는 `bindings/cpp/src/Runtime/`이다.
Java는 package path가 곧 source folder이므로 `systems.zlink.contracts.*`와
`systems.zlink.runtime.*` 같은 lower-case Java package로 역할 구조를 드러낸다.
Go, Rust, Python처럼 folder path가 package/module/import path와 직접 연결되는
언어도 실제 package/module tree 안에서 public 계약과 runtime 구현을 분리한다.
Node/TypeScript는 `package.json` exports가 public 경계를 정하지만, source folder
이름도 deep import 오해를 만들 수 있으므로 언어별 README의 실제 source path와
package export 규칙을 함께 따른다.

C는 native C ABI baseline이다. C public contract는 `core/include/zlink.h`이고,
`bindings/c`는 그 C API를 기준으로 sample, test, perf, packaging과 필요한 mapping
정책을 정렬한다. C에는 별도 `Contracts/`와 `Runtime/` 계층을 강제하지 않는다.

`Contracts`는 사용자가 확인해야 하는 public contract source 역할이다. `Runtime`은
native handle, callback bridge, request progress pump, helper substrate 호출,
object lifetime 보정 같은 구현 세부사항 역할이다. `Native`는 FFI, P/Invoke,
JNI/Panama, N-API, cgo 같은 native bridge 전용 역할이다. C++와 .NET처럼 문서가
명시한 언어에서는 이 역할 이름을 실제 폴더명으로 사용하고, 다른 언어에서는
언어별 README가 지정한 package/module/export 구조로 같은 역할을 표현한다.

`Contracts`와 `Runtime`은 공통 역할 이름이다. 이것이 곧 public package,
namespace, module, import path 이름이라는 뜻은 아니다. 디렉터리 구조가 언어의
package/module 경로에 직접 영향을 주는 언어는 `Contracts`나 `Runtime`을 public
import path로 노출하지 않는다. 대신 public package/module tree 안의 실제 경로로
계약을 배치하고, runtime 구현은 `internal`, private module, unexported module,
`pub(crate)` module 같은 언어별 비공개 경계 안에 둔다.

#### 계약 / 런타임 배치 규칙

아래 기준은 C를 제외한 wrapper binding에 적용한다. 새 public API를 추가하거나
구현을 옮길 때 이 표를 먼저 확인한다.

| 항목 | 위치 |
|---|---|
| 사용자가 호출하거나 타입으로 참조하는 공개 동작 계약 | public contract source의 해당 category |
| public constructor, factory, builder 시작점의 계약 | public contract source의 해당 category |
| public free function, static facade, extension helper, module function | public contract source의 해당 category |
| public builder convenience method or helper | public contract source의 해당 category |
| DTO, value object, enum, public error/result type | public contract source의 해당 category |
| runtime concrete class, socket kernel, handle owner | runtime/internal source의 해당 category |
| request progress pump, callback trampoline, part-loop helper | runtime/internal source의 해당 category |
| native handle wrapper, FFI declaration, struct mirror, marshalling helper | native bridge source |
| generated native loading code, platform artifact lookup | native bridge source |

판정 규칙은 아래와 같다.

- public contract 타입의 public signature는 native bridge 타입을 참조하지
  않는다.
- runtime/internal source에 user-facing method가 필요해지면 먼저 public contract
  source에 계약을 추가한다. runtime 구현은 그 계약을 구현하거나 projection한다.
- 사용자가 직접 호출하는 helper가 class method, static method, free function,
  extension method, module function 중 어떤 모양이든 public 이면 public contract
  source에 계약을 둔다. 단순 편의 함수라는 이유로 runtime 전용 위치에 남기지
  않는다.
- public factory가 runtime concrete type을 반환할 수는 있다. 단 그 생성 동작과
  반환 타입의 사용자 관찰 가능 동작은 public contract source에서 설명 가능해야
  한다.
- runtime/internal 폴더명, module path, package path 자체를 public API로 노출하지
  않는다. 다만 `Context`, socket, `SpotNode`, `Poller`, `Timer` 같은 기본 구현
  클래스나 타입은 언어별 public projection으로 노출할 수 있다. 이 경우 사용자가
  관찰하는 public behavior는 public contract source에서 설명 가능해야 한다.
- public contract 타입의 public signature는 native bridge 타입을 참조하지 않는다.
  concrete value object 내부가 native-backed storage를 써야 하는 경우에도
  P/Invoke/JNI/N-API/cgo 선언과 marshalling 전용 struct mirror는
  native bridge source에 둔다.
- 값만 담는 DTO/value/enum/error/result 타입은 구체 타입으로 둔다. symmetry를
  이유로 의미 없는 interface, trait, protocol로 감싸지 않는다.

고정 카테고리는 아래와 같다.

- `Core/`: context, version, 역할, utility resource.
- `Messaging/`: message, routing id, received, topic message, multipart.
- `Sockets/`: socket contracts, socket implementations, socket options.
- `Eventing/`: monitor, poller, timer, readiness event.
- `Service/`: SPOT, actor, SPOT topology.
- `Errors/`: public error/result/exception domains and runtime mapping.
- `Native/`: runtime/internal source 아래에만 두는 native bridge category.

이 카테고리 이름은 문서와 리뷰 기준에서 고정한다. 실제 파일명과 폴더명은 언어별
README가 지정한 관례를 따른다. 새 카테고리가 필요하면 이 공통 정책과 C를 제외한
언어별 README의 구조를 함께 바꾼 뒤 사용한다. public contract source에는
`Native` category를 만들지 않는다. native bridge는 항상 runtime/internal source
아래에 둔다.

wrapper binding 공통 구조는 아래 역할 구조로 고정한다. C를 제외한 각 언어 README는
자기 언어의 실제 repository path와 package/module/import path로 이 구조를 다시
보여야 하며, 구현도 그 구조와 일치해야 한다.

```text
bindings/<lang>/
+-- <public-package-or-module-root>/
|   +-- <public contract categories>
|   |   +-- Core
|   |   +-- Messaging
|   |   +-- Sockets
|   |   +-- Eventing
|   |   +-- Service
|   |   +-- Errors
|   +-- <private runtime/internal area>
|   |   +-- Core
|   |   +-- Messaging
|   |   +-- Sockets
|   |   +-- Eventing
|   |   +-- Service
|   |   +-- Errors
|   |   +-- Native
+-- codecs/
+-- tests/
+-- samples/
+-- perf/
+-- native/
+-- runtimes/
```

공통으로 적용되는 기준은 아래와 같다.

- 사용자가 확인해야 하는 공개 API 계약은 찾기 쉬운 위치에 모여 있어야 한다.
- native handle, callback bridge, request progress pump, helper substrate 호출,
  object lifetime 보정 같은 구현 세부사항은 공개 계약과 섞이지 않아야 한다.
- DTO, value object, enum, error/result object는 구체 타입으로 유지한다.
  값만 담는 타입을 의미 없는 interface나 trait로 감싸지 않는다.
- socket, context, monitor, timer, service node, spot, actor처럼 native
  resource와 동작을 숨기는 타입은 언어 관례에 맞는 추상 경계를 둘 수 있다.
- perf, sample, framework adapter도 원칙적으로 공개 contract를 기준으로
  작성한다. 같은 저장소 안에 있다는 이유로 runtime 내부 타입에 의존하면
  public/internal 경계가 약해진다.

언어별 적용 방향은 다음과 같다.

| Binding | 적용 기준 |
|---|---|
| C | `core/include/zlink.h`가 public C ABI의 단일 기준이다. `bindings/c`는 별도 contract/runtime 계층을 추가하지 않고, C API 기준의 mapping, sample, test, perf, packaging 정책만 정렬한다. |
| C++ | `bindings/cpp/include/zlink/Contracts/`가 공개 C++ 계약 위치다. `bindings/cpp/src/Runtime/`은 비공개 구현 위치다. C++20, RAII class, concrete value를 우선하고, public class를 virtual interface로 과도하게 감싸지 않는다. |
| .NET | 세부 기준은 [.NET 바인딩 청사진](dotnet/README.ko.md)을 따른다. 이 문서에서는 .NET 세부 파일 구조를 복사하지 않는다. |
| Java | `bindings/java/src/main/java/systems/zlink/contracts/` 아래의 public contract package가 공개 계약 위치다. Java는 URL 기반 package layout을 따르므로 lower-case `contracts`와 `runtime` package를 실제 폴더에 반영한다. native bridge는 non-exported `systems.zlink.runtime.nativeapi` 아래에 둔다. |
| Node | `bindings/node/src/index.ts`와 `package.json` exports가 public contract projection이다. contract source는 `bindings/node/src/zlink/contracts/` 같은 lower-case source path에 두고, runtime/native addon 구현은 `bindings/node/src/zlink/runtime/` 아래에 숨긴다. |
| Python | `bindings/python/src/zlink/contracts/`가 public contract source다. `zlink` root package는 이 계약을 re-export하는 projection이고, native/FFI 구현은 `_runtime/`과 `_native/` 같은 private package 아래에 둔다. |
| Go | `bindings/go/contracts/` public package가 Go public contract source다. 현재 runtime/native 구현은 root의 unexported 구현 파일과 cgo bridge 파일이 소유한다. 나중에 별도 package로 나누면 Go `internal/` 규칙으로 숨긴다. |
| Rust | `bindings/rust/src/contracts/`가 public contract source 역할을 한다. `lib.rs`는 필요한 타입을 crate root/domain projection으로 re-export하고, `bindings/rust/src/runtime/`과 `bindings/rust/src/runtime/native/`는 private module로 둔다. |

리뷰에서는 단순히 "interface가 있는가"가 아니라 아래 질문으로 판단한다.

- 공개 contract만 보고 사용자가 사용할 수 있는 API를 이해할 수 있는가?
- 공개 contract가 runtime concrete type, native handle, helper bridge 타입을
  직접 요구하지 않는가?
- 파일이 독립 개념 또는 같은 변경 이유를 공유하는 묶음을 담고 있는가?
- marker, delegate, 작은 enum, 한 줄 record만 담은 얇은 파일이 가까운 계약 파일로
  합쳐질 수 있지 않은가?
- 값 타입을 추상화하느라 오히려 equality, ownership, 비용 모델이 흐려지지
  않았는가?
- 언어 생태계의 자연스러운 캡슐화 수단을 사용했는가?

#### 바인딩별 목표 물리 레이아웃

각 언어별 README는 신규 정렬 작업의 목표로 아래 경로와 역할을 기준으로 삼는다.
현재 구현이 아직 이 구조와 다르면 해당 binding의 구조 정리 작업에서 API, sample,
perf와 함께 단계적으로 맞춘다. public package, namespace, module, import path가
아래 `Contracts`나 `Runtime` 이름을 직접 노출한다는 뜻은 아니다.

| Binding | Contract root | Runtime root | Public projection |
|---|---|---|---|
| C++ | `bindings/cpp/include/zlink/Contracts/` | `bindings/cpp/src/Runtime/` | `#include <zlink.hpp>` and installed `include/zlink/...` headers |
| .NET | [dotnet/README.ko.md](dotnet/README.ko.md) 참조 | [dotnet/README.ko.md](dotnet/README.ko.md) 참조 | [dotnet/README.ko.md](dotnet/README.ko.md) 참조 |
| Java | `bindings/java/src/main/java/systems/zlink/contracts/` | `bindings/java/src/main/java/systems/zlink/runtime/` | exported `systems.zlink.contracts.*` JPMS packages and Maven artifact |
| Node | `bindings/node/src/index.ts` and `bindings/node/src/zlink/contracts/` | `bindings/node/src/zlink/runtime/` | package root export, generated `.d.ts`, and `package.json` exports |
| Python | `bindings/python/src/zlink/contracts/` | `bindings/python/src/zlink/_runtime/` and `bindings/python/src/zlink/_native/` | `zlink` package exports from `__init__.py` |
| Go | `bindings/go/contracts/` public package | current root unexported implementation files and cgo bridge files; future split should use `bindings/go/internal/...` | exported identifiers in `zlink.systems/zlink/contracts` |
| Rust | `bindings/rust/src/contracts/` | private `bindings/rust/src/runtime/` and `bindings/rust/src/runtime/native/` modules | `lib.rs` re-exports and public rustdoc projection |

각 언어별 README는 `Core`, `Messaging`, `Sockets`, `Eventing`, `Service`,
`Errors` 역할이 실제 소스의 어디에 배치되는지 보여야 한다. `Native`는
runtime/native bridge 역할에만 존재하며 public contract 역할로 만들지 않는다.

### 패키지 / 네임스페이스 정체성 정책

공식 라이브러리 도메인은 `zlink.systems`다. 새로 확정하거나 변경하는
언어별 package, namespace, module, artifact 이름은 이 도메인에서 출발해야
하며, 기존 조직명이나 저장소 소유자 이름을 canonical public 식별자에 넣지
않는다.

| Binding | Canonical public identity |
|---|---|
| C | public header는 `zlink.h`, symbol prefix는 `zlink_` |
| C++ | namespace는 `zlink`, 설치 header root는 `include/zlink/` |
| .NET | NuGet package id와 root namespace는 `Systems.Zlink` |
| Java | Maven group id, JPMS module, root package는 `systems.zlink` |
| Node | npm package는 `@zlink-systems/zlink`, public entrypoint는 package root |
| Python | distribution name과 import package는 `zlink` |
| Go | module path는 `zlink.systems/zlink`, public package는 `zlink` |
| Rust | crate name과 public crate root는 `zlink` |

- Framework extension package와 namespace는 각 framework 언어의 canonical identity
  아래에 둔다. 예: `.NET`은 `Zlink.Framework.*`, Java는 `systems.zlink.framework.*`.
- Go, Python, Rust는 현재 framework target이 아니므로 binding-owned codec module을
  추가하지 않는다.
- Node extension package 이름은 생태계 관례를 따르되, public identity가 `zlink`와
  `zlink.systems` 도메인에서 벗어나지 않게 한다.
- 새 문서, 샘플, 테스트는 canonical identity만 사용한다.
- 기존 `Zlink` root namespace 또는 package id가 구현 호환성 때문에 남아 있더라도
  canonical public identity가 아니며, 새 public API를 그 아래에 추가하지 않는다.

### 코어 인터페이스 모양 규칙

이 절은 C를 제외한 wrapper binding의 필수 공개 인터페이스 모양을 요약한다.
자세한 계약은 뒤의 recv 절과 operation builder 절을 따른다. C는
`core/include/zlink.h`의 함수형 ABI를 그대로 유지하므로 이 wrapper 규칙을
적용하지 않는다.

- data-plane `recv`와 `subscribe` 계열은 caller-provided output storage를
  받는다. `Received`, `TopicMessage`, `SubscriptionEvent` 같은 결과 객체를
  호출자가 만들고, binding은 그 객체를 갱신한다.
- data-plane receive 반환값은 "데이터를 받았는가"만 표현한다. hard error는
  언어 관용에 맞는 typed exception, `error`, `Result`로 전달한다.
- `Monitor.recv`, `Timer.recv` 같은 control-plane API는 호출 빈도가 낮고
  결과가 작으므로 언어별 nullable, optional, value-return 형태를 허용한다.
- `Spot.recvActorJoin` 같은 service control/admission receive도 data-plane
  drain 경로가 아니므로 언어별 nullable, optional, result-value 형태를 허용한다.
  단 no-data와 hard error는 분리되어야 하며, public 계약에서 이 예외를 분명히
  설명해야 한다.
- `send`, routed send, `publish`, `request`, `reply`, SPOT send/request/reply,
  Actor 위치·session attach 계열은 operation builder를 반환한다.
- builder 시작점 인자는 목적지, topic, channel, routing id, request sequence처럼
  operation의 대상만 받는다. payload, flags, timeout, callback, async/callback
  submit 선택은 builder 단계에서 표현한다.
- multipart payload는 builder의 `message(...)` 반복으로 누적한다. 언어 관례에
  따라 `messages(...)` convenience를 둘 수 있지만, canonical 경로는 builder다.
  이런 convenience도 public 이면 builder contract의 일부이며 `Contracts/`에
  위치해야 한다.
- `sendNoWait`, `publishWithFlags`, `requestAsync`, `requestCallback`처럼 operation
  시작점 이름을 늘리는 방식은 만들지 않는다. 같은 operation 이름을 유지하고
  builder 단계가 변형을 흡수한다. async 또는 callback 완료 표면의 언어별
  마지막 실행 메서드는 [바인딩 비동기 실행 표면 정책](async-coroutine-policy.md)을
  따른다.
- resource 생성은 public constructor를 여러 runtime class에 흩어 두지 않는다.
  binding별 root facade 또는 context factory가 생성 책임을 가진다. 예를 들어
  .NET binding은 `Zlink.CreateContext()`로 context를 만들고, socket과 service
  resource는 `IContext.Create...` factory로 만든다.
- runtime concrete type은 public contract signature에 직접 드러나면 안 된다.
  public method의 인자와 반환값은 contract interface, value object, DTO, enum,
  result/error type으로 설명 가능해야 한다.
- sample, perf, framework adapter는 이 canonical 인터페이스만 사용한다. runtime
  내부 helper나 legacy overload를 기준으로 새 코드를 작성하지 않는다.

### Send/Recv 공개 모양은 고정

bindings의 `send/recv` 공개 형태는 substrate helper가 어떻게 생기느냐에 따라
매번 다시 정하는 대상이 아니다. 이 문서와 각 언어별 binding spec이 정한
public shape를 기준으로 고정한다.

즉 helper substrate가 `*_part`, `has_more`, caller-provided message storage
형태로 바뀌더라도, binding public API는 아래 원칙을 유지해야 한다.

- binding 사용자는 언어 문서에 정의된 `send`, `recv`, request/reply,
  callback 형태를 본다.
- multipart는 각 언어 문서가 정한 aggregate convenience 모델로 계속 제공할 수
  있다.
- helper substrate 변경만을 이유로 binding public `send/recv` shape를 함께
  흔들면 안 된다.
- public shape를 바꾸려면 helper 도입과는 별도의 public API 변경으로 다뤄야
  하며, `doc/spec/bindings/` 문서부터 먼저 갱신해야 한다.

즉 앞으로 helper C API를 도입하더라도, bindings 쪽 `send/recv`는
"구현 기반이 바뀌는 것"이지 "사용자에게 보이는 형태가 자동으로 바뀌는 것"이
아니다.

### Canonical Recv: Caller-Provided Storage

고수준 binding (C++ / .NET / Java / Node / Python / Go / Rust) 의 데이터
플레인 recv 표면은 **caller가 미리 만든 결과 저장소를 매개변수로 받아 내부 상태를
갱신하는 ref-out 형태**를 canonical로 한다. 매 호출마다 새 결과 인스턴스를
할당해 반환하는 형태는 hot path 할당 오버헤드를 강제하므로 canonical 표면으로
사용하지 않는다.

이 규칙은 `Required` 다. 새로운 binding 을 만들거나 기존 binding 을 갱신할 때
canonical recv 표면은 이 절을 만족해야 한다.

#### 적용 범위 (data-plane recv 전체)

| 표면 | 결과 타입 (caller storage) |
|---|---|
| `MessageSocketBase.recv` (PAIR / DEALER) | `Received` |
| `RoutedMessageSocketBase.recv` (ROUTER) | `Received` |
| `StreamSocket.recv` | `Received` |
| `SubscriberSocketBase.subscribe` (SUB / XSUB) | `TopicMessage` |
| `XPubSocketBase.receiveSubscriptionEvent` | `SubscriptionEvent` |
| `Spot.subscribe` | `TopicMessage` |
| `Spot.recv` (routed) | `Received` |

`Monitor.recv` (`MonitorEvent`) 와 `Timer.recv` (`uint64`) 는 control plane 이며
호출 빈도가 낮고 결과가 가벼운 value 형이므로 이 절의 적용 대상이 아니다.
return-form (또는 언어별 `Optional` / nullable / `Option`) 을 유지한다.
`Spot.recvActorJoin` 처럼 Actor join admission 요청을 받는 service control-plane
API도 같은 예외를 적용할 수 있다. 이 경우 public 계약은 no-data 표현과 hard
error 표현을 분리해서 설명해야 한다.

#### 기본 계약

- `recv` 호출자는 long-lived 결과 저장소를 미리 만들어 매 호출마다 같은
  인스턴스를 넘긴다. binding 은 그 안의 part collection, routing id 저장소,
  topic 버퍼 등을 가능한 한 재사용해 매 recv 할당을 0 으로 만든다.
- 반환값은 "받았는가" boolean (또는 success/no-data 를 구분하는 동등 표현) 만
  포함한다. hard error 는 언어 관용대로 예외 또는 error code 로 전달한다.
- `recv_flags_t::dontwait` 등 non-blocking flag 가 적용된 호출에서 데이터가
  없으면 `false`, `recv_result_t::no_data`, `(false, nil)`, `Ok(false)` 같이
  caller-provided storage와 함께 쓰는 no-data 표현을 반환한다. exception 으로
  EAGAIN 을 알리지 않는다.
- multipart 결과는 caller 결과 저장소에 누적 노출한다. binding 이 임시
  컬렉션을 만들어 caller 결과 저장소와 별도로 캐싱하면 안 된다 (할당이 사라지지
  않는다).
- routed recv (router / spot) 에서 routing id 는 caller 가 제공한 `Received`
  내부 storage 에 채워야 한다. routing id 마다 새 byte 배열을 할당하는 경로는
  내부 hot path 에 두지 않는다.

#### 언어별 canonical 시그니처

위 표의 각 표면에 동일한 ref-out 패턴을 적용한다. 아래는 `Received` 결과
타입 기준 예시다. `TopicMessage`, `SubscriptionEvent` 도 동일 패턴을 따른다.

| Binding | Canonical 시그니처 |
|---|---|
| C++ | `int recv(received_t& out, recv_flags_t flags = recv_flags_t::none);` 0 = 성공, 실패나 데이터 없음은 `recv_result_t` 정수값을 반환한다. 바인딩 내부에서 메시지 초기화 같은 local 실패가 나면 -1을 반환하고 errno를 설정한다. multipart 결과는 `out.parts` 에 채워진다. `subscribe(topic_message_t& out, int flags)`, `receive_subscription_event(subscription_event_t& out, int flags)` 도 동일 규칙. |
| .NET | `bool Recv(Received result, RecvFlags flags = RecvFlags.None);` `bool Subscribe(TopicMessage result, RecvFlags flags = RecvFlags.None);` `bool ReceiveSubscriptionEvent(SubscriptionEvent result, RecvFlags flags = RecvFlags.None);` `Received` 저장소는 `Received.Create()` 로 만든다. true = 받음, false = 데이터 없음 (DontWait). hard error 는 `ZlinkException`. |
| Java | `boolean recv(Received result, RecvFlags flags);` `boolean subscribe(TopicMessage result, RecvFlags flags);` `boolean receiveSubscriptionEvent(SubscriptionEvent result, RecvFlags flags);` |
| Node | `recv(received: Received, flags?: RecvFlag): boolean;` `subscribe(topic: TopicMessage, flags?: RecvFlag): boolean;` `receiveSubscriptionEvent(event: SubscriptionEvent, flags?: RecvFlag): boolean;` |
| Python | `def recv_into(self, received: Received, *, flags: int = 0) -> bool: ...` `def subscribe_into(self, topic: TopicMessage, *, flags: int = 0) -> bool: ...` `def receive_subscription_event_into(self, event: SubscriptionEvent, *, flags: int = 0) -> bool: ...` |
| Go | `func (s *Socket) Recv(out *Received, flags RecvFlags) (bool, error)` `func (s *Socket) Subscribe(out *TopicMessage, flags RecvFlags) (bool, error)` `func (s *Socket) ReceiveSubscriptionEvent(out *SubscriptionEvent, flags RecvFlags) (bool, error)` |
| Rust | `pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError>;` `pub fn subscribe(&self, out: &mut TopicMessage, flags: RecvFlags) -> Result<bool, RecvError>;` `pub fn receive_subscription_event(&self, out: &mut SubscriptionEvent, flags: RecvFlags) -> Result<bool, RecvError>;` |

C ABI binding 은 이 절의 적용 대상이 아니다. C 바인딩은 `zlink.h` 의 typed
substrate (`zlink_router_recv_part`, `zlink_subscribe_part` 등) 를 그대로
노출한다.

#### `Received` envelope 의미 통일

고수준 binding (C++ / .NET / Java / Node / Python / Go / Rust) 에서
`Received` 는 **한 번의 data-plane recv 결과를 담는 공통 envelope** 다.
socket 종류나 service 종류가 달라도 request, reply, routed source, payload
lifecycle 의 의미는 같아야 한다.

아래 규칙은 `Required` 다.

- PAIR / DEALER / ROUTER / STREAM / SPOT routed recv 결과는 모두 같은
  `Received` 의미를 사용한다.
- request-reply 수신 결과는 별도 protocol-specific 결과 타입으로 갈라지면
  안 된다. 예를 들어 `DealerReceived`, `RouterReceived`, `SpotReceived` 처럼
  request 의미를 socket 종류별 public 타입으로 나누는 표면은 canonical 이 아니다.
- request 의 의미는 socket 종류와 무관하다. `request_seq` 가 있으면
  request-reply context 가 있는 수신 결과이고, 없으면 ordinary receive 결과다.
- reply target, send-back target, source routing metadata 는 `Received` 내부
  context 로 캡슐화한다. 사용자가 request 를 처리하기 위해 socket 종류별
  frame 형식이나 내부 dispatch 규칙을 알아야 하면 안 된다.
- 언어별 이름과 optional 표현(`null`, `None`, `Optional`, `Option`, zero value
  + `has` flag 등)은 달라도 되지만, canonical field/method 의미는
  [도메인 객체 Canonical Shape](#도메인-객체-canonical-shape-모든-바인딩-공통)
  절과 같아야 한다.

C ABI binding 은 예외다. C 는 managed/object 결과 저장소를 만들지 않고
`zlink_router_recv_part()`, `zlink_spot_recv_part()`,
`zlink_dealer_recv_part()` 같은 typed out-param 으로 같은 envelope 구성 요소를
노출한다. C 에 public `zlink_received_t` 같은 aggregate 객체를 추가하지 않는다.
그 객체를 추가하면 message part 소유권, init/close/reset, reply context 보관
규칙이 새 public lifetime 계약으로 늘어나기 때문이다. C helper 가 필요하면
sample/perf/internal helper 로만 둔다.

언어별 세부 문서에는 과거 호환성을 위한 deprecated overload가 별도로 적힐 수
있다. 위 표는 새 코드와 sample/perf가 따라야 하는 canonical 경로만 정리한다.

#### 결과 저장소 재사용 계약

- 결과 저장소 (Received / TopicMessage / SubscriptionEvent) 는 새 recv 결과를
  받기 전 자동으로 내부 상태를 초기화한다. 같은 인스턴스를 반복적으로 recv 에
  넘기는 것이 정상 사용이다.
- .NET 의 `Received` 는 public 생성자를 노출하지 않는다. caller 가 채워질
  저장소를 만들 때는 `Received.Create()` 를 사용한다. `Received` 는 concrete
  receive buffer 이며, 별도 read interface 로 나누지 않는다.
- 이전 recv 의 part 메시지를 caller 가 따로 `move` 하지 않고 다음 recv 를
  호출하면 이전 메시지는 적절히 닫혀야 한다. binding 은 part Message 의
  ownership 을 caller 에게 넘기는 별도 helper (`takeFirstPart` 등) 를 제공한다.
- thread safety 는 같은 결과 저장소를 여러 thread 가 동시에 recv 에 넘기는
  것을 보장하지 않는다. socket 자체는 단일 thread 가 recv 하도록 하는 기존
  정책이 유지된다.

### Operation Builder 정책

zlink의 send/request/reply/publish 계열과 Actor 위치·세션 attach 계열은 모두
조합 축이 많다. 대상 경로, payload part 개수, `flags`, `timeout`, async/callback
완료 방식을 일반 메서드 오버로드로 펼치면 socket과 service handle이 얕고
넓은 인터페이스가 되며 multipart payload를 외부 List/Vector 컨테이너로
포장해야 한다. 고수준 바인딩은 이 조합 복잡성을 operation 객체 안으로
숨기고, multipart는 builder의 `message(...)` 반복으로 자연스럽게 누적한다.

이 정책은 C ABI 바인딩에는 적용하지 않는다. C 바인딩은 `zlink.h`에 맞춘
함수형 계약을 유지한다. C++ / Java / .NET / Node / Python / Go / Rust 같은
고수준 바인딩의 canonical public API에 적용한다.

#### 적용 대상 시작점

operation builder 시작점은 **모든 송신·요청·응답·게시·Actor 위치·Actor session
attach 표면**에서 동일한 패턴으로 노출한다. 이름은 언어 관례에 맞게 변환한다.

##### Spot facade (`Spot` / `spot_t`)

- `publish(topic)`
- `sendToChannel(channelName)` / `send_to_channel(channel_name)`
- `sendToSpot(destNodeRid, destSpotRid)` / `send_to_spot(...)`
- `requestToChannel(channelName)` / `request_to_channel(...)`
- `requestToSpot(destNodeRid, destSpotRid)` / `request_to_spot(...)`
- `requestToRouter(peerRid)` / `request_to_router(...)`
- `replyToSpot(destNodeRid, destSpotRid, requestSeq)` / `reply_to_spot(...)`
- `replyToRouter(peerRid, requestSeq)` / `reply_to_router(...)`
- `replyActorJoin(request, accepted)` (Actor join admission reply)

##### Raw socket facade

- `PubSocket.publish(topic)` / `XPubSocket.publish(topic)`
- `DealerSocket.send()` / `DealerSocket.request()`
- `RouterSocket.send(rid)` / `RouterSocket.request(rid)` / `RouterSocket.reply(rid, requestSeq)`
- `RouterSocket.sendToSpot(destNodeRid, destSpotRid)` / `requestToSpot(...)` /
  `replyToSpot(destNodeRid, destSpotRid, requestSeq)`
- `PairSocket.send()` (PAIR send)
- `StreamSocket.sendTo(rid)` (STREAM peer send)
- 다른 raw send-capable socket의 송신 entrypoint도 동일하게 builder 시작점을
  노출한다.

##### SpotNode·StreamSocket Actor 표면

- `SpotNode.joinActor(actor, destNodeRid, destSpotRid)` / `join_actor(...)`
- `SpotNode.leaveActor(actor, currentSpotRid)` / `leave_actor(...)`
- `SpotNode.destroyActor(actor)` / `destroy_actor(...)`
- `SpotNode.remoteActorGetRef(targetNodeRid, actorId)` / `remote_actor_get_ref(...)`
- `StreamSocket.bindActor(sessionRid, actor)` / `bind_actor(...)`
- `StreamSocket.unbindActor(sessionRid, actorId)` / `unbind_actor(...)`
- `StreamSocket.sendBoundActor(sessionRid, actorId)` / `send_bound_actor(...)`
- `SpotNode.sendBoundSessionMsg(actor)` / `send_bound_session_msg(...)`

#### 공통 builder 규칙

- 시작점은 즉시 전송하지 않고 `SendOp`, `RequestOp`, `ReplyOp`,
  `ActorJoinOp`, `ActorLeaveOp`, `ActorDestroyOp`, `ActorLookupOp`,
  `ActorBindOp`, `ActorUnbindOp` 같은 언어별 operation builder를 반환한다.
  서로 다른 시작점이라도 multipart payload 표현은 모두 `.message(...)`
  반복으로 통일한다.
- `.messages(...)`, `.flags(...)`, `.timeout(...)`, callback submit, async 완료
  마지막 실행 메서드 같은 builder convenience도 public 이면 builder contract의 일부다.
  runtime 내부 shortcut으로만 정의하지 않는다. async 완료 마지막 실행 메서드의
  언어별 이름과 의미는 [바인딩 비동기 실행 표면 정책](async-coroutine-policy.md)에
  둔다.
- payload는 builder의 `message(part)` 반복 호출로 누적한다. 단일 payload와
  multipart payload를 별도 시작점 오버로드로 나누지 않는다. 외부 List/Vector
  컨테이너로 multipart를 포장하지 않는다.
- 시작점과 같은 이름으로 단일 payload shortcut overload를 만들지 않는다.
  예를 들어 `send(message)`, `send(routingId, message)`,
  `publish(topic, message)`, `sendToChannel(channelName, message)`,
  `sendToSpot(nodeRid, spotRid, message)` 같은
  public overload는 금지한다. 모두 `send(...).message(message).submit()`처럼
  builder 단계로 표현한다.
- 언어가 명시적 move/consume 이름을 자연스럽게 표현할 수 있으면 같은 builder 안에
  ownership 이전 단계(`moveMessage`, `MoveMessage`, `move_message` 등)를 둘 수
  있다. 이 단계는 새 operation 시작점이 아니며, submit 실패 뒤에도 caller가 해당
  message를 재사용할 수 없다는 계약을 이름과 문서에서 분명히 드러내야 한다. 기존
  `message(...)` 단계의 실패 시 원본 보존 계약은 바꾸지 않는다.
- payload가 의미상 필수인 작업(send/request/reply/publish, Actor join,
  ActorReplyJoin 등)에서 메시지가 하나도 없는 `submit`은 금지한다. 타입
  시스템으로 막을 수 있는 언어는 compile-time에서 막고, 그렇지 않은 언어는
  `submit` 시점에 validation error로 막는다.
- payload가 없는 작업(Actor `leave`, `destroy`, `bindActor`, `unbindActor`,
  `remoteActorGetRef`)은 builder가 `message(...)` 단계 없이 곧바로 submit이
  가능하다. 단 builder 형태와 옵션 단계(`flags(...)`, `timeout(...)`,
  `callback(...)`, async 완료 마지막 실행 메서드)는 동일하게 노출한다.
- `flags`, `timeout`, callback, async 선택은 시작점 파라미터가 아니라
  builder의 선택 단계로 둔다. 시작점은 대상 주소·요청 시퀀스처럼 의미상
  키만 받는다.
- 샘플과 문서 예제의 메시징 호출은 기본값을 반복해서 적지 않는다.
  `request`, `requestToChannel`, `send`, `sendToChannel`, `reply`,
  `publish` 같은 메시지 전송 함수에서 packet 이름은 요청 객체나 등록된 packet
  타입에서 추론되는 이름을 기본으로 사용한다. `.packetName(...)`,
  `.packet_name(...)`, `.PacketName(...)` 같은 packet 이름 override는 실제 전송할
  packet 이름이 요청 타입의 기본 packet 이름과 다를 때만 사용한다. 같은 방식으로
  `.timeout(...)`, `.Timeout(...)` 같은 per-call timeout은 해당 호출이 소켓이나
  framework에 설정된 기본 timeout과 다른 값을 요구할 때만 사용한다. 이 규칙은
  예제를 짧게 보이게 하려는 규칙이 아니라, 사용자가 불필요한 옵션을 표준 사용법으로
  오해하지 않게 하기 위한 샘플 계약이다.
- request/reply protocol envelope을 직접 만드는 helper는 public binding 표면에
  두지 않는다. `requestFrame(...)` 같은 API는 request sequence와 frame layout을
  caller에게 노출하므로 runtime/internal helper에 머물러야 한다.
- reply는 수신한 request context에서 시작해야 한다. binding public API는
  `received.reply()` 또는 `router.reply(peerRid, requestSeq)`처럼 reply 가능한
  context를 드러내는 표면만 제공한다. `dealer.reply(requestToken, parts)`처럼
  DEALER가 임의 token으로 reply를 시작하는 API는 public binding 표면에 두지
  않는다. DEALER는 특정 peer routing id를 지정할 수 없으므로 reply routing
  결정이 protocol helper에 새고, 사용자가 token 의미를 알아야 한다.
- async request·async Actor operation은 submit flags를 받지 않는다. callback
  형태는 non-blocking submit을 표현하기 위해 `flags`를 받을 수 있다. 자세한
  완료 방식 차이는 [바인딩 비동기 실행 표면 정책](async-coroutine-policy.md)을
  따른다.
- builder는 한 번 submit된 뒤 다시 submit될 수 없다. 언어가 move-only 또는
  ownership 타입을 제공하면 타입으로 막고, 그렇지 않으면 런타임 상태 검사로
  막는다.
- Actor join 시작점은 admission completion 형태가 다르므로 reply payload와
  최종 Actor ref를 함께 캡처하는 전용 completion 결과(`ActorJoinResult`)를
  builder가 노출한다. lookup·destroy·leave·bind·unbind는 일반 reply
  completion (`RequestResult`) 형태를 사용한다.

#### 공통 흐름 예시

이름은 언어 관례에 맞게 변환한다.

```java
spot.publish(topic)
    .message(part1)
    .message(part2)
    .flags(SendFlags.DONTWAIT)
    .submit();

routerSocket.requestToSpot(destNodeRid, destSpotRid)
    .message(reqPart)
    .timeout(Duration.ofSeconds(3))
    .submit();

spotNode.joinActor(actor, destNodeRid, destUserSpotRid)
    .message(joinStatePart)
    .timeout(Duration.ofSeconds(3))
    .submit(joinCallback);

streamSocket.bindActor(sessionRid, actorRef)
    .timeout(Duration.ofSeconds(2))
    .submit(replyCallback);
```

#### 언어별 비동기 실행 표면 기준

언어별 async 또는 callback 완료 마지막 실행 메서드는
[바인딩 비동기 실행 표면 정책](async-coroutine-policy.md)에 둔다.

이 규칙은 POSD 기준에서 Required다. 새 send/request/reply/publish 또는 Actor
위치·attach public API를 추가하거나 정리할 때는 이 operation builder 형태와
비동기 실행 표면 정책을 기준으로 하고, 기존 오버로드를 canonical API로 더 늘리지 않는다.

## 코어 정렬 규칙

이 절은 언어별 문서의 세부 예제보다 우선 적용되는 core 계약 요약이다.
`core/include/zlink.h` 와 언어별 문서 간 불일치가 있으면 이 절을 기준으로 한다.

#### Direct receive callback 제약

- direct receive callback install surface 는 raw `STREAM` 과 SPOT routed
  receive 에만 존재한다.
- 바인딩은 raw `PAIR`, `DEALER`, `ROUTER` 에 대해 `onReceive` 류 direct data
  callback 을 public 으로 노출하면 안 된다.
- 바인딩은 raw `SUB`, `XSUB`, SPOT subscribe receive 에 대해
  `onSubscribe` 류 direct topic callback 을 public 으로 노출하면 안 된다.
- `ROUTER` inbound routed traffic 은 단일 routed recv 표면으로 수신한다.
  바인딩 runtime은 내부에서 `zlink_router_recv_part()` 를 사용하고, public
  표면에는 aggregate routed recv와 request completion callback 만 노출한다.
  direct receive callback 은 제공하지 않는다.
- core raw `STREAM` 은 `recv`, raw callback (`zlink_recv_handler()`),
  packet callback (`zlink_stream_packet_handler()`) 의 세 모드 중 하나를
  선택하는 예외 타입이다. 고수준 바인딩의 canonical public 계약은
  `recv` 와 packet callback surface 만 노출한다. raw direct callback 은
  바인딩 내부 primitive 로만 사용하며, public API 로 추가하려면 먼저 이
  정책 문서와 해당 언어 spec 을 함께 바꾸어 별도 raw/low-level surface 로
  분리해야 한다.
#### SPOT channel과 dispatch 표면

- SPOT 은 channel-aware 모델이다. 바인딩은
  `create_route_bridge(...)` 또는 동등한 typed bridge,
  `create_publisher(...)` 또는 동등한 publisher handle,
  `send_to_channel`, `send_to_spot`, `request_to_channel`,
  channel-aware send/request operation builder 시작점과 SPOT topic publish /
  subscribe 표면을 제공해야 한다. `SpotNode`에 외부 channel `DEALER`,
  route mesh `ROUTER`, raw `PUB` socket을 직접 부착하는 legacy 표면은
  공개 계약에 포함하지 않는다.
- SPOT subscribe 결과는 topic / parts 를 노출한다. channel 이름은 메시지
  결과 필드로 반복하지 않는다.
- `zlink_spot_dispatch_event_handler()` 가 SPOT topic/routed/channel-reply/timer/actor
  plane 의 canonical readable notification surface 이다.
- Actor dispatch surface는 SPOT과 같은 service layer 공개 기능이다. 모든
  바인딩은 언어별 관례에 맞는 공개 타입으로 노출하며, 공통 의미는 아래
  `Actor Dispatch Binding Contract` 절과 `Actor Dispatch Policy` 절을 따른다.
#### Auto-HWM와 SpotNode 옵션

- `ZLINK_CTX_OPT_AUTO_HWM_PROFILE` 과
  `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` 는 모든 바인딩에서 typed context option
  으로 노출해야 한다. profile 값은 compact, low latency, balanced, throughput
  네 가지이며 기본값은 balanced 이다. context message unit 기본값 `0`은 소켓
  타입별 기본 메시지 단위를 쓰겠다는 뜻이다.
- `MonitorStatus` 은 core `zlink_monitor_status_t` 의 auto-HWM v2 진단 필드를
  빠뜨리지 않고 노출해야 한다. enabled, profile enum, role, policy class,
  unit budget, size cap, socket message slots, effective message bytes,
  applied HWM, recent recalculation reason enum, deferred shrink, blocked ratio 는
  public snapshot 계약에 포함된다.
- SPOT node option 이름은 core 공개 enum을 그대로 따른다. 방향별 HWM option이나
  delivery queue hard-limit option은 노출하지 않는다. 노출 대상은
  `ZLINK_SPOT_NODE_OPT_ROUTER_HWM_PROFILE`, `ZLINK_SPOT_NODE_OPT_ROUTER_HWM`,
  `ZLINK_SPOT_NODE_OPT_PUBSUB_HWM_PROFILE`,
  `ZLINK_SPOT_NODE_OPT_PUBSUB_HWM` 네 가지 admission option과
  `ZLINK_SPOT_NODE_OPT_DISPATCH_WORKERS_MIN`,
  `ZLINK_SPOT_NODE_OPT_DISPATCH_WORKERS_MAX` 두 가지 dispatch worker option이다.
  C API의 공통 `ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES`는 raw socket의 명시
  override로만 유지한다. 언어별 고수준 바인딩은 이 값을 socket/SpotNode/Spot
  public facade로 노출하지 않고, context option만 canonical API로 노출한다.
  SPOT node와 SPOT handle에는 raw socket 공통 옵션을 설정할 수 없으며,
  호출하면 `EINVAL`로 실패한다. 이 값은 메시지 크기 제한이 아니라 자동 HWM
  예산을 슬롯 수로 바꿀 때 쓰는 계획 단위다.
  dispatch worker option은 `SpotNode` 소유 callback worker pool의 크기만
  조정하며, `ZLINK_IO_THREADS`나 data-plane thread 수를 뜻하지 않는다.
  `min`은 1 이상, `max`는 `min` 이상이어야 한다. 명시 설정이 없으면
  CPU가 1개일 때 `min=max=1`, 그 외에는 `min=2`, `max=cpu_count`로 매핑한다.
#### SPOT status와 snapshot 이름

- SPOT binding status object는 core의
  `disconnected_sub_target_count`,
  `disconnected_routed_target_count`를 언어 관례에 맞는 이름으로 노출해야 한다.
  현재 core는 delivery queue 증가만으로 target을 끊지 않으므로 두 값은 `0`을
  보고한다.
- SPOT binding이 internal socket snapshot 이름을 노출하거나 문서화할 때는
  core가 반환하는 public snapshot 이름을 그대로 사용한다. 현재 이름은
  `mesh-pub`, `mesh-xsub`, `peer_ctrl_pub`, `peer_ctrl_sub`,
  `routed-router`, `local-pub`, `internal_receiver` 이다.
  `local-pub`는 같은 node 안 subscriber로 보내는 local fanout socket이다.
  (`ingress-sub`, `pub-ingress-tx`, `internal-router`, `internal-router-tx`는
  제거되었으며 snapshot에 포함되지 않는다.)
#### Dispatch readiness 의미

- `zlink_spot_dispatch_event_handler()`는 SPOT routed receive와 Actor lifecycle readiness의 단일 진입점이다. 바인딩은 direct routed callback을 public API로 노출하지 않는다.
- `ZLINK_SPOT_DISPATCH_EVENT_SUBSCRIBE_READABLE` 와
  `ZLINK_SPOT_DISPATCH_EVENT_ROUTED_READABLE` 은 메시지 개수 알림이 아니라
  readiness 알림이다. 바인딩은 edge-trigger one-shot 처럼 설명하거나 구현하면 안 된다.
- `ZLINK_SPOT_DISPATCH_EVENT_ACTOR_READABLE` 과
  `ZLINK_SPOT_DISPATCH_EVENT_ACTOR_JOIN_READABLE` 도 같은 dispatch readiness
  축에 속한다. Actor readable 이벤트는 어떤 Actor를 drain해야 하는지 알 수
  있어야 하며, Actor join readable 이벤트는 `Spot`의 join 수신 표면으로
  drain해야 한다.
- SPOT dispatch consumer 는 `subscribe` / `recv_routed` 를 각 언어의
  no-data 표현이 나올 때까지 drain 하는 규칙을 문서와 sample 에 같이 반영해야
  한다. 예를 들어 C++은 `recv_result_t::no_data` 반환값을 사용하고,
  Java/Node/Python은 caller-provided result storage를 채우는 API에서 `false`를
  사용한다.
- 첫 SPOT routed recv 는 hidden activation, hidden queue open, hidden target registration 을
  수행하면 안 된다. 바인딩도 같은 전제를 두고 lazy bootstrap 로직을 올리지 않는다.
#### Send-ready, Peer 가중치, STREAM 수신 모드

- `zlink_send_ready_handler()` 와 poller `ZLINK_POLLOUT` 은 같은
  send-recovery readiness 축을 가리킨다. 바인딩 문서도 같은 의미로 설명해야
  한다. `ZLINK_POLLOUT` 은 "transport writable" 이 아니라
  "send recovery readiness / backpressure recovery notification" 으로 설명한다.
- 바인딩은 peer 가중치 surface 를 언어별 typed option/property 로 노출해야
  한다. 설정 대상은 `ROUTER`, `DEALER`이며 값 범위는
  `0..10000`, 기본값은 `100`이다. `0`은 새 outbound 선택에서 제외를 뜻한다.
  대응하는 제출 실패 코드는 `ZLINK_SUBMIT_NOT_ADMITTED` (값 13) 이며,
  모든 바인딩의 `SubmitError` 매핑에 포함되어야 한다.
- core raw `STREAM` 은 다음 세 수신 모드 중 하나만 선택할 수 있다:
  (a) `zlink_recv_part()` 기반 blocking/non-blocking recv, (b) `zlink_recv_handler()`
  raw direct callback, (c) `zlink_stream_packet_handler()` 빅엔디언
  `u16 header_size + u32 body_size + header + body` 프레이밍 packet callback.
  두 번째 attach 시 `EBUSY` 가 반환된다. 고수준 바인딩은 public 으로
  노출한 `STREAM` receive surface 들 사이에 같은 배타 규칙을 유지해야
  한다. `detachStream`, `streamDetach`, callback detach 같은 별도 public
  해제 API 는 core 공개 계약에 없으므로 canonical binding surface 에
  추가하지 않는다. 수신 모드 해제와 callback 정리는 socket close 가 맡는다.
- `zlink_recv_handler()` 는 raw `STREAM` 전용이다. `PAIR`/`DEALER`/`SUB`/
  `XSUB`/`ROUTER` 에 attach 하면 `ZLINK_HANDLER_NOT_SUPPORTED` 로 실패한다.
- socket 기본값: `ZLINK_ROUTER_OPT_MANDATORY` = `1`,
  `ZLINK_OPT_RID_DUPLICATE_POLICY` = `ZLINK_RID_DUPLICATE_REJECT`,
  `ZLINK_PUB_OPT_NODROP` = `0`.
  바인딩 예제는 이 기본값을 기준으로 작성한다.

## Actor Dispatch 바인딩 계약

이 절은 모든 언어 바인딩에 공통으로 적용되는 Actor 공개 계약이다. 언어별 문서는
아래 계약을 각 언어 관례에 맞는 이름과 타입으로 풀어서 적어야 한다.

Actor dispatch는 SPOT messaging의 부가 기능이 아니라 service layer의 독립 공개
기능이다. lifecycle과 routing은 `SpotNode`, `Spot`, `StreamSocket`이 나누어
소유하므로 각 공개 타입의 책임을 나누어 노출한다. 구체적인
surface 배치는 아래 `Actor Dispatch Policy` 절을 따른다.

#### Actor id, ref, lifecycle 진입점

- Actor id는 비어 있지 않은 UTF-8 문자열이며 최대 255 bytes다. NUL 문자는
  허용하지 않는다.
- Actor ref는 `node_rid`, `actor_id`, `generation`을 가진다.
  `generation == 0`은 unchecked remote ref이며 유효하지 않은 값으로 보지 않는다.
- unchecked remote Actor ref 생성은 `SpotNode`가 소유한다. 언어 관례상 static
  method 또는 factory function 으로 표현할 수 있지만, canonical 문서와 sample 은
  `SpotNode` 소유 surface 를 기준으로 한다. `ActorRef` 자체에 별도 unchecked
  factory 를 중복 public API 로 추가하지 않는다.
- local Actor는 `SpotNode`가 만든다. 한 Actor는 동시에 하나의 Spot에만 join할
  수 있고, leave는 unread 메시지를 비우지 않는다.
- `Actor.close` 또는 동등한 lifecycle method 는 그 Actor handle 이 소유한 local
  Actor 를 파괴한다. `SpotNode.destroyActor(actorRef)` 또는 동등한 method 는
  Actor handle 없이 Actor ref 만 가진 caller 를 위한 ref 기반 파괴 surface 다.
  둘은 같은 책임을 다른 이름으로 반복하는 API 가 아니라, owner 가 다른 두
  진입점이므로 언어별 spec 은 이 차이를 문서화해야 한다.
- `Actor.join` / `Actor.leave` 는 local Actor handle 을 가진 caller 를 위한
  표면이다. `SpotNode.joinActor(actorRef, ...)` /
  `SpotNode.leaveActor(actorRef, ...)` 는 Actor ref 만 가진 caller 를 위한
  표면이다. 한쪽만 제공하면 ref-only 흐름 또는 owned-handle 흐름 중 하나가
  불필요하게 복잡해진다.
#### STREAM session binding

- 한 STREAM session은 여러 Actor를 bind할 수 있다. bind/unbind는 session routing
  id와 actor id 또는 Actor ref를 기준으로 한다.
- STREAM에서 Actor로 보내는 public API는 bound session과 actor id를 선택자로
  사용한다. 제거된 lookup/send helper 이름은 public API에 남기지 않는다.
- `Actor.sendBoundSession` 과 `Actor.closeBoundSession` 은 session routing id 를
  인자로 받지 않는다. Actor 가 현재 bound session 선택을 내부에서 숨긴다.
  caller 가 session routing id 로 직접 선택해야 하는 경우에는
  `StreamSocket.sendBoundActor(...)` 를 사용한다.
- Actor recv info 의 `source_node_rid` 와 `source_session_rid` 는 core 구조체의
  값 필드이므로 nullable / optional 로 문서화하지 않는다. no-data 는 recv 결과
  자체의 `false`, no-data result, `Ok(false)` 같은 표현으로만 전달한다.
#### Dispatch와 join 결과

- Actor readable dispatch event는 어떤 Actor를 drain해야 하는지 알 수 있어야
  한다. callback을 다른 실행 컨텍스트로 넘기는 언어는 callback 진입 시점에
  Actor part를 nonblocking으로 미리 drain해서 public dispatch info가 그 part를
  반환하게 해야 한다.
- Spot join request는 message를 포함한다. join reply도 accept/reject 결과와
  함께 message를 caller에게 돌려줘야 한다. join completion은 전용 `actor join`
  result 타입으로 최종 Actor ref(remote join이면 target node의 ref)와 joined
  Spot rid를 application에 전달해야 한다.
- request reply 표면은 core reply 함수가 지원하는 payload part만 노출한다.
  core reply 함수에는 send flag 인자가 없으므로, 바인딩은 reply builder에
  no-op flag 설정 단계를 추가하지 않는다.
#### 제거된 API

- remote Actor 생성과 admission handler는 공개 표면에서 제거되었다. 원격 노드에서
  시작해야 하는 Actor는 application이 해당 SpotNode에서 직접 `actor_new`로
  생성한다. 원격 Actor의 checked ref가 필요하면 async `remote_actor_get_ref`
  lookup을 사용한다.
- Actor 위치는 Actor 생성, Spot join/leave, Actor destroy 흐름에서 갱신된다.
  STREAM session bind/unbind는 Actor 위치를 만들거나 제거하지 않는다.
- session attach와 Actor 위치 이동은 서로 다른 상태 전이다. user Spot으로 join
  하는 데 bound STREAM session은 필요하지 않다. Actor 위치 이동은 session
  mapping을 자동으로 바꾸지 않는다.
- Actor별 queue limit option은 없다. 바인딩은 이를 public option으로 만들면
  안 된다.
- 제거된 Actor ref 함수, stream actor lookup/send helper, session actor key
  설계 이름은 public surface와 문서에 남기지 않는다.

## 문서 해석 규칙
- 이 문서의 정책 본문은 기본적으로 규범 문서다.
- 아래 용어는 다음 의미로 해석한다.
  - `Required`: 현재 리뷰와 구현에서 반드시 지켜야 하는 항목.
    미준수 시 리뷰에서 차단된다.
  - `Recommended`: 강하게 권장하지만, 바인딩 특성에 따라 단계적으로 적용할
    수 있는 항목. 미준수 시 리뷰에서 사유를 요구하지만 차단하지 않는다.
  - `Target`: 장기적으로 맞춰가야 하는 목표 항목. 해당 바인딩이 이
    컴포넌트를 구현하기로 결정한 경우에만 적용된다. 구현하지 않기로
    결정한 경우 리뷰에서 요구하지 않는다.
  - `Internal-only`: 바인딩 구현 내부에서는 사용할 수 있지만 public API,
    sample, guide, spec signature 로 노출하면 안 되는 항목이다.
- 별도 표시가 없으면 정책 본문은 `Required`로 본다.
- 섹션 제목에 `(Target)` 또는 `(Recommended)`가 표시된 경우, 해당 섹션
  전체는 표시된 수준으로 해석한다. 무표시 기본값(`Required`)보다 우선한다.
- `Implementation Review Checklist` 섹션은 새 API를 추가하는 설계 초안이
  아니라, 이미 정의된 public API 계약을 구현이 지키는지 확인하는 기준이다.
- checklist 항목은 문서 본문의 의미 계약을 대체하지 않는다.

## 핵심 원칙
- 코어 계약은 `zlink.h`의 `*_part` substrate가 단일 기준이다.
- send/recv/request/reply/publish/subscribe 계열의 내부 구현은 반드시 core `*_part`
  substrate를 사용한다. aggregate 형태의 core 함수를 binding 내부에서 직접 호출하지 않는다.
- public API는 multipart 모델을 기준으로 설계한다.
- blocking과 non-blocking은 이름으로 구분할 수 있다.
- 동일한 능력을 여러 방식으로 중복 노출하지 않는다.
- 값의 의미는 `int`가 아니라 enum, boolean, value object로 올린다.
- raw option bag은 public에 노출하지 않는다.
- 바인딩은 코어의 상태 오류를 추론하지 않는다.
- 입력 값의 형식, 범위, overflow, truncation 위험은 바인딩이 먼저 막는다.
- 구조는 POSD 원칙에 따라 깊은 모듈, 정보 은닉, 낮은 변경 파급을
  우선한다.
- 이 문서는 의미 계약을 우선 정의한다.
- 언어별 표면은 각 언어 관례에 맞게 달라질 수 있지만, 의미 계약은 같아야
  한다.

## Monitor Ready 계약
- `*_READY_CHANGED` monitor event 의 `value` 는 aggregate ready count 계약이 아니다.
- binding public API는 monitor snapshot 에 ready-count surface 가 있다고
  가정하면 안 된다.
- readiness gate 가 필요하면 low-cost event edge 를 직접 사용해야 한다.
- raw perf/샘플은 `CONNECTION_READY` event counting 을 사용한다.
- SPOT perf/샘플은 별도 서비스 이벤트 gate 를 사용하지 않는다.
- SPOT perf 는 explicit `READY/START` barrier protocol 을 사용한다.
- delivery-ready/count 계열 monitor event 를 새 gate contract 로 만들면 안 된다.

## POSD 구조 정책
- 바인딩 설계는 John Ousterhout의 POSD 원칙을 따른다.
- public API는 사용자가 알아야 할 개념 수를 줄여야 한다.
- 내부 구현 복잡도는 facade, value object, domain object 뒤로 숨겨야 한다.
- 얕은 래퍼(shallow wrapper)는 지양한다.
  - 단순히 native 함수 이름만 바꾸고 새 의미를 추가하지 못하는 public
    wrapper는 늘리지 않는다.
- 같은 능력을 여러 타입과 여러 이름으로 반복 노출하지 않는다.
- 변화가 한 곳에서 끝나야 할 규칙은 한 모듈에 모은다.
  - 예: routing id 길이 제한
  - 예: send failure contract
  - 예: typed option ownership
- 여러 언어가 공유하는 역할, owner, no-data, error, naming 규칙은 이
  정책 문서가 한 번만 소유한다. 언어별 spec 은 같은 규칙을 다시 설계하지
  않고, 이 문서의 계약을 언어 관례에 맞게 표현한다.
- 언어별 spec 이 이 문서와 다른 규칙을 필요로 하면, 개별 문서부터 바꾸지
  않는다. 먼저 이 정책 문서에 예외 사유와 적용 범위를 적고, 그 다음 해당
  언어 문서를 갱신한다. 그래야 같은 설계 결정이 여러 문서에 흩어지지 않는다.
- 시간 순서에 의존하는 분해(temporal decomposition)를 줄인다.
  - 예: 사용자가 `setOption` 조합 순서를 기억해야 하는 API 금지
- public API는 "무엇을 할 수 있는지"를 드러내고, "내부에서 어떻게 배선되는지"를
  드러내지 않아야 한다.
- 값 객체와 결과 객체는 깊은 모듈로 취급한다.
  - 호출자에게는 작은 인터페이스를 주고, 내부에서는 검증, ownership, shape
    규칙을 함께 캡슐화해야 한다

## 공개 표면 규칙

### 기반 타입 노출
- 가능하면 컴파일 단계에서 사용자가 concrete socket type만 직접 쓰게 해야 한다.
- 사용자가 generic root base, raw compat base, shared base를 concrete socket
  type 대신 직접 쓰는 구조는 피한다.
- static typed binding은 public type/export/visibility를 이용해 이 규칙을
  강제해야 한다.
- dynamic binding은 export 제한과 surface test로 같은 규칙을 강제해야 한다.
- generic root base 또는 raw compat base는 공통 lifecycle과 공통 관리 기능만
  외부에 노출한다.
- 역할-specific shared base는 모든 descendant가 공통으로 가지는 능력만
  외부에 노출할 수 있다.
- socket-type-specific 역할을 generic root base나 raw compat base로
  올리면 안 된다.
- public base에서 외부 접근을 허용해도 되는 공통 기능 예:
  - `bind`, `unbind`
  - `connect`, `disconnect`, `disconnectRid` on connectable base only
  - `close` / `dispose`
  - common typed options
  - `monitorOpen` 또는 동등한 monitor 진입점
  - `setTlsServer`, `setTlsClient` 또는 동등한 TLS helper
- generic root base 또는 raw compat base에서 외부 접근을 허용하면 안 되는 기능:
  - `send(...)`
  - `send(routingId, ...)`
  - `sendParts(...)`
  - `sendFrom(...)`
  - `recv()`
  - `recv(flags)` / `recv(size, flags)`
  - `recvInto(...)`
  - `recvMsgInto(...)`
  - routed receive alias (`receiveRouted` 등)
  - `publish(...)`
  - `setSubscription(...)`
  - `unsetSubscription(...)`
  - `subscribe()`
  - `receiveSubscriptionEvent()`
  - raw direct receive handler registration
  - `onSubscribe(...)`
  - `setSendReadyHandler(...)`
  - `setRoutingId(...)`, `getRoutingId()`
  - `attachStreamRaw(...)`, `detachStream()`
  - `streamAttach(...)`, `streamAttachRaw(...)`, `streamDetach()`
  - `streamPeerRoutingId(...)`, `streamSend(...)`
  - raw option bag (`setOption`, `getOption`, `setSockOpt`, `getSockOpt` 등)
  - topic/socket-type-specific option facade
  - canonical 이름을 우회하는 legacy alias
    - 예: `recvHandler(...)`, `subscribeHandler(...)`
- 역할-specific shared base는 descendant 전부에 공통인 역할에 한해
  허용할 수 있다.
  - 예: subscriber-only base의 `setSubscription`, `unsetSubscription`,
    `subscribe`
  - 예: publisher-only base의 `publish`, `setSendReadyHandler`
- 위 역할은 역할 matrix에서 `Y`인 concrete socket type에만
  public으로 존재해야 한다.
- 역할 matrix에서 `—`인 socket type에 대해 base 경유 우회 호출이 가능하면
  안 된다.
- perf, sample, helper, compat layer도 canonical public surface 규칙을
  우회하는 base entry를 새 기준처럼 사용하면 안 된다.
- deprecated compat API가 필요하더라도 canonical public API와 분리된 compat
  namespace 또는 internal surface로 격리한다.
- 사용자가 `SocketType`과 raw flag 조합을 기억해서 올바른 send/recv 계열을
  선택해야 하는 구조는 POSD 위반으로 본다.

### Multipart 전용
- send/receive public surface는 multipart 기준으로 통일한다.
- 단일 메시지 수신 편의 오버로드는 public에 두지 않는다.
- 단일 part 전송 편의 메서드는 허용할 수 있다.
  - 예: `send(Message part)`는 `send(List<Message> parts)`의 간편 오버로드
- 수신 결과는 언어에 맞는 도메인 객체 또는 동등한 multipart 표현으로
  반환한다.

### 오류 처리 정책

모든 데이터 경로 함수 (`send`, `recv`, `request`, `reply`, `subscribe`,
`publish`) 는 동일한 에러 처리 원칙을 따른다.

#### 원칙

1. **Exception 언어는 반환값으로 에러를 전달하지 않는다.**
   - 대상: C++, Java, .NET, Node, Python.
   - 성공 시 결과를 반환하거나 void 반환한다.
   - 실패 시 예외를 던진다.
   - 예외에는 `int code` (0–706 범위) 를 포함하여 호출자가 실패 원인을
     구분할 수 있게 한다.
   - `BACKPRESSURED`, `NOT_CONNECTED`, `NOT_FOUND` 를 포함한 모든 실패는
     예외로 전달한다. 이들은 반환값이 아니다.
2. **C / Go / Rust 는 exception 이 없으므로 return-based 계약을 따른다.**
   바인딩은 각 언어 관용구에 맞는 스타일로 처리한다.
   - C: 함수별 typed result enum 반환
     (`zlink_submit_result_t`, `zlink_recv_result_t`,
      `zlink_handler_result_t`, `zlink_close_result_t`,
      `zlink_bind_result_t`, `zlink_connect_result_t`,
      `zlink_config_result_t`).
   - Go: `(T, error)` 반환. error 객체에 `int` 코드를 포함한다.
   - Rust: `Result<T, E>` 반환. `E` 는 가능한 한 함수군별 구체 에러
     (`BindError`, `SubmitError` 등)를 쓰고, 여러 함수군이 섞이는 경계에서만
     `ZlinkError` 로 승격한다. 에러 값에는 `int` 코드가 포함된다.
     `?` 연산자로 호출측 전파를 쓴다.
3. **`Try*` 대신 `flags` 와 반환 규칙으로 blocking 여부를 표현한다.**
   - C 는 C ABI 함수형 계약을 유지한다.
   - Go / Rust 는 return-based 오류 전달을 유지하되, wrapper binding의
     ref-out recv와 operation builder 규칙은 그대로 적용한다.
   - `.NET` / `Java` / `Node` / `Python` / `C++` 는 public
     `trySend`, `tryRecv`, `tryRequest` 를 두지 않는다.
   - C ABI 는 blocking 과 non-blocking 을 함수 인자의 `flags` 로 표현한다.
   - wrapper binding 의 send/publish/request/reply 계열은 builder 의
     `.flags(...)` 단계로 non-blocking submit 을 표현한다. operation 시작점
     시그니처에 별도 `flags` 인자를 늘리지 않는다.
   - wrapper binding 의 data-plane `recv`, routed recv, `subscribe` 는
     caller-provided result storage 를 채우고, 반환값은 "데이터를 받았는가"만
     표현한다.
   - non-blocking receive 에서 현재 읽을 데이터가 없으면 `false` / `nil,false` /
     `Ok(false)` 같은 언어별 no-data 표현을 반환하고, 진짜 오류만 예외 또는
     반환 에러로 전달한다.
   - 비동기 request 는 같은 `request` operation builder 의 완료 객체 반환 단계로
     선택하고, submit flags 를 받지 않는다.
   - `sendNoWait`, `recvNoWait`, `publishNoWait` 같은 transport-style 이름은
     공개 surface 에 두지 않는다.
4. **`INTERNAL_ERROR` 상세 조회.**
   - result code 가 `INTERNAL_ERROR` 계열
     (12, 105, 206, 306, 404, 505, 604, 704 등) 이면
     `zlink_errno()` 로 내부 raw errno 를 조회할 수 있다.
   - 바인딩의 에러 타입(exception 언어는 예외 객체, return-based 언어는
     에러 값)은 `internalErrno` / `internal_errno` 필드로 이를 노출한다
     (디버깅 전용).
   - 그 외 result code 에서는 `zlink_errno()` 호출이 불필요하다.

#### 언어별 에러 표현

| 언어 | 처리 방식 | 에러 타입 | 코드 접근 | 내부 errno |
|---|---|---|---|---|
| C | return | 함수별 result enum 반환 | enum 값 자체 | `zlink_errno()` |
| C++ | return / throw | caller-provided recv는 `int`, 그 외 실패는 `zlink_error_t` | recv는 반환값, 예외는 `.code()` | recv `-1`일 때 `errno`, 예외는 `.internal_errno()` |
| Java | throw | `ZlinkException` | `.getCode()` | `.getInternalErrno()` |
| .NET | throw | `ZlinkException` | `.Code` | `.InternalErrno` |
| Go | return | `error` | `.Code()` | `.InternalErrno()` |
| Rust | return (`Result`) | `ZlinkError` | `.code()` | `.internal_errno()` |
| Node | throw | `ZlinkError` | `.code` | `.internalErrno` |
| Python | throw | `ZlinkError` | `.code` | `.internal_errno` |

- `return` 그룹(C / Go / Rust) 은 호출자가 반환값을 명시적으로 검사한다.
  Go 는 `if err != nil`, Rust 는 `match` / `?` 연산자 관용구를 쓴다.
- `throw` 그룹(C++ / Java / .NET / Node / Python) 은 예외를 전파한다. caller
  는 언어별 `try`/`catch` 또는 상위 propagation 에서 처리한다.

#### Error Codes

- C API 는 함수별 typed result enum 을 반환한다.
- 모든 enum 값은 0–706 범위에서 겹치지 않는다.
- 바인딩은 이 코드를 언어별 에러 타입의 `int code` 에 포함시킨다
  (exception 언어는 예외 객체, return-based 언어는 반환 에러 값).
- 전체 enum 정의는
  [errno-map.md](https://kairos-code-dev.github.io/zlink/en/spec/core/04-errno-map/) 를 참조한다.

#### 함수별 에러 타입 계층

C API 의 **함수별 typed result enum 구조를 모든 바인딩이 그대로 계승**한다.
단일 `ZlinkException` / `ZlinkError` 만 두면 시그니처만으로 발생 가능한 에러
집합을 알 수 없기 때문이다.

각 바인딩은 8 개의 함수군 에러 타입을 `ZlinkException` / `ZlinkError` 의
하위 타입으로 제공한다. 메서드 시그니처는 해당 함수군의 구체 에러 타입을
노출해야 한다.

| C result enum | 함수군 | 하위 에러 타입 (의미 계약) |
|--------------|--------|--------------------------|
| `zlink_submit_result_t` | send / publish / request submit / reply submit | `SubmitError` |
| `zlink_request_result_t` | request completion (callback) | `RequestError` |
| `zlink_recv_result_t` | recv / subscribe / subscription event / monitor recv / timer recv | `RecvError` |
| `zlink_handler_result_t` | handler 등록 | `HandlerError` |
| `zlink_close_result_t` | close / destroy | `CloseError` |
| `zlink_bind_result_t` | bind | `BindError` |
| `zlink_connect_result_t` | connect / disconnect / unbind | `ConnectError` |
| `zlink_config_result_t` | option set/get, snapshot, poller mutation, proxy, timer config | `ConfigError` |

##### 언어별 네이밍

| 언어 | 최상위 타입 | 하위 타입 네이밍 | 기반 타입 | 예시 시그니처 |
|------|-----------|----------------|----------|-------------|
| C | — | 함수별 typed enum 그대로 | — | `zlink_bind_result_t zlink_bind(...)` |
| C++ | `zlink_error_t` | `zlink::<category>_error_t` (snake_case + `_t`) | `std::runtime_error` 계열 | `void bind(...) /* @throws bind_error_t */` |
| Java | `ZlinkException` | `<Category>Exception` | **unchecked** (`RuntimeException`) | `void bind(...) /* @throws BindException */` |
| .NET | `ZlinkException` | `Zlink<Category>Exception` | `System.Exception` (unchecked; .NET 의 모든 exception 은 unchecked) | `void Bind(...) /* throws ZlinkBindException */` |
| Node | `ZlinkError` | `<Category>Error` | `Error` | `bind(ep): void /* @throws BindError */` |
| Python | `ZlinkError` | `<Category>Error` | `Exception` | `def bind(ep): ...  # raises BindError` |
| Go | `error` (interface) | `*<Category>Error` (typed error struct) | `error` 인터페이스 구현 | `func (s) Bind(ep) error  // returns *BindError` |
| Rust | `ZlinkError` (enum) | `<Category>Error` (variant 또는 별도 타입) | `std::error::Error` 구현 | `fn bind(ep) -> Result<(), BindError>` |

- `Category` 는 `Submit`/`Request`/`Recv`/`Handler`/`Close`/`Bind`/`Connect`/
  `Config` 의 8 개.
- `ZlinkException` / `ZlinkError` 는 모든 하위 타입의 부모로서 "모두 잡기"
  관용구를 유지한다. caller 는 세분화가 필요하면 하위 타입으로, 아니면
  부모로 캐치한다.
- 각 하위 에러 타입은 해당 함수군의 `ErrorCode` 중첩 enum 을 전용으로
  가진다. 다른 함수군 코드는 그 타입에서 표현되지 않는다.
- **Java / .NET 은 unchecked exception 체계를 따른다.** 메서드 시그니처에
  `throws` 절을 강제하지 않는다. 발생 가능 exception 은 Javadoc `@throws`
  / XML doc `/// <exception cref="...">` 로 명시한다.
- Rust / Go 는 반환 타입으로 구체 하위 에러를 선언한다. 동적 언어
  (Node/Python) 는 TSDoc `@throws` / Python docstring `Raises:` 로 동일
  정보를 제공한다.

##### 시그니처 선언 규칙

- 메서드가 단일 함수군 에러만 던질/반환할 수 있으면 구체 하위 타입만
  명시한다.
  - Java: `@throws BindException` (Javadoc, 시그니처에 `throws` 절 불필요)
  - .NET: `/// <exception cref="ZlinkBindException">`
  - C++: `/// @throws bind_error_t` (noexcept 로 표시하지 않음)
  - Node: TSDoc `@throws {BindError}`
  - Python: docstring `Raises: BindError`
  - Go: 반환 타입 문서 `returns *BindError`
  - Rust: 반환 타입 `Result<T, BindError>`
- 메서드가 여러 함수군에 걸칠 경우 (예: service 계층 조합 호출) 공통 부모
  `ZlinkException` / `ZlinkError` 를 선언하고 doc 에 실제 발생 가능한
  하위 타입을 나열한다.
- validation 예외 (language-native `IllegalArgumentException` 등) 는 위 체계
  와 별도이며, `ZlinkException` / `ZlinkError` 계층에 들어가지 않는다.

### Flags 정책

모든 데이터 경로 함수는 `flags` 선택 항목을 갖는다. 일반 socket 함수는
언어별 시그니처의 `flags` 파라미터로 표현하고, SPOT operation builder 대상
함수는 builder의 `flags(...)` 단계로 표현한다.

| 함수 계열 | flags 용도 |
|---|---|
| `send`, `publish`, `reply` | `DONTWAIT` — non-blocking submit |
| `recv`, `subscribe`, `receiveSubscriptionEvent` | `DONTWAIT` — non-blocking receive |
| `request` (callback) | `DONTWAIT` — non-blocking submit |
| `request` (비동기 완료) | flags 없음 — 언어별 완료 객체 반환 경로를 사용 |

- flags 기본값은 `0` (blocking).
- non-blocking 호출의 temporary 상태는 언어별 public 계약에 맞춰 전달한다.
  - `.NET` / `Java` / `Node` / `Python`
    - `send`, `publish`, callback `request`: temporary backpressure 면
      `false`
    - caller-provided `recv`, `subscribe`,
      `receiveSubscriptionEvent`: 현재 데이터가 없으면 `false`
    - 그 외 실패: typed exception
  - C++
    - operation builder `send` / `publish` / callback `request`: temporary
      backpressure 면 `false`
    - caller-provided `recv` / `subscribe` /
      `receive_subscription_event`: 현재 데이터가 없으면 `recv_result_t::no_data`
      정수값 반환
    - binding-local 실패만 `-1`을 반환하고 `errno`를 설정
  - return-based 언어 (C/Go/Rust): 에러 반환 (C=result enum,
    Go=`error`, Rust=`Err(E)`).
- 언어별 flags 표현:
  - C: `int flags = 0` (C ABI는 builder 정책 적용 안 됨)
  - C++ / Java / .NET / Node / Python / Go / Rust 송신·요청·응답·게시·Actor
    attach 표면: builder의 `.flags(...)` 단계로 표현한다. operation 시작점
    시그니처에 별도 `flags` 인자나 `_with_flags` 변형을 두지 않는다.
  - C++ / Java / .NET / Node / Python / Go / Rust data-plane recv/subscribe 표면:
    caller-provided output storage와 함께 `flags` 인자를 받는다.

### 네이밍 정책

#### 생성 함수 네이밍

`Message`와 `RoutingId`처럼 입력 값에서 새 객체를 만드는 공개 함수는 `.NET`
바인딩의 `From(...)` 의미를 기준으로 맞춘다. 입력 타입이 함수 이름에 들어가면
같은 개념이 언어마다 여러 이름으로 갈라지므로 피한다.

- 일반 생성은 `from(...)` 또는 언어별 동등 이름 하나로 모은다.
  `from_bytes`, `from_string`, `from_u32`, `from_uuid` 같은 타입 suffix 이름은
  canonical public API 로 쓰지 않는다.
- hex 디코딩은 사람이 읽는 문자열 디코딩이라는 의미가 다르므로
  `from_hex` 계열을 예외로 허용한다. 언어별 표기는 `FromHex`, `fromHex`,
  `from_hex`, `NewRoutingIDFromHex`처럼 관용구를 따른다.
- Python은 `from`이 예약어이므로 `from_(...)`를 사용한다.
- Rust는 routing id에 표준 `From` 구현을 사용하고, 실패 가능한 message
  생성에는 `try_from` 관용구를 사용할 수 있다. 입력 타입 이름을 붙인
  `from_bytes` / `from_string` 계열 public helper는 두지 않는다.
- Go는 오버로드가 없으므로 `NewRoutingID(...)`, `NewRoutingIDString(...)`,
  `NewRoutingIDUint32(...)`, `NewRoutingIDUUIDBytes(...)`처럼 typed constructor를
  허용한다. 이 예외는 Go의 정적 타입 스타일을 유지하기 위한 것이며,
  `NewRoutingIDFromString`처럼 `From`과 타입 이름을 함께 반복하지 않는다.
- allocation은 source 변환이 아니므로 `allocate(...)` 또는 언어별 생성자
  관용구(`NewMessageWithSize(...)` 등)를 사용한다.

#### 한 entrypoint, 빌더 단계로 변형 표현

같은 작업의 변형(async/callback, single/multipart, flags 유무, timeout 유무)은
동일한 entrypoint 이름을 사용하고, 변형은 builder 단계로 표현한다. 별도 이름
(`request_callback`, `send_nonblocking`, `send_with_flags` 등)을 만들지 않는다.

```
// GOOD: one name, builder absorbs the form.
spot.request_to_channel(channel)
    .message(part)
    .timeout(Duration::from_secs(3))
    .submit()                              // returns the language completion object

spot.request_to_channel(channel)
    .message(part)
    .flags(SendFlags::DONTWAIT)
    .submit(callback)                      // callback variant

// BAD: split names for the same operation.
request_to_channel(channel, parts, timeout)
request_to_channel_callback(channel, parts, callback, flags, timeout)
```

#### 공통 결과 타입 이름

공통 결과 타입은 owner 이름을 반복하지 않는다. 타입 이름은 값이 나타내는
도메인 개념을 직접 드러낸다.

- Poller 대기 결과 타입의 canonical 이름은 `PollEvent` 이다.
  C++은 `poll_event_t`, Java/.NET은 `PollEvent`, Node/TypeScript는
  `PollEvent` 형태를 사용한다. `PollerEvent`처럼 owner를 한 번 더 붙인
  이름은 canonical public API 로 쓰지 않는다.
- Timer, monitor, dispatch 결과도 같은 규칙을 따른다. owner가 이미
  반환 타입이나 네임스페이스에서 드러나면 타입 이름에 owner를 반복하지 않는다.

#### SPOT 대상 네이밍

SPOT routed 네이밍은 pub/sub 와 대상 지정 messaging 을 분리한다.

- **channel-aware 경로**
  - `send_to_channel(channel_name) -> SendOp`
  - `request_to_channel(channel_name) -> RequestOp`
- **SPOT topic 경로**
  - `publish(topic) -> SendOp`
    - receiver가 이미 publish-capable socket 또는 `Spot` 이므로 `publish_spot`,
      `publish_to_topic`처럼 owner나 파라미터 의미를 반복하지 않는다.
- **direct routed 경로**
  - `send_to_spot(dest_node_rid, dest_spot_rid) -> SendOp`
  - `request_to_spot(dest_node_rid, dest_spot_rid) -> RequestOp`
  - `request_to_router(peer_rid) -> RequestOp`
- **reply 경로**
  - `reply_to_spot(dest_node_rid, dest_spot_rid, request_seq) -> ReplyOp`
  - `reply_to_router(peer_rid, request_seq) -> ReplyOp`

`SendOp`, `RequestOp`, `ReplyOp`의 payload와 option은
`Operation Builder Policy` 절이 정한 `message(...)`, `flags(...)`,
`timeout(...)`, `submit...` 단계로 표현한다. 따라서 새 canonical SPOT
surface에서는 같은 시작점에 `Message` / `List<Message>` / `flags` / `timeout`
조합 오버로드를 추가하지 않는다.

새 SPOT 바인딩 표면에서는 예전 `send_service` / `request_service` 대신
`send_to_channel` / `request_to_channel` / `publish(...)` 를 기본 경로로
본다. 직접 주소 지정 경로는 코어가 제공하는 typed routed surface 로서 별도
지원할 수 있다.

언어별 관례에 따라 camelCase / PascalCase / snake_case 로 변환한다.

### Request 정책

request 는 언어별 async 완료와 callback 완료 방식을 제공할 수 있으며, 두 방식
모두 동일한 `request` entrypoint 가 반환하는 `RequestOp` operation builder
의 submit 단계로 선택한다. 별도 이름 (`request_callback`, `requestAsync` 등)
을 만들지 않는다.

SPOT operation builder 대상의 작업 시작점은 `requestToChannel` /
`requestToSpot` / `requestToRouter` 이고, raw `DealerSocket` /
`RouterSocket` 의 작업 시작점은 `request` / `request(peer)` 이다. 어느
시작점이든 완료 방식은 [바인딩 비동기 실행 표면 정책](async-coroutine-policy.md)에
정의한 언어별 마지막 실행 메서드로 선택한다.
- **성공 시 reply payload 의 `List<Message>` 만 반환한다.** caller 는 이미
  자기가 보낸 request 의 routing_id 와 request_seq 를 알고 있으므로
  `Received` 를 되돌려 받을 필요가 없다. 별도 `Reply` 타입은 만들지 않는다.
- multipart reply 가 가능하므로 단일 `Message` 가 아닌 `List<Message>` 를
  반환한다. 단일 part reply 는 `list[0]` 으로 꺼낸다.

#### Callback request

builder의 callback submit 메서드 (`submit(callback)`).

- flags 파라미터 있음. builder의 `.flags(...)` 단계로 전달하며,
  `DONTWAIT` 으로 non-blocking submit 가능.
- timeout 은 builder의 `.timeout(...)` 단계로 전달한다. 지정하지 않으면 소켓
  기본 timeout 을 사용한다.
- submit 단계는 아래처럼 해석한다.
  - exception 기반 언어:
    blocking 성공=`true`, non-blocking temporary backpressure=`false`,
    그 외 submit 실패=예외
  - return-based 언어:
    기존 에러 반환 계약 유지
  실패 시 callback 은 등록되지 않는다.
- submit 성공 시 callback 이 정확히 한 번 호출된다.
  - 성공: `result = OK`, reply parts 포함
  - 실패: `result != OK` (TIMED_OUT 등), parts 는 empty / null / None /
    `Option::None`
- callback 시그니처는 언어 관용구를 따르며 **reply payload 는 `List<Message>`**
  로 전달한다 (`Received` 가 아니다):
  - 공통 패턴 (C++/Java/.NET/Node/Python/Go):
    `(RequestResult result, List<Message> parts)` — 결과 enum 과 parts 리스트
  - Rust 관용구: `FnOnce(Result<Vec<Message>, RequestError>)` — `Result` 타입
    이 Rust 에서 에러 + 값을 표현하는 표준 방식이므로 이 패턴을 허용한다.
    `RequestError::code` 는 `RequestResult` enum 값과 1:1 대응한다.

#### 공통

- `zlink_request_result_t` 전체 정의는
  [errno-map.md](https://kairos-code-dev.github.io/zlink/en/spec/core/04-errno-map/) 를 참조한다.
- Go / Rust 는 exception 이 없으므로 callback request 의 submit 실패도
  return-based 로 처리한다 (Go: `*SubmitError` 반환, Rust:
  `Result<_, SubmitError>` 반환).

## 도메인 객체 정책
- Java, C#, Go, Rust, Node, Python은 가능하면 `out` 파라미터나 raw tuple보다
  도메인 객체를 우선한다.
- 최소 핵심 도메인 모델:
  - `Message`
  - `RoutingId`
  - `Received`
  - `TopicMessage`
  - `SubscriptionEvent`
  - `SubmitResult` (C / Go / Rust — return-based 언어에서 반환 객체/에러에
    포함. exception 언어에서는 예외 객체 `.code` 로 노출)
- 결과 객체는 payload shape, ownership, optional routing metadata를 함께
  설명해야 한다.
- 편의 기능은 결과 객체 메서드로 둔다.
  - 예: `singlePartOrThrow()`

### 도메인 객체 Canonical Shape (모든 바인딩 공통)

각 도메인 객체는 아래 canonical field/method 집합을 **그대로** 노출한다.
언어별로 명명법(camelCase / snake_case / PascalCase) 만 변환하고,
**필드 타입과 메서드 의미는 바꾸지 않는다.** 언어별 관용 편의 메서드를
추가할 수는 있지만, canonical 메서드를 대체하거나 일부만 생략하면 안 된다.

#### `Message`

transport payload 를 담는 단일 message part 다. 모든 send/request/reply/publish
builder 는 하나 이상의 `Message` 를 누적해서 multipart payload 를 만든다.

| 구성 | 타입 | 의미 |
|------|------|------|
| empty constructor | ctor/static | zero-length message 생성 |
| `allocate(size)` | static/ctor | `size` bytes payload buffer 생성 |
| `from(bytes)` | static/ctor | bytes-like 입력을 message-owned storage 로 복사 |
| `from(string)` | static/ctor | 사용자 문자열을 UTF-8 payload 로 인코딩 |
| `copy()` / `from(Message)` | `Message` | source payload 를 새 message 로 복사 |
| `move()` / consume path | `Message` / builder step | 명시적 ownership 이전. 호출 뒤 source 는 재사용 불가 |
| `size` | `int` / `usize` | payload byte length |
| `is_empty()` | `bool` | `size == 0` |
| `to_bytes()` | `bytes` / `byte[]` / `Vec<u8>` | payload snapshot copy |
| `data` / `as_bytes()` | view | payload read view. close 이후 lifetime 보장 없음 |
| `mutable_data` / `as_mut_bytes()` | mutable view | allocated payload 를 채우는 mutable view |
| `copy_to(destination)` | `int` / `bool` | caller-provided buffer 로 payload 복사 |
| `to_string()` / `as_str()` | `string` / result | UTF-8 decode convenience |
| `get_property(name)` | `string?` / result | native message string property 조회 |
| `ref_count()` | `int` | native storage reference count 진단값 |
| `close()` / `Dispose()` / `Drop` | — | native storage 정리. 언어별 lifecycle 관용구 적용 |

언어별 이름은 관용구를 따른다. 의미는 아래 슬롯에 맞춘다.

| 의미 | .NET | Java | Node | Python | Rust | C++ | Go |
|------|------|------|------|--------|------|-----|----|
| 빈 message | `new Message()` | `new Message()` | `Message.from(Buffer.alloc(0))` 또는 equivalent | `Message()` | `Message::new()` | `message_t()` | `NewMessage(nil)` |
| 크기 allocation | `Allocate(size)` | `allocate(size)` | `allocate(size)` | `allocate(size)` | `with_size(size)` / `allocate(size)` | `allocate(size)` | `NewMessageWithSize(size)` |
| bytes copy | `From(bytes)` | `from(byte[])` | `from(BufferLike)` | `from_(buffer)` | `try_from(bytes)` | `from(...)` | `NewMessage(data)` |
| UTF-8 string | `From(string)` | `from(String)` | `from(string)` | `from_(str)` | `TryFrom<&str>` 또는 equivalent | `from(std::string)` | `NewMessageString` |
| external buffer copy | — | `from(ByteBuffer)` / `from(ByteBuf)` | `from(BufferLike)` | `from_(buffer)` | `try_from(bytes)` | `from(...)` | `NewMessage(data)` |
| message copy | `Copy()` | `from(Message)` | `copy()` 또는 `from(Message)` | `copy()` | `Clone` 또는 `try_clone()` | copy constructor | `Clone()` / `Copy()` |
| explicit move | `Move()` / `MoveMessage(...)` | `move()` / `moveMessage(...)` | `moveMessage(...)` | `move_message(...)` | move-by-value | move constructor / rvalue builder | `MoveMessage(...)` |
| bytes snapshot | `ToArray()` | `toByteArray()` | `toBytes()` | `to_bytes()` | `to_vec()` | `to_bytes()` | `BytesCopy()` 또는 equivalent |
| read view | `AsReadOnlySpan()` | `dataBuffer()` | `data()` | `data` | `as_bytes()` | `bytes()` | `Data()` |
| mutable view | `AsSpan()` | `mutableDataBuffer()` | `data()` | `data` | `data_mut()` | `bytes()` / `data()` | `Data()` |
| UTF-8 decode | `GetString()` | `toUtf8String()` | `toString()` / `getString()` | `to_string()` / `decode` helper | `as_str()` | `to_string()` | `String()` / `Text()` |
| property | `GetProperty(name)` | `getProperty(name)` | `getProperty(name)` | `get_property(name)` | `get_property(name)` | `property(name)` | `GetProperty(name)` |
| refcount | `RefCount` | `refCount()` | `refCount()` | `ref_count()` | `ref_count()` | `ref_count()` | `RefCount()` |

규칙:
- `from(bytes)` 계열은 항상 message-owned storage 로 복사한다. caller 는 입력
  buffer 를 이후 자유롭게 변경하거나 해제할 수 있어야 한다.
- Java `from(ByteBuf)` 는 Netty `ByteBuf` 의 readable bytes 를 복사하되
  `readerIndex` 를 변경하지 않는다. `copyTo(ByteBuf)` 는 destination 의
  writable 영역에 쓰고 `writerIndex` 를 증가시킨다.
- borrowed / zero-copy 생성자는 canonical public contract 가 아니다. 특정
  바인딩이 내부 최적화로 쓰더라도 public API 에서 lifetime 책임을 caller 에게
  떠넘기면 안 된다.
- `message(...)` builder 단계는 원본 보존 계약을 따른다. submit 실패 뒤에도
  caller 가 넘긴 message 를 다시 사용할 수 있어야 한다.
- ownership 이전은 `move`, `MoveMessage`, move-by-value 처럼 이름에서 consume
  의미가 드러나는 별도 경로에서만 허용한다. 이 경로는 submit 실패 뒤에도 원본
  message 를 재사용할 수 없다는 계약을 문서화해야 한다.
- `to_bytes()` 는 snapshot copy 다. allocation 없는 payload 접근은 read view
  API(`data`, `as_bytes`, `AsReadOnlySpan` 등)로 분리한다.
- read / mutable view 는 message 가 close / dispose / drop 되기 전까지만
  유효하다. 바인딩은 close 뒤 view 사용을 보장하지 않는다.
- `get_property(name)` 은 native message metadata 를 읽는 진단/interop API 다.
  property 쓰기 API 는 공통 필수 계약이 아니다.
- `ref_count()` 는 진단값이다. reference count 값으로 ownership 정책이나 send
  가능 여부를 판단하는 public contract 를 만들면 안 된다.
- RAII 언어(C++, Rust)는 `close()`를 명시 노출하지 않아도 된다. 명시 lifecycle
  언어(.NET, Java, Python, Go)는 idempotent close/dispose 를 제공해야 한다.
- closed / moved-from message 에 대한 `size`, `data`, `get_property` 동작은
  언어별 관례를 따르되, 빈 값 반환인지 예외/에러인지 문서화해야 한다.

#### `TopicMessage`

raw `SUB` / `XSUB` 와 `Spot subscribe` 의 recv 결과다.
raw pub/sub 는 C API `zlink_subscribe_part()` 를, Spot subscribe 는
`zlink_spot_subscribe_part()` 를 바인딩 도메인 객체 하나로 감싼다. 바인딩
public API는 part helper 호출 결과를 언어별 multipart 객체로 조립해서 돌려준다.

| 구성 | 타입 | 의미 |
|------|------|------|
| `routing_id` | `RoutingId?` (optional) | 송신자 routing id. transport 가 carry 안 하면 null/None/empty |
| `topic` | **`string` (UTF-8)** | 매칭된 topic. **bytes 가 아니다.** |
| `parts` | `List<Message>` / `Vec<Message>` | multipart payload |
| `is_single_part()` | `bool` | `parts.size() == 1` |
| `first_part()` | `Message` | `parts[0]`; 비어있으면 에러/예외 |
| `single_part_or_throw()` | `Message` | `is_single_part()` 면 part 반환, 아니면 에러/예외 |
| `close()` / `Dispose()` / `Drop` | — | 보유 parts 정리. 언어별 lifecycle 관용구 적용 |

규칙:
- `Subscribed` 나 그와 유사한 subclass 를 만들지 않는다. `TopicMessage`
  하나만 노출한다.
- Spot subscribe 결과는 `topic + parts` 를 함께 노출한다. channel 은
  `Spot` handle 이 이미 묶인 `SpotNode` 쪽 상태이므로 메시지 결과 필드로
  반복하지 않는다.
- `topic` 은 UTF-8 `string` 이다. `bytes` / `byte[]` / `Vec<u8>` 으로
  노출하지 않는다 (내부적으로 raw bytes 로 왔더라도 공개 API 는 decode).
- `RoutingId` 필드는 typed `RoutingId` 하나만 둔다. `RoutingId: string` +
  `RoutingIdValue: RoutingId?` 같은 이중 property 금지.

#### `Received`

PAIR / DEALER / ROUTER / STREAM / SPOT routed recv 결과를 담는 단일 canonical
도메인 객체다. topic 필드가 없는 점 외에는 `TopicMessage` 와 동일한 편의
메서드 집합을 가진다. routed recv 결과는 일반 응답 전송용 `send()` operation
builder 를 제공하고, request-reply 결과는 `reply()` builder 도 함께 제공한다.
두 entrypoint 모두 `Operation Builder Policy` 를 따라 payload 와 옵션을
builder 단계로 누적한다.

`Received` 는 socket 종류별 message wrapper 가 아니다. request 의 의미는
DEALER, ROUTER, SPOT 에서 동일하며, `request_seq` 와 reply context 로만
표현한다. binding 은 `DealerReceived` / `RouterReceived` / `SpotReceived` 같은
protocol-specific public 결과 타입을 새 canonical 표면으로 추가하면 안 된다.
기존 binding 에 이런 타입이 있으면 제거하고, 새 코드, sample, perf, framework
연동은 `Received` 를 사용해야 한다.

| 구성 | 타입 | 의미 |
|------|------|------|
| `routing_id` | `RoutingId?` | 송신자 routing id (router=peer_rid, spot=source_node_rid) |
| `spot_rid` | `RoutingId?` | SPOT routed recv 에서만 설정 (source_spot_rid) |
| `request_seq` | `uint64?` | request-reply 모드일 때 설정, 아니면 null |
| `parts` | `List<Message>` | multipart payload |
| `is_single_part()` | `bool` | 동일 |
| `first_part()` | `Message` | 동일 |
| `single_part_or_throw()` | `Message` | 동일 |
| `send()` | `SendOp` | 이 `Received` 의 송신자에게 일반 routed message 를 보내는 operation builder. routed source context 가 없으면 submit 시점에 `SubmitError` |
| `reply()` | `ReplyOp` | request 였을 때만 유효한 reply operation builder. `request_seq` 없거나 reply context 가 invalid 하면 submit 시점에 `SubmitError` |
| `close()` / 동등 | — | 동일 |

.NET 에서는 `Received.Create()` 가 caller-provided recv 저장소를 만드는
canonical 생성 경로다. `Received` 는 public concrete contract 타입으로 유지한다.

`request_seq` 규칙:
- `null` / `None` / empty `Optional` / `hasRequestSeq == false` 는 ordinary
  receive 결과를 뜻한다.
- `0` 은 public high-level `Received` 에서 "request 있음"으로 노출하지 않는다.
  core out-param 의 `request_seq == 0` 은 high-level binding 에서 absent 로
  변환한다.
- non-zero `request_seq` 는 request-reply context 가 있는 수신 결과를 뜻한다.
  이 의미는 DEALER / ROUTER / SPOT 에서 동일하다.
- request/reply message type 같은 substrate 세부 구분은 public `Received`
  의미를 갈라서는 안 된다. 그런 값이 실제 public 계약으로 필요하면
  protocol-specific 결과 타입이 아니라 `Received` 의 공통 metadata 로만
  노출한다.

`send()` 규칙:
- request 여부와 무관하다. `request_seq` 가 없어도 routed source context 가
  있으면 호출할 수 있다.
- `send()` 는 request-reply 의미를 갖지 않는다. 단순히 이 `Received` 를 보낸
  쪽으로 일반 routed message 를 보낸다.
- `ROUTER` 와 `STREAM` 수신 결과는 peer routing id 로 보낸다.
  `SPOT` routed 수신 결과는 source node rid 와 source spot rid 로 보낸다.
- payload 누적과 `flags(...)` 같은 옵션은 `SendOp` builder 단계로 표현하며,
  `DONTWAIT` 같은 non-blocking submit flag 도 builder 의 `.flags(...)`
  단계로 전달한다.

`reply()` 규칙:
- **`request_seq` 가 `null` 이면 호출 금지**. 호출 시 builder 의 submit 단계에서
  `SubmitError` 계열로 처리한다. `request_seq == 0`, 잘못된 `(routing_id,
  request_seq)` 조합 등 invalid reply context 도 같은 submit domain 으로
  본다.
- `Received` 가 내부적으로 source socket 참조를 보유한다 (binding 이 recv /
  handler 에서 Received 를 만들 때 주입).
- socket 이 close 된 후 `reply().submit()` 호출하면 `SubmitError(TERMINATED)`.
- 서버 측 사용자가 `(peerRid, requestSeq)` 를 따로 보관할 필요 없음 —
  `Received` 하나로 완결.
- 별도 `router.reply(peerRid, seq).message(...).submit()` 경로도 pull-mode
  호환성 위해 남겨두되, **권장 경로는 `received.reply().message(...).submit()`**.

#### `SubscriptionEvent`

XPub 이 받는 subscribe/unsubscribe 이벤트와 Spot subscription event recv 결과다.

| 구성 | 타입 | 의미 |
|------|------|------|
| `routing_id` | `RoutingId?` | 구독자 routing id |
| `topic` | `string` (UTF-8) | 구독/해제 topic |
| `subscribed` | `bool` | true=subscribe, false=unsubscribe |

규칙:
- value object 로만 노출한다 (메서드 없음, 필드만).
- `close()` 등 lifecycle 없음 (값 타입).
- Spot subscription event 결과는 `topic + subscribed` 를 노출한다.

#### `RoutingId`

Routing id value object. Binary-safe (1-255 bytes).

| 구성 | 타입 | 의미 |
|------|------|------|
| `bytes` / `data` | `bytes` / `byte[]` / `Vec<u8>` / `Buffer` | raw bytes (immutable view) |
| `size` | `int` (1-255) | byte length |
| `from(bytes)` | static/ctor | raw bytes 로 생성 |
| `from(value: string)` | static/ctor | 사용자 문자열을 UTF-8 bytes 로 인코딩 |
| `from_hex(value)` | static/ctor | `to_hex()` 로 만든 hex 문자열을 다시 생성 |
| `from(value: uint32)` | static/ctor | 4바이트 big-endian `uint32` routing id 생성 |
| `from(value: guid)` | static/ctor | 16바이트 UUID routing id 생성 |
| `to_bytes()` | `bytes` | 원본 바이트 반환 |
| `to_hex()` | `string` | raw bytes 를 hex 문자열로 표시 |
| equality / hash | — | 언어별 관용구 (`equals`/`hashCode`, `__eq__`/`__hash__`, `PartialEq+Eq+Hash`) |

언어별 이름은 관용구를 따른다. 의미는 아래 슬롯에 맞춘다.

| 의미 | .NET | Java | Node | Python | Rust | C++ | Go |
|------|------|------|------|--------|------|-----|----|
| 사용자 문자열 | `From(string)` | `from(String)` | `from(string)` | `from_(str)` | `From<&str>` | `from(std::string)` | `NewRoutingIDString` |
| raw bytes | `From(bytes)` | `from(byte[])` | `from(Buffer)` | `from_(bytes)` | `From<&[u8]>` | `from(bytes)` | `NewRoutingID` |
| hex round-trip | `FromHex` | `fromHex` | `fromHex` | `from_hex` | `from_hex` / `try_from_hex` | `from_hex` | `NewRoutingIDFromHex` |
| uint32 | `From(uint)` | `from(long)` | `from(number)` | `from_(int)` | `From<u32>` | `from(uint32_t)` | `NewRoutingIDUint32` |
| UUID | `From(Guid)` | `from(UUID)` | 16-byte `from(Buffer)` | `from_(uuid.UUID)` | `From<[u8; 16]>` | `from(std::array<uint8_t, 16>)` | `NewRoutingIDUUIDBytes` |

규칙:
- **binary-safe value type**. 사용자 설정 routing id 는 보통 사람이 읽는
  문자열이므로 string overload `from(value)` 는 UTF-8 인코딩을 뜻한다.
  native/core 에서 받은 임의 bytes 는 bytes overload `from(bytes)` 로 보존한다.
- `from_hex(value)` 입력은 hex 문자만 허용한다. hex 문자열은 최대
  510자이며, 디코딩된 routing id가 255 bytes를 넘으면 언어별
  예외나 에러 코드로 실패해야 한다.
- 4바이트 STREAM routing id 처럼 core 가 `uint32_t`로 다루는 값은
  `from(value: uint32)` / `try_to_uint32(out value)` 같은 typed API로 다룬다.
- 16바이트 UUID 값은 `from(value: guid)` / `try_to_guid(out value)` 같은
  typed API로 다룬다.
- `to_string()` / `String()` 은 언어별 표시용 문자열이다. printable UTF-8이면
  그대로, 4바이트 `uint32`이면 숫자 문자열, 16바이트 UUID이면 UUID 문자열,
  그 외에는 `hex:` 접두어가 붙은 hex 표시를 권장한다. round-trip 저장에는
  `to_hex()` / `from_hex(value)` 를 사용한다.
- 불변 (immutable). 한 번 생성하면 내용 변경 불가.
- 캐싱은 관찰 가능한 계약이 아니다. 바인딩은 필요하면 hash, native struct,
  recv hot path 의 짧은 lived cache 를 내부에서 사용할 수 있지만, 동등성은
  항상 bytes 값으로 판단해야 하며 cache hit 여부가 API 동작을 바꾸면 안 된다.
- Node 에서는 raw `Buffer` 대신 `RoutingId` 래퍼 타입을 그대로 노출한다.

#### `MonitorEvent`

socket monitor 가 방출하는 이벤트. 모든 바인딩이 **필수 노출**.

| 구성 | 타입 | 의미 |
|------|------|------|
| `event` | `MonitorEventType` (enum) | 이벤트 종류 (CONNECTION_READY, CONNECTED, DISCONNECTED 등) |
| `value` | `uint32` | 이벤트 별 상세 값 (예: DISCONNECTED 시 reason code) |
| `routing_id` | `RoutingId?` | 해당 peer routing id (없는 이벤트는 null) |
| `local_addr` | `string` | 로컬 endpoint |
| `remote_addr` | `string` | 원격 endpoint |

#### `MonitorStatus`

socket monitor 가 제공하는 런타임 상태 스냅샷. 모든 바인딩이 **필수 노출**.

| 구성 | 타입 | 의미 |
|------|------|------|
| `source_kind` | enum | 모니터 대상 종류 |
| `state_flags` | enum flags | 상태 비트마스크 |
| `detail_flags` | enum flags | 세부 비트마스크 |
| `snd_pending_msgs` | `uint64` | 송신 큐 대기 메시지 수 |
| `rcv_pending_msgs` | `uint64` | 수신 큐 대기 메시지 수 |
| `auto_hwm_*` diagnostic fields | enum / number / bigint | C `zlink_monitor_status_t`의 canonical auto-HWM 필드를 같은 의미로 노출해야 한다. enabled, profile(enum), role, policy class, unit budget, size cap, socket message slots, effective message bytes, applied HWM, applied buffer, 최근 재계산 이유(enum), deferred shrink, blocked ratio를 포함한다 |
| `is_ready()` | `bool` | raw socket monitor source에서만 `state_flags` 의 ready 비트 확인 편의 메서드 |

#### 서비스 계층 엔트리 객체

아래는 service-layer snapshot/query 에서 반환되는 value object 들.
모든 바인딩이 **필드 목록을 spec 에 명시**해야 한다 (C 구조체를 그대로
노출하면 안 되며 언어별 named field 로 래핑).

- `SpotNodeStatus` — spot node 상태 스냅샷
- `SpotNodePeerEntry` — spot node peer 엔트리.
  `weight` 를 포함해야 한다.
- `SpotNodeSubjectEntry` — spot node subject 엔트리

각 spec 은 이들 타입의 필드를 표 또는 코드 블록으로 명시한다. `Cpp` 는
raw `zlink_*_t` 구조체를 바인딩 API 표면으로 노출하지 않고 `class
<name>_t { ... }` 형식으로 래핑한다.

위 canonical 을 벗어난 추가 메서드/필드는 정책 위반이다. 언어별 spec 에서
누락이 발견되면 canonical 기준으로 채워 넣고, 추가된 비표준 메서드는 삭제한다.

## 소켓 타입 능력 정책
- 소켓 타입별 능력은 타입 자체에만 노출한다.
- 관련 없는 소켓은 관련 없는 함수에 접근할 수 없어야 한다.
  - 예: `PairSocket`에 publish/subscribe/xpub control surface 금지
  - 예: `StreamSocket`에 일반 connect surface 금지
- 소켓 타입별 option도 타입별 역할 facade로만 노출한다.

### 소켓 클래스 네이밍/구조 규칙 (중요)
- **소켓 클래스 이름은 core C API 의 socket 타입 이름을 그대로 따른다**:
  `PairSocket`, `PubSocket`, `SubSocket`, `XPubSocket`, `XSubSocket`,
  `DealerSocket`, `RouterSocket`, `StreamSocket`. 바인딩이 임의로 이름을
  바꾸거나 동의어(`ClientSocket`, `BrokerSocket` 등)를 추가하면 안 된다.
- **소켓의 기능 함수(`send`, `recv`, `request`, `reply`, `publish`,
  `subscribe`, `on*` 핸들러 등)는 소켓 클래스의 메서드로 직접 노출한다**.
  단일 함수 또는 좁은 역할(예: request-reply) 만을 위한 별도 wrapper/
  "helper" 클래스(`RequestDealer`, `RequestRouter`, `DealerClient`,
  `RouterRequester` 등) 를 만들지 않는다.
  - 이유 1: C API 는 `zlink_dealer_request_part()` /
    `zlink_router_request_part()` / `zlink_router_reply_part()` 를 raw socket
    handle 위에 직접 두는 계약이다.
    바인딩 표면이 이 구조를 유지해야 core ↔ 바인딩 대응이 1:1 로 유지된다.
  - 이유 2: wrapper 클래스는 "래핑된 소켓을 또 하나 들고 다녀야 하는"
    중복 lifecycle 을 만든다.
  - 이유 3: 이름에서 역할이 반전돼 읽히기 쉬움 (`RequestDealer` →
    "requests 를 dealing" 으로 오독).
- Future/Promise 완료 연결 같은 구현 상태(pending map 등)는 소켓 클래스
  내부에 두고, 외부로는 메서드만 노출한다.
- 예외는 **서로 다른 소켓 타입을 조합**하는 service-layer surface 뿐이다
  이들은 단일 소켓 함수 wrapper 가 아니라 독립된 service 계약이다.
- 이 규칙은 전 바인딩(C++/Java/.NET/Node/Python/Go/Rust) 에 동일하게
  적용되며, spec 파일에서 위반이 발견되면 **즉시 수정 대상**이다.

### Socket Capability Matrix
- 이 표는 `core/include/zlink.h` C API를 기준으로 각 소켓 타입이 가져야 할
  능력을 정의한다.
- 이 표는 모든 언어 바인딩에 공통으로 적용되는 공개 기능 계약이다.
  바인딩마다 기능성이 달라지면 안 된다.
- 언어별 차이는 케이싱, overload, nullable, exception/error 표현처럼
  같은 기능을 해당 언어 관례에 맞게 드러내는 방식에만 허용된다.
- 각 바인딩은 이 표를 정답으로 삼아 surface test를 작성한다.
- `Y`는 모든 바인딩이 해당 능력을 반드시 public API로 노출해야 함을
  의미한다.
- `—`는 어떤 바인딩도 해당 능력을 public API로 노출하면 안 됨을
  의미한다.

#### Connection Capabilities

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `bind` | Y | Y | Y | Y | Y | Y | Y | Y |
| `unbind` | Y | Y | Y | Y | Y | Y | Y | Y |
| `connect` | Y | Y | Y | Y | Y | Y | Y | — |
| `disconnect` | Y | Y | Y | Y | Y | Y | Y | — |
| `disconnectRid` | Y | Y | Y | Y | Y | Y | Y | — |

`disconnectRid` 는 connectable raw socket 의 peer-rid disconnect 표면이다.
`STREAM` 은 bind-only socket 이며 `connect`, `disconnect`, `disconnectRid`
를 public API 로 노출하지 않는다. `Spot` 도 raw peer-rid disconnect 를
노출하지 않고, SPOT node peer 연결 해제는 `SpotNode.disconnectPeerRid`
계열이 담당한다.

#### Send Capabilities

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `send` | Y | Y | — | — | — | — | — | — |
| `send(routingId)` | — | — | Y | — | — | — | — | Y |
| `publish` | — | — | — | Y | — | Y | — | — |

#### Receive Capabilities

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `recv` | Y | Y | Y | — | — | — | — | Y |
| `subscribe` | — | — | — | — | Y | — | Y | — |
| `receiveSubscriptionEvent` | — | — | — | — | — | Y | — | — |

#### Subscription Management

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `setSubscription` | — | — | — | — | Y | — | Y | — |
| `unsetSubscription` | — | — | — | — | Y | — | Y | — |

#### Callback Capabilities

| Capability | Pair | Dealer | Router | Pub | Sub | XPub | XSub | Stream |
|---|---|---|---|---|---|---|---|---|
| `setPacketHandler` | — | — | — | — | — | — | — | Y |
| `onReceive` | — | — | — | — | — | — | — | — |
| `onSubscribe` | — | — | — | — | — | — | — | — |
| `setSendReadyHandler` | Y | Y | Y | Y | — | Y | — | Y |

`STREAM` public surface 는 `recv` 와 `setPacketHandler` 를 제공해야 한다.
raw direct callback `onReceive` 는 canonical public binding API 가 아니다.
이미 한 수신 모드가 걸려 있는 상태에서 다른 모드 attach 를 시도하면
`HandlerResult::BUSY` (또는 동등한 `EBUSY`) 를 반환한다. public
`detachStream` / `streamDetach` 류 해제 API 는 제공하지 않는다.

#### Typed Option Capabilities

| Option Facade | 적용 소켓 |
|---|---|
| Common options (linger, HWM, timeout 등) | 전체 |
| Router options (mandatory, handover, probe, connectRoutingId) | Router |
| Dealer options (probe) | Dealer |
| Stream options (notify) | Stream |
| Pub options (verbose, verboser, noDrop, manual 등) | Pub, XPub |
| Sub options (topicsCount) | Sub, XSub |
| RoutingId (set/get) | Dealer, Router, Stream |

  `disconnectRid`, `unbind`, `close`는 차단된다.

## 언어별 스펙 파일 준수 규칙

각 언어별 스펙 파일(`doc/spec/bindings/{lang}/README.md`)은 아래 규칙을
반드시 준수해야 한다. 스펙 파일 작성이나 리뷰 시 이 체크리스트를 적용한다.

### Capability Matrix 정합성
- 각 언어별 스펙은 위 Socket Capability Matrix의 기능성을 빠뜨리거나
  추가하면 안 된다.
- 각 소켓 타입 클래스는 위 Socket Capability Matrix에서 `Y`인 능력을
  해당 언어 관례에 맞는 public surface로 반드시 제공해야 한다.
- `—`인 능력은 어떤 언어 바인딩에서도 해당 소켓 타입 클래스에 존재하면
  안 된다.
- Socket Capability Matrix가 다루지 않는 service layer 기능은 별도
  역할 matrix 또는 정책 섹션에 명시된 경우에만 public API로 노출할
  수 있다.
- 특히 다음 위반이 자주 발생하므로 주의한다:
  - `RouterSocket` / `StreamSocket`에 plain `send` (routingId 없는 send) 금지 —
    반드시 `send(routingId, ...)` 형태여야 한다.
  - `StreamSocket`에 `connect`, `disconnect`, `disconnectRid` 노출 금지 —
    `STREAM`은 bind-only socket이다.
    Dealer, Router, Pub, Sub에만 허용된다.
  - `XPubSocket`에 `onSubscribe` 콜백 금지 —
    XPub는 `receiveSubscriptionEvent`만 허용된다.
  - `STREAM` raw direct callback `onReceive` 및 `detachStream` 류 해제 API 금지 —
    canonical surface 는 `recv` / `setPacketHandler` / `close` 조합이다.
  - socket 공통 TLS helper(`setTlsServer`, `setTlsClient` 또는 동등한 이름) 누락
    금지 — TLS 설정은 transport 공통 기능이므로 모든 raw socket 타입에서 같은
    위치에 있어야 한다.

### Routed Send 필수 인자
- `RouterSocket`과 `StreamSocket`의 send는 routingId를 **필수** 인자로 받아야 한다.
- routingId를 optional/default 파라미터로 만들면 plain send가 가능해지므로 금지한다.

### Send / Publish 반환값
- `.NET` / `Java` / `Node` / `Python` / `C++` 에서는 blocking
  `send` / `publish` / callback `request` submit 성공 시 항상 `true` 를
  반환한다.
- 위 언어의 non-blocking submit 에서는 temporary backpressure 일 때만
  `false` 를 반환한다.
- temporary backpressure 가 아닌 submit 실패는 예외로 전달해야 한다.
- 상태 코드(int, number 등)를 반환하는 방식은 금지한다.

### 언어별 네이밍 일관성
- 한 바인딩 내에서 네이밍 컨벤션이 혼재되면 안 된다.
  - Python: 모든 public API는 `snake_case`. (프로퍼티 포함)
  - Java: `camelCase` 메서드, `PascalCase` 클래스.
  - C#: `PascalCase` 전체.
  - Go: `PascalCase` exported.
  - Rust: `snake_case` 메서드, `PascalCase` 타입.
  - C++: `snake_case` 메서드. 타입명은 한 바인딩 안에서 일관되게
    유지한다. `_t` 접미사는 핸들/값 래퍼 타입이나 타입명과 메서드명이
    충돌하기 쉬운 경우에 사용할 수 있지만, 모든 enum/class에 강제하지
    않는다. 접미사 규칙을 맞추기 위해 같은 타입에 별도 alias만 추가하지
    않는다.
  - Node/TypeScript: `camelCase` 메서드, `PascalCase` 클래스.

### C API 전수 커버리지
- 각 언어별 스펙 파일은 `core/include/zlink.h`와 `core/include/zlink/**`
  하위 public header의 모든 ZLINK_EXPORT 함수에
  대응하는 바인딩 인터페이스를 빠짐없이 기술해야 한다.
- 대응은 1:1이 아닐 수 있다 (옵션 함수 그룹이 하나의 typed facade로 통합되는 등).
- 그러나 C API의 어떤 기능도 바인딩 스펙에서 누락되면 안 된다.
- 새로운 C API가 public header에 추가되면 모든 언어 스펙 파일도 함께 갱신해야 한다.
## 서비스 계층 정책
- 이 섹션은 소켓 레이어 위에 올라가는 서비스 계층(Spot, Actor)의 public API
  정책을 정의한다.
- 서비스 계층도 소켓 계층과 동일한 POSD 원칙, naming policy, error policy,
  ownership policy, testing policy를 따른다.
- 서비스 계층의 기준은 `core/include/zlink.h`의 Spot/Actor C API다.

### Spot / SpotNode Lifecycle (POSD 원칙)

- **`SpotNode` 가 lifecycle 소유자**다. `Spot` 은 그 위의 pub/sub facade 로,
  `SpotNode` 가 살아 있는 동안만 유효하다.
- `Spot` 은 독립 생성자로 만들지 않는다. **`SpotNode.createSpot(...)` 등
  factory 메서드로 생성**한다. 이름은 언어 관용구대로 (`spot_node.new_spot`,
  `spotNode.createSpot`, 등).
- 명시적 Spot routing id를 기준으로 "있으면 가져오고 없으면 만든다"는 흐름은
  `zlink_spot_node_spot_get_or_new(...)`에 직접 대응하는
  `SpotNode.getOrCreateSpot(...)` 계열 메서드로 노출한다. 바인딩은 lookup과 create를
  조합해서 이 의미를 흉내 내면 안 된다.
- `Spot` 생명은 부모 `SpotNode` 에 바인드된다.
  - `spot.close()` — Spot 만 끝내고 node 는 유지
  - `spotNode.close()` — node 와 그 아래 모든 live Spot 을 함께 정리
    (cascading close)
- 사용자가 `Spot` 과 `SpotNode` 의 close 순서를 수동으로 조합할 필요를
  제거한다. 바인딩이 `SpotNode.close()` 에서 child spots 를 선처리한 후
  node 를 내린다.
- C API 의 raw `zlink_spot_new(...)` + `zlink_spot_node_new(...)` 조합을
  바인딩 public 생성자로 그대로 노출하지 않는다. 반드시 `SpotNode` 중심의
  factory 패턴으로 싼다.

### 서비스 계층 인트로스펙션 표면 계층

서비스 계층의 introspection / snapshot / entry 타입은 **사용 빈도에 따라
두 계층으로 구분**한다. 바인딩 spec 은 이 구분을 반영한다.

- **Primary (핵심)**: 일반 사용자가 자주 쓰는 snapshot/query surface.
  `bindings/<lang>/README.md` 의 상위 섹션에 기술한다.
  - `SpotNodeStatus` (spot node 상태)

- **Advanced / Diagnostic (진단용)**: 디버깅 / 운영 모니터링 등 특수 용도.
  spec 에서 "Advanced" 또는 "Diagnostic" 하위 섹션으로 분리 기술한다.
  - `SpotNodePeerEntry`, `SpotNodeSubjectEntry`
  - `SpotNodeSocketEntry`, `SpotNodeSpotEntry`,
    `SpotNodeActorEntry`
  - 각종 filter 타입 (`SpotNodePeerFilter`, `SpotNodeSubjectFilter`,
    `SpotNodeSocketFilter`)

Primary 타입만으로 기본 사용 시나리오가 성립해야 한다. Advanced 타입을
배우지 않고도 "서비스 등록 / 검색 / 연결" 흐름이 완결돼야 한다.

### `zlink_errno()` 공개 노출

- 바인딩은 **raw `zlink_errno()` / `zlinkErrno()` 함수를 public 으로 노출하지
  않는다**. 에러 상세는 **언제나 에러 타입의 `internalErrno` /
  `internal_errno` 필드**로만 접근한다.
- 사용자가 에러 조사 시 "가끔 `ZlinkException.getCode()` 쓰고 가끔 `Zlink.
  errno()` 쓰는" 이중 경로를 만들지 않는다 — 한 진입점으로 통일.
- 바인딩 내부 구현이 `zlink_errno()` 를 호출해 예외 객체에 채워 넣는 건
  허용 (내부 해석용). public surface 에만 금지 적용.
- `Zlink.strerror(errno)` 같은 message lookup 유틸은 convenience 로 남겨두되,
  raw `errno()` accessor 는 private 또는 삭제.

### 서비스 계층 아키텍처
- 서비스 계층의 현재 공개 축은 `SpotNode`, `Spot`, `Actor`,
  `StreamSocket`의 Actor binding 표면, 그리고 SPOT route bridge/publisher
  표면이다. Public Discovery/Registry handle은 core 8.4.3에서 제거되었으므로
  새 바인딩 표면으로 되살리면 안 된다.

```
SpotNode
  |-- bind
  |-- raw mesh: connectPeer, disconnectPeer
  |   createPublisher
  |-- actor: create, lookup, remote create, join, leave
  |-- introspection: status, peers, peers(filter),
  |   subjects, spots, actors
  `-- TLS: setTlsServer, setTlsClient

Spot
  |-- publish, subscribe
  |-- sendToChannel, requestToChannel
  |-- sendToSpot, requestToSpot, requestToRouter
  |-- replyToSpot, replyToRouter
  |-- actor join: recvActorJoin, replyActorJoin, actors
  |-- actor lifecycle: recvActorLifecycle
  |-- setSubscription, unsetSubscription
  |-- setDispatchHandler, setSendReadyHandler
  `-- close facade only

Actor
  |-- ref: nodeRid, actorId, generation
  |-- receive: recvPart
  |-- bound session: send, close
  `-- close lifecycle handle

StreamSocket
  |-- bindActor, unbindActor
  `-- sendBoundActor

  |-- connect
  |-- snapshot
  `-- close
```

### Actor Dispatch 정책

Actor dispatch는 현재 core 공개 헤더에 존재하는 정식 service layer 계약이다.
바인딩은 Actor를 SPOT 내부 세부사항으로 숨기지 않고, `SpotNode`, `Spot`,
`Actor`, `StreamSocket`에 걸친 별도 공개 기능으로 정리한다.

언어가 header, module, package, namespace처럼 public surface를 나누는 단위를
제공한다면 Actor는 독립 entrypoint를 가져야 한다. 이 entrypoint는 단순히
SPOT 전체 헤더나 모듈을 다시 include/import/export하는 얇은 forwarding
파일이어서는 안 된다. Actor entrypoint는 Actor 값 객체, Actor lifecycle
handle, Actor recv/join helper처럼 Actor 계약을 구성하는 public type과
함수 선언을 실질적으로 소유해야 한다. SPOT entrypoint가 Actor entrypoint를
재사용하는 구조는 허용하지만, Actor entrypoint가 SPOT 구현 전체에 기대어
존재만 하는 구조는 정책 미준수다.

기준이 되는 core 공개 타입과 함수는 아래다.

- 타입: `zlink_actor_ref_t`, `zlink_actor_route_t`, `zlink_actor_recv_info_t`,
  `zlink_actor_join_info_t`, `zlink_actor_join_result_t`,
  `zlink_actor_join_entry_spot_result_t`, `zlink_actor_lookup_result_t`,
  `zlink_spot_actor_lifecycle_info_t`, `zlink_actor_join_spot_handler_fn`,
  `zlink_actor_join_entry_spot_handler_fn`, `zlink_actor_lookup_handler_fn`,
  `zlink_spot_node_spot_entry_t`, `zlink_spot_node_actor_entry_t`
- `SpotNode` 축: `zlink_spot_node_actor_new`,
  `zlink_spot_node_actor_lookup`, `zlink_remote_actor_get_ref` (async lookup),
  `zlink_spot_node_actor_destroy` (async submit),
  `zlink_spot_node_actor_join_spot` (async submit + 전용 completion typedef),
  `zlink_spot_node_actor_join_entry_spot` (async submit + 전용 completion typedef),
  `zlink_spot_node_actor_leave_spot` (async submit),
  `zlink_spot_node_actor_recv_part`,
  `zlink_spot_node_actor_send_bound_session_msg`,
  `zlink_spot_node_actor_reply_no_bind`,
  `zlink_spot_node_actor_close_bound_session`
- `Spot` 축: `zlink_spot_actor_join_recv`,
  `zlink_spot_actor_join_reply`, `zlink_spot_recv_actor_lifecycle`,
  `zlink_spot_actors`
- `StreamSocket` 축: `zlink_stream_bind_actor` (async submit),
  `zlink_stream_unbind_actor` (async submit),
  `zlink_stream_send_bound_actor_part`,
  `zlink_stream_bound_actors`
- snapshot 축: `zlink_spot_node_spots`,
  `zlink_spot_node_actors`, `zlink_spot_actors`

바인딩 surface는 아래 책임 분리를 따른다.

| Public owner | Actor 역할 |
|---|---|
| `SpotNode` | local Actor 생성/조회, async remote Actor lookup, async destroy, async join/leave, node-level Actor snapshot |
| `Actor` | Actor ref 보유, Actor recv, bound STREAM session message send, bound session close |
| `Spot` | Actor join request recv/reply, Actor lifecycle event receive, 현재 Spot에 join된 Actor snapshot |
| `StreamSocket` / session facade | async STREAM session Actor bind/unbind, bound Actor 대상 send, session attach 목록 조회 |

바인딩은 아래 도메인 객체를 public contract로 제공해야 한다. 이름은 언어 관례에
맞게 변환할 수 있지만 필드 의미는 바꾸지 않는다.

| 객체 | 필수 의미 |
|---|---|
| `ActorRef` | `node_rid`, `actor_id`, `generation` |
| `ActorRoute` | route 대상 Actor, current Spot routing id, current Spot kind |
| `ActorRecvInfo` | 수신 Actor, source node/session routing id, flags |
| `ActorReceived` | `ActorRecvInfo`, payload parts. 이름은 언어 관례에 따라 바꿀 수 있지만 part 단위 loop와 `has_more`는 public field로 노출하지 않는다. payload parts를 소유하는 언어에서는 복제 가능한 record/value가 아니라 dispose 가능한 envelope로 노출한다 |
| `ActorJoinInfo` + join message | join 요청 판단과 응답에 필요한 `source_actor`, `target_actor`, `source_node_rid`, `source_spot_rid`, `target_node_rid`, `target_spot_rid`, `join_epoch`, `flags`, join message. 언어 관례에 따라 `ActorJoinRequest` wrapper나 tuple/pair로 묶을 수 있다. join message를 소유하는 wrapper는 dispose 가능해야 한다. native reply context는 binding 내부에서만 보관하며 public field로 노출하지 않는다 |
| `ActorJoinResult` | join completion에 전달. `result`, 최종 `actor` ref(remote join이면 target node ref), `joined_spot_rid`, `join_epoch`, `flags` |
| `ActorJoinEntrySpotResult` | Entry Spot join completion에 전달. `result`, 최종 `actor` ref, `target_node_rid`, `join_epoch`, `flags`. join message나 reply payload는 없다 |
| `ActorLookupResult` | remote Actor lookup completion에 전달. `result`, checked `actor` ref, `flags` |
| `SpotActorLifecycleEvent` | Spot lifecycle readable event를 drain한 결과. `kind`, `info`. request parts를 함께 소유하는 언어에서는 복제 가능한 record/value가 아니라 dispose 가능한 envelope로 노출한다 |
| `SpotActorLifecycleInfo` | Spot lifecycle event에 포함된다. `previous_actor`, `current_actor`, `previous_spot_rid`, `current_spot_rid`, `join_epoch`, `flags` |
| `SpotNodeSpotEntry` | Spot routing id, Entry/User Spot kind, dispatch handler 여부, joined/pending Actor 수, route sync 상태, 변경 시각 |
| `SpotNodeActorEntry` | Actor ref, current Spot routing id, current Spot kind, route sync 상태, pending message 수, 변경 시각 |

세부 규칙은 아래와 같다.

- Actor id는 비어 있지 않은 UTF-8 문자열이며 최대 255 bytes다. NUL 문자는
  허용하지 않는다.
- `generation == 0`은 unchecked remote ref이며 유효하지 않은 값으로 보지 않는다.
- local Actor는 `SpotNode`가 만들고, lifecycle handle은 언어별 `Actor` 타입으로
  노출한다. 한 Actor는 동시에 하나의 Spot에만 join할 수 있다.
- `leave`는 async submit API다. unread Actor message를 비우지 않는다. 같은 node의
  Entry Spot으로만 돌아가며, user Spot에서 leave가 성공하면 source left event와
  Entry Spot joined lifecycle event가 발생하고 active route가 Entry Spot
  위치로 갱신된다.
- Entry Spot join은 async submit API다. target 인자는 Entry Spot rid가 아니라
  SpotNode rid다. 한 SpotNode에는 Entry Spot이 하나뿐이므로 별도 Entry Spot rid를
  public API에 요구하지 않는다. Entry Spot join은 join message를 보내지 않고
  application join queue를 거치지 않는다. completion handler는 성공/실패와 최종
  Actor ref만 돌려준다.
- 원격 노드에서 시작해야 하는 Actor는 application이 해당 SpotNode에서 직접
  `actor_new`로 생성한다. remote Actor의 checked ref는 async
  `remote_actor_get_ref` lookup으로 얻는다. remote create-or-get과 admission
  handler는 공개 표면에 없다.
- Spot join request는 message를 포함한다. join reply도 accept/reject 결과와
  함께 message를 caller에게 돌려줘야 한다. join completion은 `ActorJoinResult`
  값으로 caller에게 최종 Actor ref와 joined Spot rid를 전달한다.
- request reply 표면은 core reply 함수가 지원하는 payload part만 노출한다.
  core reply 함수에는 send flag 인자가 없으므로, 바인딩은 reply builder에
  no-op flag 설정 단계를 추가하지 않는다.
- `ActorJoinInfo`가 native `zlink_actor_join_info_t`의 모든 필드를 public
  field로 노출해야 한다는 뜻은 아니다. 언어별 binding은 reply에 필요한 native
  request context를 opaque 내부 상태로 보관한다. public 값 객체에는 사용자가
  판단과 응답에 필요한 `source_actor`, `target_actor`, source/target node와
  Spot routing id, `join_epoch`, `flags`, message를 노출한다.
- 한 STREAM session은 여러 Actor를 bind할 수 있다. bind/unbind는 session
  routing id와 actor id 또는 Actor ref를 기준으로 한다.
- 언어가 session facade를 자연스럽게 제공할 수 있으면 STREAM Actor bind/unbind와
  bound Actor 대상 send는 socket-wide 함수가 아니라 session facade의 동작으로
  노출하는 편이 좋다. 이렇게 하면 session routing id를 반복해서 넘기지 않아도
  된다.
- STREAM에서 Actor로 보내는 public API는 bound session과 actor id를 선택자로
  사용한다.
- Actor 위치는 Actor 생성, Spot join/leave, Actor destroy 흐름에서 갱신된다.
  STREAM session bind/unbind는 Actor 위치를 바꾸지 않는다.
- Actor별 queue limit option은 없다. 바인딩은 이를 public option으로 만들면
  안 된다.
- 제거된 Actor ref 함수, stream actor lookup/send helper, session actor key
  설계 이름은 public surface와 문서에 남기지 않는다.

Actor dispatch event는 SPOT dispatch event handler와 같은 readiness 모델을
사용한다.

- `ZLINK_SPOT_DISPATCH_EVENT_ACTOR_READABLE` 은 Actor part를 읽을 수 있다는
  알림이다. callback 1회가 part 1개를 뜻하지 않는다.
- `ZLINK_SPOT_DISPATCH_SUBJECT_ACTOR` 의 subject는 callback 동안만 유효한
  native Actor ref다. 바인딩 public API는 raw pointer를 노출하지 않는다.
- callback을 다른 실행 컨텍스트로 넘기는 언어는 callback 진입 시점에 Actor
  part를 nonblocking으로 미리 drain해서 public dispatch info가 그 part를
  반환하게 해야 한다.
- `ZLINK_SPOT_DISPATCH_EVENT_ACTOR_JOIN_READABLE` 은 Spot의 Actor join request
plane readiness다. 바인딩은 `Spot.recvActorJoin` 또는 동등한 public 표면으로
  언어별 no-data 표현이 나올 때까지 drain할 수 있게 해야 한다.

### SpotNode Capability Matrix

| Capability | SpotNode |
|---|---|
| `bind` | Y |
| `connectPeer` | Raw mesh only |
| `disconnectPeer` | Raw mesh only |
| `disconnectPeerRid` | Raw mesh only |
| `createSpot` | Y |
| `entrySpot` | Y |
| `spotLookup` | Y |
| `setTlsServer` | Y |
| `setTlsClient` | Y |
| `status` | Y |
| `peers` | Y |
| `peers(filter)` | Y |
| `subjects` | Y |
| `internalSockets` | Diagnostic |
| `spots` | Y |
| `actors` | Y |
| `close` | Y |

- SpotNode는 data plane API(`send`/`recv`/`publish`/`subscribe`)를 직접
  노출하지 않는다.
- data plane은 `Spot` facade를 통해서만 접근한다.
- `connectPeer`/`disconnectPeer`는 raw peer topology 전용 control path 다.
- `createSpot` 은 `zlink_spot_new()` 위에 놓는 public factory다.
  `entrySpot` 은 `zlink_spot_node_entry_spot()` 을 언어별 typed `Spot`
  factory로 감싼다. `spotLookup` 은 `zlink_spot_node_spot_lookup()` 을
  언어별 typed `Spot` 조회 표면으로 감싼다.
  `createPublisher`를 중심으로 다룬다.

### Actor Capability Matrix

Actor dispatch는 `SpotNode`, `Actor`, `Spot`, `StreamSocket`에 걸친 독립
service layer 기능이다. 각 바인딩은 아래 역할을 언어별 관례에 맞는 public
surface로 노출해야 한다.

| Capability | Public owner | Core substrate |
|---|---|---|
| local Actor create | `SpotNode` | `zlink_spot_node_actor_new` |
| local Actor lookup | `SpotNode` | `zlink_spot_node_actor_lookup` |
| unchecked remote Actor ref | `SpotNode` | `zlink_remote_actor_get_ref` |
| Actor destroy by ref | `SpotNode` | `zlink_spot_node_actor_destroy` |
| owned Actor close/destroy | `Actor` | `zlink_spot_node_actor_destroy` |
| Spot Actor lifecycle receive | `Spot` | `zlink_spot_recv_actor_lifecycle` |
| Actor join by ref | `SpotNode` | `zlink_spot_node_actor_join_spot` |
| Actor Entry Spot join by ref | `SpotNode` | `zlink_spot_node_actor_join_entry_spot` |
| owned Actor join | `Actor` | `zlink_spot_node_actor_join_spot` |
| owned Actor Entry Spot join | `Actor` | `zlink_spot_node_actor_join_entry_spot` |
| Actor leave by ref | `SpotNode` | `zlink_spot_node_actor_leave_spot` |
| owned Actor leave | `Actor` | `zlink_spot_node_actor_leave_spot` |
| Actor recv | `Actor` | `zlink_spot_node_actor_recv_part` |
| no-bind request reply | `SpotNode` | `zlink_spot_node_actor_reply_no_bind` |
| bound session send | `Actor` | `zlink_spot_node_actor_send_bound_session_msg` |
| bound session close | `Actor` | `zlink_spot_node_actor_close_bound_session` |
| join request recv | `Spot` | `zlink_spot_actor_join_recv` |
| join request reply | `Spot` | `zlink_spot_actor_join_reply` |
| STREAM bind Actor | `StreamSocket` / session facade | `zlink_stream_bind_actor` |
| STREAM unbind Actor | `StreamSocket` / session facade | `zlink_stream_unbind_actor` |
| STREAM send bound Actor | `StreamSocket` / session facade | `zlink_stream_send_bound_actor_part` |
| STREAM bound Actor snapshot | `StreamSocket` / session facade | `zlink_stream_bound_actors` |
| node Spot snapshot | `SpotNode` | `zlink_spot_node_spots` |
| node Actor snapshot | `SpotNode` | `zlink_spot_node_actors` |
| Spot joined Actor snapshot | `Spot` | `zlink_spot_actors` |

### Spot Capability Matrix

| Capability | Spot |
|---|---|
| `publish(topic, ...)` | Y |
| `subscribe` | Y |
| `receiveSubscriptionEvent` | Y |
| `setSubscription` / `unsetSubscription` | Y |
| `sendToChannel` / `requestToChannel` | Y |
| `sendToSpot` | Routed ordinary send (spot → spot) |
| `requestToSpot` | Routed request initiation (spot → spot) |
| `requestToRouter` | Routed request initiation (spot → router) |
| `replyToSpot` | Routed reply surface (spot → spot) |
| `replyToRouter` | Routed reply surface (spot → router) |
| `setDispatchHandler` | Y |
| `setSendReadyHandler` | Y |
| `recvActorLifecycle` | Y |
| `close` | Y |

- Spot은 소켓 타입이 아니라 SpotNode 위에 올라가는 channel-aware facade다.
- Spot routed receive 는 `recv_routed` 또는 동등한 typed recv surface 로
  노출할 수 있다.
- Spot은 `bind`/`connect`를 갖지 않는다 (SpotNode가 담당).
- Spot `close`는 facade만 해제하고 SpotNode는 살아 있다.

### 제거된 Discovery / Registry capability

공개 Discovery와 Registry C API는 core 8.4.3에서 core 계약에서 제거되었다.
바인딩은 Discovery/Registry factory, resolver method, sync option, registry
query client, compatibility alias를 현재 API로 노출하면 안 된다.

### 서비스 관찰성 정책
- 공개 서비스 계층 관찰은 별도 monitor handle 대신 snapshot/query surface로 한다.
- SPOT(SpotNode, Spot) 관찰은 `status`, `peers`,
  `peers(filter)`, `subjects`, `spots`, `actors` API를
  사용한다. 내부 socket 진단이 필요한 바인딩은 `internalSockets`을
  별도 diagnostic 표면으로 둔다.
- 상태 전이가 필요하면 연속된 snapshot/query 결과를 비교한다.
- SocketMonitor callback 해제 정책은 기존과 같다.
  - callback 등록 API가 있는 경우 `close()`로만 해제한다

### 서비스 계층 도메인 객체
- 서비스 계층도 domain object를 사용해야 한다.
- 최소 핵심 domain object:
  - `MonitorStatus`: monitor 상태 스냅샷
  - `SpotNodeStatus`: SpotNode 상태 (state, peer count 등)
- Advanced / Diagnostic domain object:
  - `SpotNodePeerEntry`: peer 정보
  - `SpotNodeSubjectEntry`: subject 정보
  - `SpotNodeSocketEntry`: 내부 socket 진단 정보. socket 종류는 공통
    `SocketType` enum을 사용하며, 같은 값을 반복하는 별도 SpotNode 전용
    socket type enum을 만들지 않는다.
  - `SpotNodeSpotEntry`: node 소유 Spot 정보
  - `SpotNodeActorEntry`: node 소유 Actor route 정보
- 필터 객체:
  - `SpotNodePeerFilter`: peer 조회 필터
  - `SpotNodeSubjectFilter`: subject 조회 필터
  - `SpotNodeSocketFilter`: 내부 socket 진단 필터
- enum/value object:
  - `SocketType`: 일반 socket과 SpotNode 내부 socket 진단에서 함께 쓰는
    socket 종류
  - `SpotRole`: `PUB`, `SUB`
  - `SubjectKind`: `NONE`, `TOPIC`, `PATTERN`
  - `SpotNodeState`: `IDLE`, `CONNECTING`, `PARTIAL_READY`, `READY`, `ERROR`
  - `MonitorSourceKind`: `SOCKET`, `SPOT_PUB`, `SPOT_SUB`
  - `SpotPeerSource`: `MANUAL`, `DISCOVERY`, `MIXED`
  - `SpotPeerState`: `CONFIGURED`, `CONNECTING`, `CONNECTED`
- `MonitorStatus.isReady()` 또는 동등한 편의 accessor는 raw socket
  monitor source에서만 ready 의미를 해석한다. `SPOT_PUB`, `SPOT_SUB`
  source에서는 ready bit를 SPOT readiness로 확장 해석하면 안 된다.

### 서비스 계층 네이밍 정책
- 서비스 계층도 Naming Policy를 따른다.
- 허용되는 변형은 Naming Policy의 세 가지 변형과 같다. 즉 케이싱 변형,
  overload 불가 언어의 최소 접미사, 언어별 property/getter 관례만 허용한다.
- 단어 교체, 생략, 대체는 금지한다.
- 규칙 상세는 Naming Policy 본문과 동일하다.

#### 서비스 계층 Canonical Name 표

| Component | Canonical Name | 설명 |
|---|---|---|
| SpotNode | `bind` | endpoint 바인드 |
| SpotNode | `connectPeer` | raw peer 연결 |
| SpotNode | `disconnectPeer` | raw peer 연결 해제 |
| SpotNode | `createRouteBridge` | caller/channel runtime 소유 socket을 SPOT route bridge에 등록 |
| SpotNode | `createPublisher` | SpotNode의 topic publish ingress에 쓰는 publisher handle 생성 |
| SpotNode | `setTlsServer` | TLS 서버 설정 |
| SpotNode | `setTlsClient` | TLS 클라이언트 설정 |
| SpotNode | `status` | 노드 상태 스냅샷 |
| SpotNode | `peers` | peer 목록 스냅샷 |
| SpotNode | `peers(filter)` | peer 필터 조회 |
| SpotNode | `subjects` | subject 목록 스냅샷 |
| SpotNode | `internalSockets` | 내부 socket 진단 스냅샷 |
| SpotNode | `spots` | node 소유 Spot 스냅샷 |
| SpotNode | `actors` | node 소유 Actor 스냅샷 |
| SpotNode | `close` | 노드 종료 |
| Spot | `publish(topic, ...)` | Spot topic 발행 |
| Spot | `subscribe` | 토픽 구독 수신 |
| Spot | `receiveSubscriptionEvent` | topic 구독 이벤트 수신 |
| Spot | `setSubscription` / `unsetSubscription` | 구독 필터 관리 |
| Spot | `sendToChannel` / `requestToChannel` | channel 단위 routed 송신 / 요청 |
| Spot | `setDispatchHandler` | topic/routed/channel reply/timer readable 알림 handler 등록 |
| Spot | `setSendReadyHandler` | send ready callback handler 등록 |
| Spot | `recvActorLifecycle` | Actor join/leave lifecycle event 수신 |
| Spot | `close` | facade 종료 |

### 서비스 계층 테스트 정책
- 서비스 계층은 sample이나 perf에서 직접 검증되지 않는 컴포넌트를 포함하므로
  FFI 매핑, lifecycle, 타입 변환이 올바른지 테스트해야 한다.
- 서비스 계층도 Test Matrix와 동일한 카테고리로 테스트한다.

#### 서비스 계층 Surface 테스트
- SpotNode 역할 matrix 정렬 확인
- Spot 역할 matrix 정렬 확인
- service TLS helper 존재 확인
- typed domain object 존재 확인 (SpotNodeStatus, SpotNodePeerEntry,
  SpotNodeSocketEntry, SpotNodeSpotEntry, SpotNodeActorEntry 등)
- typed enum 존재 확인 (SpotRole, SubjectKind, SpotNodeState 등)

#### 서비스 계층 Contract 테스트
- SpotNode: create/bind/close lifecycle 누수 없음
- Spot: create/close lifecycle (SpotNode는 살아 있어야 함)
- 예외/오류 경로에서도 native 리소스가 정리되는지 확인

#### 서비스 계층 Behavior 테스트
- SpotNode bind → Spot publish → Spot subscribe 경로 성공
- Spot subscribe → 데이터 없음 시 empty 반환 (non-blocking)
- Spot publish 실패 시 예외 확인
- Spot dispatch event callback 호출 확인
- Spot setSendReadyHandler callback 호출 확인
- Spot receiveSubscriptionEvent 경로 확인
- SpotRouteBridge attach/send/request/handleReceived 경로 동작 확인
- SpotNode publisher handle publish 경로 동작 확인

#### 서비스 계층 Introspection 테스트
- SpotNode status → SpotNodeStatus 필드 검증
  (state, peerCount, subjectCount 등)
- SpotNode peers → SpotNodePeerEntry 목록 검증
- SpotNode peers(filter) → 필터 적용 결과 검증
- SpotNode subjects → SpotNodeSubjectEntry 목록 검증

#### 서비스 계층 테스트 범위

| Test Category | SpotNode+Spot | Actor | Stream Actor Binding |
|---|---|---|---|
| Surface | Required | Required | Required |
| Contract | Required | Required | Required |
| Behavior | Required | Required | Required |
| Introspection | Required | Required | Required |

- service/spot 계열이 없는 바인딩은 이 테스트를 제외할 수 있다.
- 여기서 monitor 설명은 socket monitor 기준이다.

### 서비스 계층 샘플 정책
- Canonical Sample Set에 정의된 서비스 계열 샘플:
  - `spot_recv_sample`: Spot channel-aware subscribe / routed recv
  - `spot_callback_sample`: Spot dispatch event callback
  - `monitor_recv_sample`: monitor event 수신 (socket monitor 포함)
- service/spot 계열이 없는 바인딩은 `spot_*` 샘플을 제외할 수 있다.

### 바인딩별 서비스 계층 범위
- 모든 바인딩이 서비스 계층 전체를 구현해야 하는 것은 아니다.
- 최소 요구 사항:

| Component | 요구 수준 |
|---|---|
| SpotNode + Spot | 해당 바인딩에 spot 지원이 있으면 Required |

### Callback API 정책
- callback 등록 API는 각 소켓 타입의 역할에 따라 노출한다.
- 위 Callback Capabilities 표가 기준이다.
- canonical handler 등록 이름:
  - `setDispatchHandler`: SPOT unified readable notification callback 등록
  - `setSendReadyHandler`: send ready 상태 callback 등록
- SPOT routed receive와 Actor lifecycle은 direct callback 등록 API를 노출하지 않는다.
  `setDispatchHandler`가 readable event를 알리고, 사용자는 `recvRouted` 또는
  `recvActorLifecycle`로 queue를 명시적으로 drain한다.
- `onReceive` 는 raw `STREAM` direct fragment callback 의 내부 이름으로만
  사용할 수 있다. canonical public binding API 이름으로 쓰지 않는다.
- callback을 `null`/`None`으로 설정하여 해제하는 것은 허용하지 않는다.
  callback 해제는 socket close로만 이루어진다.

## 코어 API 추가 사항

이 섹션은 `core/include/zlink.h`에 추가된 core API를 정리한다.
각 바인딩은 이 API를 언어별 typed surface로 노출해야 한다.

### Request-Reply 정책

> 언어별 인터페이스 시그니처와 사용 예는
> `cpp/`, `java/`, `dotnet/`, `node/`, `python/`, `go/`, `rust/` 를 참조한다.

#### 설계 원칙

- request-reply 는 ZMP protocol envelope 로 처리한다.
  `zlink_msg_t` 에 request 표시를 붙이는 방식은 사용하지 않는다.
- dispatch, pending map, timeout, reply 매칭은 core C API 에서 처리한다.
  바인딩은 이 로직을 다시 구현하지 않는다.
- core 는 callback 기반 비동기 모델을 제공한다.
  바인딩은 [바인딩 비동기 실행 표면 정책](async-coroutine-policy.md)에 따라
  callback 위에 언어별 완료 객체 반환 표면을 얹을 수 있다. coroutine 연결은 framework가
  맡는다.
- `request()` 는 thread blocking API 가 아니다.
- request-reply 는 Router/Dealer 소켓과 SPOT 의 기능 확장이다.
  별도 추상 레이어가 아니라 기존 표면에 역할을 얹는다.

#### 공개 표면에 두지 않는 API

message-level request-reply marker API 와 per-message metadata API 는
public surface 의 일부가 아니다. 바인딩은 다음 함수나 상수를 public 으로
노출하지 않고, `Message` 객체 안에 request marker 상태를 두지 않는다.

- `zlink_msg_set_request`, `zlink_msg_set_reply`, `zlink_msg_get_request_info`
- `zlink_msg_set_metadata`, `zlink_msg_get_metadata`, `zlink_msg_clear_metadata`

#### 유효한 Request-Reply 조합

**Socket 경로:**

| 요청자 | 응답자 | 가능 | reply 경로 |
|--------|--------|------|-----------|
| Dealer | Router | Y | Router 가 Dealer 의 routing_id 로 회신 |
| Router | Router | Y | 서로 routing_id 로 회신 |
| Dealer | Dealer | **N** | 양쪽 다 routing_id 없음 |
| Router | Dealer | **N** | Dealer 가 특정 peer 에 회신 불가 |

**SPOT 경로:**

| 요청자 | 응답자 | 가능 | reply 경로 |
|--------|--------|------|-----------|
| Spot | Spot | Y | 상대 주소 + request_seq 로 회신 |
| Spot | Router | Y | Spot 이 Router 에 request, Router 가 Spot 에 reply |
| Router | Spot | Y | Router 가 Spot 에 request, Spot 이 Router 에 reply |

`DealerSocket.request()` 연결 제약:
- 연결 대상은 전부 Router 여야 한다.
  Dealer 에 Router 와 Dealer 가 섞이면 request 가 실패할 수 있다.
- 바인딩은 이 제약을 런타임에 검증하지 않는다. 사용자 책임이며 API 문서에 명시한다.

#### C API 표면

**공통 타입:**

```c
typedef void (*zlink_reply_handler_fn)(
    zlink_request_result_t result_,
    zlink_msg_t *parts,
    size_t part_count,
    void *userdata);

```

callback 으로 전달된 `parts` 는 borrowed view 다.
callback 반환 시점까지만 유효하다. 밖에서 유지하려면 복사한다.

**Socket API:**

```c
zlink_submit_result_t zlink_dealer_request_part(void *dealer,
    zlink_msg_t *part, zlink_send_flags_t flags,
    zlink_part_flag_t part_flag, uint32_t timeout_ms,
    zlink_reply_handler_fn handler, void *userdata);

zlink_submit_result_t zlink_dealer_reply_part(void *dealer,
    uint64_t request_seq, zlink_msg_t *part,
    zlink_part_flag_t part_flag);

zlink_submit_result_t zlink_router_request_part(void *router,
    const zlink_routing_id_t *peer_rid, zlink_msg_t *part,
    zlink_send_flags_t flags, zlink_part_flag_t part_flag,
    uint32_t timeout_ms, zlink_reply_handler_fn handler,
    void *userdata);

zlink_submit_result_t zlink_router_reply_part(void *router,
    const zlink_routing_id_t *peer_rid, uint64_t request_seq,
    zlink_msg_t *part, zlink_part_flag_t part_flag);

zlink_recv_result_t zlink_router_recv_part(void *router,
    const zlink_routing_id_t **source_node_rid_out,
    const zlink_routing_id_t **source_spot_rid_out,
    uint64_t *request_seq_out, zlink_msg_t *part_out,
    zlink_part_flag_t *has_more_out, zlink_recv_flags_t flags);
```

**SPOT API:**

```c
zlink_submit_result_t zlink_spot_send_channel_part(void *spot, ...);
zlink_submit_result_t zlink_spot_request_channel_part(void *spot, ...);
zlink_submit_result_t zlink_spot_send_spot_part(void *spot, ...);
zlink_submit_result_t zlink_spot_request_spot_part(void *spot, ...);
zlink_submit_result_t zlink_spot_request_router_part(void *spot, ...);
zlink_submit_result_t zlink_spot_reply_spot_part(void *spot, ...);
zlink_submit_result_t zlink_spot_reply_router_part(void *spot, ...);
zlink_submit_result_t zlink_router_request_spot_part(void *router, ...);
zlink_submit_result_t zlink_router_reply_spot_part(void *router, ...);
zlink_submit_result_t zlink_router_send_spot_part(void *router, ...);
zlink_submit_result_t zlink_spot_publish_part(void *spot, ...);
zlink_recv_result_t zlink_spot_subscribe_part(void *spot, ...);
zlink_recv_result_t zlink_spot_recv_part(void *spot, ...);
zlink_handler_result_t zlink_spot_dispatch_event_handler(void *spot, ...);
```

전체 시그니처는 `core/include/zlink.h` 를 참조한다.

#### 수신 Dispatch 모델

core 가 request-reply dispatch 를 처리한다. 바인딩은 dispatch owner 를 구현하지 않는다.

- `request_seq = 0` 이면 ordinary message.
- `request_seq != 0` 이면 request-reply message.
- core 가 pending map 에서 `source_node_rid + request_seq` 로 매칭한다.
- 매칭 실패한 reply (stray/late reply) 는 drop 한다.
- ROUTER 는 generic `zlink_recv_part()` 대신 `zlink_router_recv_part()` typed surface 를
  사용한다. generic `zlink_recv_part()` 호출 시 `EOPNOTSUPP`.
- ROUTER 의 routed 수신 plane 은 **단일 표면**이다. 일반 ROUTER 트래픽과
  spot-origin routed 트래픽 모두 `zlink_router_recv_part()` 하나로 받는다.
  `source_spot_rid` 가 `NULL` 이면 일반 ROUTER 트래픽, 채워져 있으면
  spot-origin 트래픽이다.

#### Request API 변형

request 는 두 완료 방식을 가진다.

비동기 request와 callback completion request는 모두 `request`
entrypoint가 반환하는 `RequestOp` operation builder를 통해 노출된다. 완료 방식별
flags, timeout, 실패 전달 규칙은 [바인딩 비동기 실행 표면 정책](async-coroutine-policy.md)을
따른다.

C binding 은 `zlink_*_request_part(..., flags, part_flag, timeout, ...)`
substrate 형태를 유지한다. C ABI에는 wrapper builder 정책을 적용하지 않는다.

- 에러 처리는 Error Handling Policy 를 따른다.
  callback request 의 submit 실패도 언어 관용구를 그대로 적용한다:
  exception 언어 (C++/Java/.NET/Node/Python) 는 예외, return-based 언어
  (C/Go/Rust) 는 에러 반환.
- reply 결과는 callback 이 정확히 한 번 전달한다.
  `(RequestResult result, List<Message> parts)`

#### SPOT Request-Reply

SPOT 직접 전달 위에서도 같은 request-reply 프로토콜을 사용한다.
`SPOT routed envelope -> request-reply envelope -> payload` 순서로 싣는다.
SPOT reply 도 ctx 없이 상대 주소 + request_seq 로 보낸다.
같은 Spot 에서 여러 request 를 동시에 outstanding 상태로 둘 수 있다.
high-level request 완료는 첫 reply 1건으로 끝난다.

#### Timeout

- timeout 은 core 가 관리한다. 바인딩은 timeout 로직을 구현하지 않는다.
- 기본 timeout: `5000ms`. per-call > socket default > 구현 기본 `5000ms`.
- `timeout_ms = 0` 이면 socket default timeout 을 사용한다.
- timeout 은 send 대기 + reply 대기를 합산한 전체 경과 시간에 적용된다.
- timeout 시 core 가 pending map 에서 제거하고 callback 에 `ZLINK_REQUEST_TIMED_OUT` 전달.
- timeout 후 late reply 는 core 가 drop 한다.

#### Pending map

- `request_seq` 채번, pending 등록, reply 매칭, timeout 제거 모두 core 에서 한다.
- 바인딩은 pending map 을 별도로 유지하지 않는다.
- 바인딩이 유지하는 것은 callback → Future/Promise resolve 매핑뿐이다.

#### Wire format

- `request_seq` 는 부호 없는 64비트 정수 (8바이트, network byte order).
- 시작값 `1`. `0` 은 ordinary message 예약값.
- overflow 시 `1` 로 wrap. outstanding 충돌값은 건너뛴다.
- envelope 은 4개 control part: protocol id, version, message type, request_seq.
- SPOT routed 조합 시 8개 SPOT control part + 4개 request-reply control part + payload.
- 바인딩은 envelope 을 직접 파싱하지 않는다. core 가 처리한다.

#### 반환 타입

- `request()` 성공 시 **reply payload `List<Message>` 만** 반환한다
  (`Vec<Message>` / `IReadOnlyList<Message>` / `Message[]` /
  `tuple[Message, ...]` 등 언어별 리스트 타입).
- caller 는 이미 자기가 보낸 request 의 대상 routing_id 와 request_seq 를
  알고 있으므로, 그걸 wrap 한 `Received` 를 되돌려받을 필요가 없다.
- 별도 `Reply` 타입은 만들지 않는다.
- multipart reply 지원이 목적이므로 단일 `Message` 가 아닌 리스트 형태다.
  단일 part reply 는 `parts[0]` 으로 꺼낸다.
- request handler (서버 측) 는 `peer_rid`, `request_seq`, payload 를 함께
  전달한다. 별도 `Request` 타입이나 `onRequest` 전용 callback 은 만들지
  않는다. (server 측은 누가 어떤 request_seq 로 보냈는지 알아야 하므로
  차이가 있다.)

#### 소유권

- `request()` / `reply()` 호출 시 메시지 ownership 은 기존 send 계약을 따른다.
- request callback 으로 전달된 `parts` 는 borrowed view 다.
  callback 반환 후 무효. 바인딩은 이를 복사해 언어별 리스트 타입 또는
  `Vec<Message>` 로 전달한다.
- 소켓 close 시 core 가 pending map 의 모든 미완료 request 를 `ZLINK_REQUEST_TERMINATED` callback 으로 reject 한다.

#### Callback 계약

- callback 은 정확히 한 번 호출된다.
  성공이면 `result = OK` + reply parts, 실패면 `result != OK` +
  empty/null/Err 경로로 전달된다.
- core callback 시그니처: `void(zlink_request_result_t result_, zlink_msg_t *parts_, size_t part_count_, void *userdata_)`
- 언어별 패턴 (per-function `RequestError` 계승):
  - C++: `std::function<void(request_result_t, std::vector<message_t>)>`
  - Java: `BiConsumer<RequestResult, List<Message>>`
  - .NET: `Action<RequestResult, IReadOnlyList<Message>>`
  - Node: `(result: RequestResult, parts: Message[]) => void`
  - Python: `callback(result: RequestResult, parts: list[Message])`
  - Go: `func(RequestResult, []*Message)` (실패 시 nil/empty 허용)
  - Rust: `FnOnce(Result<Vec<Message>, RequestError>)` (Rust 관용구;
    `RequestError::code` 가 `RequestResult` 에 대응)

### SPOT Messaging 정책

> 언어별 SPOT 인터페이스는 `cpp/`, `java/`, `dotnet/`, `node/`, `python/`, `go/`, `rust/` 를 참조한다.

SPOT public surface 는 두 이름 축을 분리한다. `sendToChannel(...)` 과
`requestToChannel(...)` 은 channel-aware 직접 메시징 경로이고,
`publish(topic, ...)` 는 `Spot` 자신이 속한 topic plane 에 발행한다.
직접 주소 지정 routed messaging 은 선택적으로 추가할 수 있는 보조 typed surface 다. request-reply 는
routed messaging 위에 얹어진다.

#### Pub/Sub 메시징

SPOT pub/sub 는 `Spot` handle 이 속한 channel 과 `topic` 기반 발행/구독 모델이다.
발행 호출자는 channel 이름을 별도 인자로 전달하지 않는다.

```c
/* publish */
zlink_submit_result_t zlink_spot_publish_part(void *spot,
    const char *topic_id, zlink_msg_t *part, zlink_send_flags_t flags,
    zlink_part_flag_t part_flag);

/* subscribe receive */
zlink_recv_result_t zlink_spot_subscribe_part(void *spot,
    const zlink_routing_id_t **source_rid_out,
    char *topic_id_buf, size_t topic_id_capacity,
    size_t *topic_id_len_out, zlink_msg_t *part_out,
    zlink_part_flag_t *has_more_out, zlink_recv_flags_t flags);

/* subscription filter */
zlink_config_result_t zlink_set_subscription(
    void *handle,
    const char *filter);
zlink_config_result_t zlink_unset_subscription(
    void *handle,
    const char *filter);
```

바인딩 규칙:
- C API 는 publish 를 위한 별도 no-wait 함수 이름을 따로 두지 않는다.
- non-blocking publish 는 `zlink_spot_publish_part(..., ZLINK_DONTWAIT, ...)` 를 호출하고
  errno 를 `zlink_submit_result_t` 로 분류한다. 바인딩은 별도 `tryPublish` 나
  `publishNoWait` 를 두지 않는다.
- `subscribe` 수신은 `topic + parts` 를 돌려주는 typed receive surface 로
  노출한다.
- topic filter 설정은 typed subscription API 로 노출한다.
- channel-aware send/request 와 topic publish 의 실패는 `SubmitError` 로 승격된다.
  - `NOT_FOUND`: channel-aware send/request 는 해당 `channel_name` 또는 attach 대상이 없음.
    topic publish 는 발행 가능한 topic plane 대상이 없음.
  - `NOT_CONNECTED`: attachment 는 있으나 active/send-ready 경로가 없음
  - `BACKPRESSURED`: 경로는 있으나 HWM 도달
  - `NOT_ADMITTED`: 대상 peer 가 drain 상태라 신규 submit 거부

#### Routed Direct Messaging

SPOT routed direct messaging 은 특정 Spot 또는 Router peer, routed reply 대상에
직접 메시지를 보낸다. Core substrate는 아래 part 기반 C 함수로 표현된다.
고수준 바인딩의 `Spot` facade와 `RouterSocket`의 router-to-spot helper 모두
이 기능을 `Operation Builder Policy`에 맞춘 operation builder 시작점으로
노출한다. raw socket의 일반 send/request/reply도 동일한 builder 패턴을 따른다.

```c
/* spot -> spot */
zlink_submit_result_t zlink_spot_send_spot_part(void *spot,
    const zlink_routing_id_t *dest_node_rid,
    const zlink_routing_id_t *dest_spot_rid,
    zlink_msg_t *part, zlink_send_flags_t flags,
    zlink_part_flag_t part_flag);

/* router -> spot */
zlink_submit_result_t zlink_router_send_spot_part(void *router,
    const zlink_routing_id_t *dest_node_rid,
    const zlink_routing_id_t *dest_spot_rid,
    zlink_msg_t *part, zlink_send_flags_t flags,
    zlink_part_flag_t part_flag);
```

바인딩 규칙:
- C ABI는 part 기반 함수형 계약을 유지한다.
- 고수준 바인딩의 `Spot` endpoint, `RouterSocket`의 router-to-spot helper,
  그리고 raw `DealerSocket`/`RouterSocket`/`PubSocket`/`StreamSocket` 등의
  일반 send/request/reply/publish 표면 모두 이 문서의
  `Operation Builder Policy`를 따른다.
- 목적지 주소·요청 시퀀스는 builder 시작점 인자로 받고, payload·flags·timeout·
  callback은 builder 단계로 표현한다.
- routed recv 는 아래 Event Dispatcher 의 handler/recv surface 를 사용한다.

#### SPOT Lifecycle / Bridge / Deprecated Attachment

```c
void *zlink_spot_new(void *node);          /* create SPOT facade */
zlink_close_result_t zlink_spot_destroy(void **spot_p);

void *zlink_spot_node_new(
    void *ctx,
    const zlink_spot_node_options_t *options);
zlink_close_result_t zlink_spot_node_destroy(void **node_p);
zlink_bind_result_t zlink_spot_node_bind(void *node, const char *endpoint);
zlink_connect_result_t zlink_spot_node_connect_peer(void *node,
    const char *peer_endpoint);
zlink_connect_result_t zlink_spot_node_disconnect_peer(void *node,
    const char *peer_endpoint);
zlink_connect_result_t zlink_spot_node_disconnect_peer_rid(void *node,
    const zlink_routing_id_t *peer_rid);

void *zlink_spot_route_bridge_new(
    void *ctx,
    void *spot_node,
    const zlink_spot_route_bridge_options_t *options);
int zlink_spot_route_bridge_attach_router_channel(
    void *bridge,
    const char *channel_name,
    void *router,
    const zlink_spot_route_bridge_endpoint_options_t *options);
int zlink_spot_route_bridge_send(
    void *bridge,
    const char *channel_name,
    const zlink_routing_id_t *target_node_rid,
    const zlink_routing_id_t *target_spot_rid,
    zlink_msg_t *parts,
    size_t part_count,
    zlink_send_flags_t flags);
int zlink_spot_route_bridge_request(
    void *bridge,
    const char *channel_name,
    const zlink_routing_id_t *target_node_rid,
    const zlink_routing_id_t *target_spot_rid,
    zlink_msg_t *parts,
    size_t part_count,
    zlink_reply_handler_fn reply_handler,
    void *userdata,
    zlink_send_flags_t flags,
    uint32_t timeout_ms);
int zlink_spot_route_bridge_handle_router_received(
    void *bridge,
    const char *channel_name,
    const zlink_routing_id_t *source_node_rid,
    uint64_t request_seq,
    zlink_msg_t *parts,
    size_t part_count,
    bool *handled_out);
int zlink_spot_route_bridge_drain(void *bridge);
int zlink_spot_route_bridge_close(void *bridge);

void *zlink_spot_node_publisher_new(void *node);
int zlink_spot_node_publisher_publish(
    void *publisher,
    const char *topic,
    zlink_msg_t *parts,
    size_t part_count,
    zlink_send_flags_t flags);
int zlink_spot_node_publisher_close(void *publisher);
```

`options == NULL` 또는 `options->mode == 0`은 모든 SPOT 기능을 켠다. 바인딩은
각 언어의 기본 생성자에서 이 기본값을 사용하고, mode를 노출하는 경우
`PUBSUB`, `ROUTED`, `ALL` 값을 C 계약과 같은 의미로 매핑한다. 내부 socket
관찰 API는 `zlink_spot_node_internal_sockets()`을 기준으로 하며,
이미 생성된 socket만 반환한다.

SpotNode option facade는 core의 여섯 public option을 빠뜨리지 않아야 한다.

| Core option | Binding surface |
|-------------|-----------------|
| `ZLINK_SPOT_NODE_OPT_ROUTER_HWM_PROFILE` | router admission HWM profile |
| `ZLINK_SPOT_NODE_OPT_ROUTER_HWM` | router admission HWM override |
| `ZLINK_SPOT_NODE_OPT_PUBSUB_HWM_PROFILE` | pub/sub admission HWM profile |
| `ZLINK_SPOT_NODE_OPT_PUBSUB_HWM` | pub/sub admission HWM override |
| `ZLINK_SPOT_NODE_OPT_DISPATCH_WORKERS_MIN` | minimum dispatch callback workers |
| `ZLINK_SPOT_NODE_OPT_DISPATCH_WORKERS_MAX` | maximum dispatch callback workers |

dispatch worker min/max는 `SpotNode` dispatch callback 실행 pool 설정이다.
data-plane thread나 transport I/O thread 수를 바꾸는 옵션으로 설명하면 안 된다.
값 검증은 core와 동일하게 `min >= 1`, `max >= min`이다. 바인딩은 각 언어의
typed option/property로 이 두 값을 노출하고, raw option bag을 canonical 경로로
되살리면 안 된다.

바인딩 규칙:
- `SpotNode` 와 `Spot` 은 별도 typed handle 로 노출한다.
- `Spot` 은 `SpotNode` 위에 올라가는 facade 다. `SpotNode` 해제 시 `Spot` 도 무효가 된다.
- Spot에서 다른 channel로 보내거나 `ROUTER` channel에서 Spot relay packet을 받을 때는
  `SpotRouteBridge`를 사용한다. bridge에 등록되는 `ROUTER` socket은 caller 또는
  channel runtime이 계속 소유한다.
- raw Core socket에는 logical channel metadata를 설정하거나 조회하는 API가
  없다. channel name은 `SpotRouteBridge`의 typed operation이 받는 논리적
  routing 값으로만 사용한다.
- bridge의 `handle_router_received()`는 channel runtime의 receive loop에서 호출한다.
  `handled == true`이면 payload 소유권은 bridge가 가져가며, caller는 같은 received
  object를 다시 처리하지 않는다.
- `SpotNodePublisher`는 외부 코드가 raw `PUB` socket을 `SpotNode`에 attach하지 않고
  SpotNode의 topic publish ingress로 publish하기 위한 handle이다.
- `Spot.publish(topic).message(...).submit()`은 `SpotNode` 자신의 topic publish
  ingress queue로 들어가는 channel-aware topic plane이다. 외부 channel 호출은
  `SpotRouteBridge`와 channel runtime 소유 socket 경로로 설명한다.
- `connect_peer` / `disconnect_peer` 는 raw peer topology 전용 control
  path 다. channel-aware public surface 의 중심 API 로 설명하면 안 된다.

### SPOT Event Dispatcher 정책

core 는 callback 기반 event dispatcher 모델을 제공한다.
하나의 I/O thread context 안에서 여러 이벤트 소스
(sub recv, routed recv, timer, send-ready) 를 동기화 없이 처리할 수 있다.

핵심 원리:
- handler callback 을 등록하면 core I/O thread 가 이벤트 발생 시 callback 을 호출한다.
- 모든 callback 은 같은 thread context 에서 실행되므로 lock 없이 상태를 공유할 수 있다.
- callback 안에서 recv, send, reply 를 호출해도 동기화 문제가 없다.
- timer 도 같은 context 에서 실행된다.

#### Callback 등록 API

```c
/* raw STREAM direct recv callback */
zlink_handler_result_t zlink_recv_handler(void *s,
    zlink_socket_msg_handler_fn handler, void *userdata);

/* raw STREAM packet callback */
zlink_handler_result_t zlink_stream_packet_handler(void *stream,
    zlink_stream_packet_handler_fn handler, void *userdata);

/* register writable notification callback */
zlink_handler_result_t zlink_send_ready_handler(void *s,
    zlink_send_ready_handler_fn handler, void *userdata);
```

규칙:
- core C attach 함수는 한 subject 당 활성 handler 하나만 허용한다.
  이미 native handler가 attach된 상태에서 다시 attach하면 `EBUSY`가 날 수 있다.
  public binding의 `set...Handler` 표면은 이 raw attach 함수를 직접 반복 노출하지
  않고, 현재 public handler를 저장하거나 교체하는 의미로 제공한다.
- `zlink_recv_handler()` 는 raw `STREAM` 에만 허용한다.
- `zlink_stream_packet_handler()` 도 raw `STREAM` 에만 허용하며,
  `recv` / raw callback / packet callback 세 모드는 서로 배타적이다.
- raw `PAIR`, `DEALER`, `ROUTER`, `SUB`, `XSUB` 는 direct receive callback
  install surface 를 두지 않는다. `PAIR`, `DEALER`, `ROUTER` 는 공개
  recv 메서드로만 수신하고, `SUB`, `XSUB` 는 topic subscribe 수신 표면으로만
  수신한다.
- callback 등록 후 같은 subject 에 대한 direct recv 와 해당 data-plane
  `ZLINK_POLLIN` 등록은 `EBUSY` 로 실패할 수 있다. 정확한 적용 범위는
  STREAM / SPOT 의 타입별 규칙을 따른다.
- public callback setter는 replace-only다. `NULL` 전달은 허용하지 않는다.

#### Spot Dispatch Event Handler

Spot 의 핵심 event dispatcher 는 `zlink_spot_dispatch_event_handler()` 다.
이 handler 를 등록하면 Spot 에 관련된 모든 이벤트가 하나의 callback 으로 올라온다.
같은 `spot` 에 대해서는 callback 이 순차적으로 전달되어야 한다. 구현은 같은
`spot` 의 dispatch callback 을 동시에 호출하거나 재진입 호출해서는 안 된다.
callback 안에서 event 종류를 확인하고 recv 를 호출하면서 Spot 메시징을
순차적으로 처리할 수 있어야 한다.

이 직렬화는 `spot` 단위다. 서로 다른 `spot` 사이에는 전역 직렬화를 요구하지
않는다. 구현은 다른 Spot 들을 병렬로 처리할 수 있어야 하며, 그 과정에서도
같은 `spot` 의 순차 처리 계약은 유지되어야 한다.

```c
typedef enum zlink_spot_dispatch_event_t {
    ZLINK_SPOT_DISPATCH_EVENT_SUBSCRIBE_READABLE = 1,
    ZLINK_SPOT_DISPATCH_EVENT_ROUTED_READABLE = 2,
    ZLINK_SPOT_DISPATCH_EVENT_TIMER_READABLE = 3,
    ZLINK_SPOT_DISPATCH_EVENT_CHANNEL_REPLY_READABLE = 4,
    ZLINK_SPOT_DISPATCH_EVENT_ACTOR_READABLE = 5,
    ZLINK_SPOT_DISPATCH_EVENT_ACTOR_JOIN_READABLE = 6
} zlink_spot_dispatch_event_t;

typedef enum zlink_spot_dispatch_subject_kind_t {
    ZLINK_SPOT_DISPATCH_SUBJECT_SPOT = 1,
    ZLINK_SPOT_DISPATCH_SUBJECT_TIMER = 2,
    ZLINK_SPOT_DISPATCH_SUBJECT_CHANNEL_DEALER = 3,
    ZLINK_SPOT_DISPATCH_SUBJECT_ACTOR = 4
} zlink_spot_dispatch_subject_kind_t;

typedef struct zlink_spot_dispatch_info_t {
    zlink_spot_dispatch_event_t event;
    zlink_spot_dispatch_subject_kind_t subject_kind;
    void *subject;
} zlink_spot_dispatch_info_t;

typedef void (*zlink_spot_dispatch_event_handler_fn)(
    void *spot, const zlink_spot_dispatch_info_t *info, void *userdata);

zlink_handler_result_t zlink_spot_dispatch_event_handler(void *spot,
    zlink_spot_dispatch_event_handler_fn handler, void *userdata);
```

사용 패턴:
- dispatch event handler 를 등록한다.
- callback 이 호출되면 `info->event`, `info->subject_kind`, `info->subject`를 확인한다.
- 같은 `spot` 의 활성 dispatch callback 안에서는 기본 recv surface 를 사용할 수 있다.
- `SUBSCRIBE_READABLE` 이면 `zlink_spot_subscribe_part()` 또는
  `zlink_spot_recv_subscription_event()` 로 pub/sub plane 을 drain 한다.
- `ROUTED_READABLE` 이면 `zlink_spot_recv_part()` 로 routed/request 메시지를 recv 한다.
- `TIMER_READABLE` 이면 `info->subject` timer handle에 대해 `zlink_timer_recv()` 로 timer fire 를 recv 한다.
- `CHANNEL_REPLY_READABLE` 은 readiness 신호일 뿐이며 별도 public drain API 는
  없다. reply 는 `zlink_spot_request_channel_part()` 호출 시 등록한
  `zlink_reply_handler_fn` 을 통해 core 가 자동으로 전달한다. `info->subject`
  dealer handle 은 deprecated dealer attach 경로에서만 의미가 있는 진단 정보다.
- `ACTOR_READABLE` 이면 `info->subject` 로 전달된 Actor subject를 기준으로
  `zlink_spot_node_actor_recv_part()` 를 drain 한다. public API는 raw subject
  pointer나 part loop를 노출하지 않고 `ActorReceived` 또는 동등한 aggregate
  값 객체를 돌려준다.
- `ACTOR_JOIN_READABLE` 이면 `zlink_spot_actor_join_recv()` 로 join request
  plane 을 drain 한다.
- dispatch event 는 readable 알림이다. callback 1회가 메시지 1개를 뜻하지는 않는다.
- callback 안에서는 해당 plane 을 더 이상 읽을 것이 없을 때까지 drain 할 수 있어야 한다.
- `zlink_spot_recv_part()` 의 첫 호출은 hidden activation, hidden queue open, hidden registration 을 수행하면 안 된다.
- 같은 `spot` 의 dispatch callback 은 직렬화되므로 Spot 메시징을 순차적으로 처리할 수 있다.
- 서로 다른 `spot` 은 병렬 처리될 수 있으므로 고성능 room 실행 모델을 구성할 수 있다.

#### Spot Timer API

Spot 소유 timer 는 `zlink_spot_timer_new(spot)` 로 생성하고, 이후 공통
`zlink_timer_*` 함수로 제어한다.

```c
void *zlink_spot_timer_new(void *spot);

/* use the common timer API after creation */
zlink_close_result_t zlink_timer_destroy(void **timer_p);
zlink_config_result_t zlink_timer_start(void *timer,
    uint64_t interval_ns, uint64_t repeat_count);
zlink_config_result_t zlink_timer_stop(void *timer);

typedef void (*zlink_timer_handler_fn)(
    void *timer, uint64_t fire_count, void *userdata);

zlink_handler_result_t zlink_timer_handler(void *timer,
    zlink_timer_handler_fn handler, void *userdata);
zlink_recv_result_t zlink_timer_recv(void *timer, uint64_t *fire_count_out);
```

규칙:
- timer 는 `zlink_spot_timer_new(spot)` 로 Spot 에 종속하여 생성한다.
- 생성 후에는 `zlink_timer_start`, `zlink_timer_stop`, `zlink_timer_recv`,
  `zlink_timer_handler`, `zlink_timer_destroy` 공통 API로 제어한다.
- `interval_ns` 는 나노초 단위다. `repeat_count = 0` 이면 무한 반복.
- timer fire 는 dispatch event handler 에 `TIMER_READABLE` 로 올라온다.
- timer handler callback 을 직접 등록하거나 `zlink_timer_recv()` 로 polling 할 수 있다.
- dispatch callback 안에서는 `zlink_timer_recv()` 로 pending fire 를 순차 처리할 수 있다.

바인딩 규칙:
- timer 는 typed wrapper 로 노출한다.
- `interval_ns` 는 언어별 Duration 타입으로 변환한다.
- timer 와 dispatch event 를 통합하여, 사용자는 callback 등록만으로
  sub recv + routed recv + timer 를 동기화 없이 처리할 수 있어야 한다.

#### Dispatch 모델 요약

```
zlink_spot_dispatch_event_handler callback
  (serialized per spot, non-reentrant)
  |-- SUBSCRIBE_READABLE -> zlink_spot_subscribe_part()
  |                         or zlink_spot_recv_subscription_event()
  |-- ROUTED_READABLE -> zlink_spot_recv_part()
  |-- TIMER_READABLE -> zlink_timer_recv()
  |-- CHANNEL_REPLY_READABLE -> readiness only; reply handler runs internally
  |-- ACTOR_READABLE -> zlink_spot_node_actor_recv_part()
  `-- ACTOR_JOIN_READABLE -> zlink_spot_actor_join_recv()
```

같은 `spot` 에 대해서는 이 callback 안에서 recv, send, reply 를 순차적으로
처리할 수 있어야 한다.
서로 다른 `spot` 은 필요하면 병렬로 실행될 수 있어야 한다.
callback 안에서는 event 로 알려진 plane 을 drain 할 수 있어야 한다.

#### Receive-model 요약

| 소켓 타입 | 수신 경로 |
|-----------|----------|
| `PAIR` / `DEALER` | runtime은 `zlink_recv_part()` 를 사용하고 public 표면은 aggregate recv |
| `SUB` / `XSUB` | runtime은 `zlink_subscribe_part()` 를 사용하고 public 표면은 aggregate topic recv |
| `ROUTER` | runtime은 `zlink_router_recv_part()` 를 사용하고 public 표면은 aggregate routed recv. request completion 은 `zlink_reply_handler_fn` 으로 유지 |
| `STREAM` | 아래 세 모드 중 하나 (상호 배타). raw recv / `zlink_recv_handler()` / `zlink_stream_packet_handler()` |
| `SPOT` | `zlink_spot_recv_part()` + `zlink_spot_subscribe_part()` + `zlink_spot_recv_subscription_event()` + `zlink_spot_recv_actor_lifecycle()` + `zlink_spot_dispatch_event_handler()`. direct routed callback은 노출하지 않는다 |

바인딩은 위 계약을 구현에 그대로 반영한다. public 소켓 클래스에는 aggregate
recv 표면만 노출하고, 금지된 callback install surface 는 base 클래스 어디에서도 우회 접근되지
않도록 한다.

#### Typed Receive Surface

SPOT 수신은 여러 typed surface 를 제공한다.
바인딩은 이 typed surface 위에 언어별 handler/callback 표면을 얹는다.

#### Spot 수신

```c
zlink_recv_result_t zlink_spot_recv_part(void *spot, ...);
zlink_recv_result_t zlink_spot_recv_actor_lifecycle(void *spot, ...);
```

- `request_seq = 0` 이면 ordinary routed message다.
- `request_seq != 0` 이면 request-reply message다.
- `source_rid + spot_rid` 는 발신자 주소이며 reply target 으로 사용한다.
- 바인딩 public API는 part helper 대신 aggregate `Received` 또는 언어별 동등 타입을 노출한다.
- Actor lifecycle은 dispatch event 뒤 `zlink_spot_recv_actor_lifecycle()`로 drain한다.

#### Router 수신 (routed 통합 recv 표면)

```c
zlink_recv_result_t zlink_router_recv_part(void *router,
    const zlink_routing_id_t **source_node_rid_out,
    const zlink_routing_id_t **source_spot_rid_out,
    uint64_t *request_seq_out,
    zlink_msg_t *part_out, zlink_part_flag_t *has_more_out,
    zlink_recv_flags_t flags);
```

- ROUTER 의 routed 수신은 단일 plane 이다. 일반 ROUTER 트래픽과
  spot-origin routed 트래픽을 하나의 recv 로 받는다.
- `source_spot_rid == NULL` 이면 일반 ROUTER 트래픽 (reply 는
  `zlink_router_reply_part` 사용). `source_spot_rid` 가 채워져 있으면
  spot-origin 트래픽 (reply 는 `zlink_router_reply_spot_part` 사용).
- `request_seq == 0` 이면 fire-and-forget. `request_seq != 0` 이면 request.
- 바인딩은 ROUTER data-plane callback install surface 를 별도로 노출하지 않는다.
  request completion callback 은 `request(...)` 경로에서만 유지한다.

#### Pub/Sub 수신

- raw `SUB`, `XSUB` 는 수신 전용 topic socket 이다.
- 바인딩은 `zlink_subscribe_part()` typed receive substrate 위에 언어별
  aggregate topic receive surface 를 노출한다.
- direct topic callback install surface 는 raw pub/sub family 에 두지 않는다.

#### SPOT Snapshot Query

```c
zlink_config_result_t zlink_spot_node_status(void *node,
    zlink_spot_node_status_t *out);
zlink_config_result_t zlink_spot_node_peers(void *node,
    zlink_spot_node_peer_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_peers(void *node,
    const zlink_spot_node_peer_filter_t *filter,
    zlink_spot_node_peer_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_subjects(void *node,
    const zlink_spot_node_subject_filter_t *filter,
    zlink_spot_node_subject_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_internal_sockets(void *node,
    const zlink_spot_node_socket_filter_t *filter,
    zlink_spot_node_socket_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_spots(void *node,
    zlink_spot_node_spot_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_node_actors(void *node,
    zlink_spot_node_actor_entry_t *entries, size_t *count);
zlink_config_result_t zlink_spot_actors(void *spot,
    zlink_actor_ref_t *entries, size_t *count);
```

바인딩 규칙:
- snapshot 결과는 언어별 typed domain object 배열로 변환한다.
- filter query 는 typed filter builder 또는 struct 로 노출한다.
- 반환된 배열의 메모리는 바인딩이 적절히 해제해야 한다.

### SpotNode Node-Level 옵션

SpotNode의 node-level 옵션은 `zlink_set_spot_node_option()` 계열로 다룬다.

## 옵션 정책

### 공개 옵션 표면
- **public raw `setOption(key, value)` / `getOption(key)` bag 은 금지.**
- **public raw `setsockopt/getsockopt` bag 도 금지.**
- 공용 옵션은 언어에 맞는 typed surface (facade) 로만 노출한다.
- 특화 옵션도 언어에 맞는 역할 surface (facade) 로만 노출한다.
- raw enum key + 범용 setter/getter 를 돌리는 public 경로가 spec 에
  남아 있으면 정책 위반. (`set_option(ZLINK_OPT_*, value)` 같은 C 계약이
  바인딩 public API 로 올라오면 안 됨. 바인딩 내부에서 native 호출 경로는
  허용.)
- typed facade 가 이미 있으면 **raw 경로를 중복 노출하지 않는다** — 사용자가
  두 방식 중 고를 필요 없게 한다.
- 예:
  - Java/.NET: `CommonSocketOptions`, `RouterSocketOptions`
  - Go: typed method set, 역할 interface
  - Rust: typed builder, method set, newtype
  - Python/Node: property, namespace object, 역할 object, typed method set

#### Option Facade Canonical 타입 이름
- 각 바인딩은 아래 canonical facade 타입을 제공해야 한다.
- 타입 이름은 언어 케이싱 관례만 변형한다.

| Facade | 내용 | 적용 소켓 |
|---|---|---|
| `CommonSocketOptions` | linger, sendHighWaterMark, receiveHighWaterMark, sendTimeout, receiveTimeout, immediate, connectTimeout, ipv6, tcpNoDelay, tcpKeepAlive, heartbeatInterval/Ttl/Timeout, maxMessageSize, backlog, reconnectInterval/Max, submitRetryMode, submitRetryTimeout, submitRetryAttempts | 전체 |
| `RouterSocketOptions` | mandatory (bool), handover (bool), probe (bool), connectRoutingId (RoutingId), requestTimeout (Duration), peerWeight (int, read/write) | Router |
| `DealerSocketOptions` | probe (bool), requestTimeout (Duration), peerWeight (int, read/write) | Dealer |
| `StreamSocketOptions` | notify (bool) | Stream |
| `PubSocketOptions` | verbose (bool), verboser (bool), noDrop (bool), manual (bool) | Pub, XPub |
| `SubSocketOptions` | topicsCount (int, read-only) | Sub, XSub |

- 각 facade의 option 항목은 `core/include/zlink.h`의 해당 option enum 값을
  기준으로 한다.
- facade 내 option 값 타입은 Option Value Types 정책을 따른다.
- submit retry option은 raw socket facade에서 기본값을 off/0ms/0회로 노출한다.
  managed SPOT/service 내부 profile은 `LOCAL_FAILURE`/100ms/2회를 사용할 수 있지만,
  raw socket option 기본값을 바꾸지 않는다. `DONTWAIT` 호출, backpressure, admission
  거절, request submit 성공 뒤의 reply timeout은 submit retry 대상이 아니다.

### 옵션 값 타입
- option 값은 가능한 한 의미 기반 타입으로 노출한다.
- 정책:
  - `0/1` 옵션: `boolean`
  - 유한 상태 집합: `enum`
  - 시간 의미: `Duration` 또는 언어 표준 시간 타입
  - binary identifier: `RoutingId` 같은 value object
  - 진짜 수치 설정: `int`/`long`
  - 문자열/바이트: `String`/`byte[]`
- option 이름만 enum이고 값은 raw `int`인 형태는 충분하지 않다.

## 성능 정책
- 성능은 별도 최적화 항목이 아니라 public API 설계의 일부다.
- canonical hot path는 숨은 비용이 가장 적은 경로여야 한다.
- hot path에서는 다음을 기본적으로 금지한다.
  - 숨은 payload 복사
  - 숨은 배열/리스트 재할당
  - 불필요한 UTF-8 인코딩/디코딩
  - 바인딩 레이어의 중복 포장
  - 결과를 만들기 위한 불필요한 boxing/unboxing
- 편의 API는 기본 경로보다 비용이 더 크면 문서화해야 한다.
- callback path와 direct receive path는 payload shape뿐 아니라 비용 모델도
  과도하게 벌어지면 안 된다.
- zero-copy, borrowed, owned 경로가 다르면 ownership과 함께 비용 모델도
  문서화해야 한다.
- 성능 검증 강도는 언어와 런타임 특성에 따라 달라질 수 있다.
- 다만 모든 바인딩은 hot path에서 불필요한 복사, 할당, 변환을 줄이는 방향을
  기본 정책으로 삼아야 한다.

### 고성능 버퍼 생태계 정책 (Recommended)
- canonical public contract 는 계속 `Message` / `List<Message>` / `Received` /
  `TopicMessage` 를 기준으로 유지한다.
- 다만 send / publish / request / reply 입력 경로에서는, **해당 언어에서 사실상
  표준급이고 copy 감소 효과가 큰 버퍼 생태계 타입**을 adapter surface 로
  지원하는 것을 권장한다.
- 이 지원은 canonical contract 를 대체하지 않는다.
  - recv 결과를 외부 라이브러리 타입으로 바꾸지 않는다.
  - domain object 필드 타입을 외부 라이브러리 타입으로 바꾸지 않는다.
  - 지원하더라도 `Message` 생성 / 입력 adapter / `from_*` helper /
    `impl IntoMultipart` 같은 진입점으로 제한한다.
- 지원 기준:
  - 그 언어의 네트워킹/IO 생태계에서 널리 쓰이는가
  - zero-copy 또는 copy 감소 효과가 실질적인가
  - 특정 프레임워크 종속을 public surface 전체에 강제하지 않는가
- 비기준:
  - niche 라이브러리
  - 특정 회사/프로젝트 내부에서만 주로 쓰는 버퍼 타입
  - canonical type 을 대체하려는 wrapper

권장 우선순위:

| 언어 | 권장 지원 | 수준 | 비고 |
|---|---|---|---|
| Java | Netty `ByteBuf` | Recommended | 네트워크 스택에서 매우 흔하고 direct/off-heap 경로 가치가 큼 |
| Java | Agrona `DirectBuffer` | Optional | 저지연 계열에서 유용하지만 Netty보다 우선순위는 낮음 |
| .NET | `ReadOnlyMemory<byte>` / `ReadOnlySequence<byte>` / `IBufferWriter<byte>` | Recommended | 표준 버퍼 생태계. copy 감소 효과가 큼 |
| .NET | `PipeReader` / `PipeWriter` | Optional | `System.IO.Pipelines` 사용자층에 유용 |
| Rust | `bytes::Bytes` / `BytesMut` | Recommended | async/network 생태계에서 사실상 표준급 |
| Python | buffer protocol / `memoryview` | Recommended | `bytes` / `bytearray` 외 zero-copy 입력 경로 확보 |
| Node | `Buffer` / `Uint8Array` | Baseline | 사실상 기본 지원 범주 |
| Go | `[]byte` / `[][]byte` | Baseline | 언어 기본 경로가 이미 hot path 표준 |

- 설계 규칙:
  - adapter 는 input-side convenience 여야 한다. canonical return type 을
    바꾸지 않는다.
  - 언어 표준 라이브러리나 런타임이 아닌 **third-party buffer type** 은
    가능하면 core binding 이 아니라 별도 extension module 로 분리한다.
    예를 들어 Java `ByteBuffer` 는 core 에 둘 수 있지만, Netty `ByteBuf` 는
    별도 Netty extension 에 두는 방향이 맞다.
  - adapter 지원 여부 때문에 overload 폭이 과도하게 늘어나면 안 된다.
    가능하면 `MessageLike`, `IntoMultipart`, buffer protocol 같은 **한 개의
    통합 진입점**으로 흡수한다.
  - 외부 버퍼 타입을 받더라도 ownership / retain / release 규칙은 바인딩이
    문서로 명확히 정의해야 한다.
  - 프레임워크별 객체 수명 규칙 (`ByteBuf.retain/release`, pooled buffer 등)을
    사용자가 추측하게 두면 안 된다.
  - "지원 가능" 과 "zero-copy 보장" 을 혼동하지 않는다. zero-copy 보장이
    불가능하면 문서에 copy 가능성을 명시한다.

### Codec / Serializer Extension 모듈 정책
- `Message` 와 multipart transport 자체는 계속 canonical binding core contract 다.
- protobuf / json / messagepack codec-aware domain conversion 은
  **binding core 위에 올라가는 정식 별도 extension contract** 로 취급한다.
- 단, `C` binding 은 예외다. `C`는 raw transport contract 를 기본 public surface 로
  유지하며, codec-aware domain conversion 을 기본 binding contract 로 요구하지
  않는다.
- 따라서 `Parse(...)`, `Serialize(...)`, `ToMessage(...)`, `FromMessage(...)`
  같은 helper 를 public 으로 노출할 수 있다. 다만 이 helper 는 binding core
  package/module 에 섞으면 안 된다.
- Required rules:
  - binding core package/module 은 codec-agnostic 해야 한다.
  - binding core 가 protobuf/json/messagepack dependency 를 필수 의존성으로
    끌고 들어오면 안 된다.
  - `C` binding 은 raw byte/message contract 만 정식으로 유지하면 되며,
    protobuf/json helper 를 public contract 로 추가할 의무가 없다.
  - `C`를 제외한 binding 은 codec extension layer 를 public contract 로 두며,
    `protobuf`, `json`, `messagepack` 세 codec 을 지원해야 한다.
  - `C`를 제외한 binding 의 `protobuf`, `json`, `messagepack` extension 은
    각각 **core binding 과 별도 배포 단위** 로 제공해야 한다.
  - third-party buffer adapter extension 도 같은 원칙을 따른다.
    core binding 과 별도 배포 단위로 제공해야 하며, core binding 이 그
    extension dependency 를 필수로 요구하면 안 된다.
  - codec extension 은 core binding 에 의존할 수 있지만, core binding 이 codec
    extension 에 의존하면 안 된다.
  - codec extension 이 추가되어도 canonical recv/request/reply contract 는 계속
    `Message`, `List<Message>`, `Received`, `TopicMessage` 기준으로 유지한다.
  - codec extension 은 object <-> `Message` encode/decode helper 계약만 정의한다.
    payload 타입에 필요한 parser, schema, generated type 입력을 받는 것은 허용된다.
  - codec extension 은 transport 결과 타입을 domain object 로 바꾸는 helper 를
    추가할 수 있지만, raw transport contract 자체를 대체하면 안 된다.
  - codec extension 문서는 packet name 추론 규칙, high-level outbound serializer
    lookup, typed request/reply decode 정책을 정의하지 않는다.
  - framework 가 존재하는 언어에서는 위 정책을 framework 문서가 담당한다.
    codec extension 문서는 low-level encode/decode helper 입력 조건만 설명한다.
- 이유:
  - raw transport 사용자에게 특정 codec dependency 를 강제하지 않기 위함이다.
  - 언어별 codec 생태계 선택이 다르므로 core binding 이 한 구현체에 잠기지
    않게 하기 위함이다.
  - high-level domain helper 와 low-level transport ownership 계약을 분리해서
    변경 파급을 줄이기 위함이다.

JSON codec baseline by language:

| Language | JSON baseline |
|---|---|
| C | none required |
| C++ | `nlohmann/json` |
| .NET | `System.Text.Json` |
| Java | `Jackson` |
| Node | built-in `JSON.parse` / `JSON.stringify` |
| Python | stdlib `json` |
| Go | `encoding/json` |
| Rust | `serde_json` |

- 이 표는 "json codec extension 을 public 으로 노출할 때 기본으로 삼는 구현체"를
  뜻한다.
- 다른 json 라이브러리를 추가 지원할 수는 있다. 다만 public contract 와 sample,
  test, 기본 동작 기준은 위 표를 따른다.
- Node 는 built-in JSON 이 plain object encode/decode 의 기준이며, typed
  validation 은 별도 schema/parser object 위에 얹을 수 있다.

MessagePack codec baseline by language:

| Language | MessagePack baseline |
|---|---|
| C | none required |
| C++ | `msgpack-c` |
| .NET | `MessagePack for C#` |
| Java | `jackson-dataformat-msgpack` |
| Node | `@msgpack/msgpack` |
| Python | `msgpack` |
| Go | `vmihailenco/msgpack/v5` |
| Rust | `rmp-serde` |

Bindings는 더 이상 codec extension 배포 단위를 정의하지 않는다.

| Language | Core binding root | Binding-owned codec package 정책 |
|---|---|---|
| C | `bindings/c/include/zlink/`, `bindings/c/src/` | 없음 |
| C++ | `bindings/cpp/include/zlink/` | 없음. framework 직렬화는 `framework/languages/cpp/extensions/`에서 다룬다 |
| .NET | `bindings/dotnet/src/Zlink/` | 없음. framework 직렬화는 `framework/languages/dotnet/src/`에서 다룬다 |
| Java | `bindings/java/src/main/java/systems/zlink/` | 없음. framework 직렬화는 `framework/languages/java/`에서 다룬다 |
| Node | `bindings/node/src/` | 없음. framework 직렬화는 `framework/languages/node/packages/`에서 다룬다 |
| Python | `bindings/python/src/zlink/` | 없음. raw `Message`/bytes만 유지한다 |
| Go | `bindings/go/` | 없음. raw `Message`/bytes만 유지한다 |
| Rust | `bindings/rust/src/` | 없음. raw `Message`/bytes만 유지한다 |

- 배치 규칙:
  - codec helper source를 core socket/message namespace와 같은 디렉터리에 직접 섞지 않는다.
  - 언어별 codec spec 문서는 raw-only 정책을 설명하고, 해당 언어가 framework target이면
    framework codec extension 위치를 안내한다.
  - binding sample과 test는 raw `Message`/bytes 동작을 검증한다.

### 외부 버퍼 Attach / Release Hook 정책
- C API 의 `zlink_msg_init_data(..., zlink_free_fn*, hint)` 는 **external buffer
  attach + release hook** 능력을 제공한다.
- 바인딩은 이 능력을 **언어 관용구와 메모리 모델에 맞을 때만** public 으로
  노출한다.
- 기본 원칙:
  - **copy-based `Message` 생성 경로는 모든 바인딩에서 Required**
  - **VM 또는 GC 기반 언어(Java, .NET, Go, Python, Node)는 VM-managed
    buffer를 native queue에 borrowed/zero-copy 로 넘기는 public API 를
    제공하지 않는다**
  - **release hook 없는 borrowed zero-copy wrap API 는 managed 언어 public
    surface, default send path, perf 전용 fast path 에 두지 않는다**
  - VM 언어의 성능 경로는 caller buffer 를 native queue 에 빌려주는 방식이
    아니라, native-owned `Message` 를 만들고 그 payload 를 채운 뒤 part 기반
    send/recv API 로 넘기는 방식이어야 한다.
  - external buffer attach 는 **release 시점을 public contract 로 닫을 수 있을
    때만** 허용한다
- 허용:
  - C++
    - `from_external(..., zlink_free_fn*, hint)` 같은 형태로 external attach 허용
    - release hook 이 explicit 하므로 public contract 로 닫을 수 있다
- 비권장/금지:
  - Java / .NET / Go / Rust / Python / Node
    - generic public borrowed wrap (`wrapDirect`, `wrapNative`, `wrap_buffer`
      등) 금지
    - VM-managed buffer 를 `zlink_msg_init_data(..., NULL, NULL)` 로 native
      queue 에 넘기는 send/publish/request/reply fast path 금지
    - VM-managed buffer 를 pin 한 뒤 release callback 으로 풀어 주는 public 또는
      default fast path 금지
    - 이유: send 후 backing buffer lifetime, retain/release, arena/session,
      GC 와의 상호작용을 public contract 로 안전하게 닫기 어렵다
- 예외:
  - C++처럼 caller 가 release hook 과 lifetime 을 명시적으로 소유하는 언어만
    advanced external attach 를 둘 수 있다.
  - VM 또는 GC 기반 언어에서 이 예외를 추가하려면 별도 draft spec, public
    lifetime contract, 회귀 테스트, perf 비교가 먼저 필요하다. 정식 spec 과
    구현에는 바로 추가하지 않는다.

## 경계 비용 정책
- 경계 검증은 가장 이른 안전한 위치에서 한 번 수행하는 것을 우선한다.
- 같은 검증을 여러 레이어에서 반복하면 이유가 명확해야 한다.
- 고정 크기 native struct에 들어가는 값은 truncation 대신 즉시 오류를 반환한다.
- 문자열, topic, routing id, metadata 같은 경계 값은 다음을 함께 고려한다.
  - 길이 상한
  - 인코딩 비용
  - 복사 횟수
  - 재할당 정책
- core의 고정 크기 struct 필드에 대응하는 바인딩 입력의 길이 상한:

  | 필드 | C struct 크기 | 바인딩 검증 책임 |
  |------|--------------|----------------|
  | `RoutingId` | `data[255]` | 값 객체 생성 시 255바이트 초과 시 즉시 오류 반환 |
  | topic / filter | C 문자열 (null-terminated) | 바인딩은 embedded null 문자 포함 시 즉시 오류 반환. 길이 상한은 core가 처리하므로 바인딩에서 별도 길이 검증하지 않는다 |
  | channel_name | `char[256]` | 255바이트 초과 시 즉시 오류 반환 |
  | endpoint | `char[256]` | 255바이트 초과 시 즉시 오류 반환 |
  | metadata | `zlink_msg_t` (가변) | core가 처리, 바인딩은 null 검증만 |

- 바인딩은 고정 크기 필드에 들어가는 값이 상한을 넘으면 truncation 없이
  즉시 예외/오류를 반환한다.
- public 도메인 객체를 만들 때 불필요한 중간 컬렉션 생성은 피한다.
- helper나 sample이 느린 경로를 canonical path처럼 보이게 만들면 안 된다.

## Peer 가중치 정책

peer 가중치는 peer-level outbound 선택 비율과 drain 상태를 제어하는 canonical
surface 다. 모든 바인딩은 구현된 대상 handle에 대해 이를 공개해야 한다.

핵심 API/계약:
- `ZLINK_ROUTER_OPT_WEIGHT`
- `ZLINK_DEALER_OPT_WEIGHT`
- 값 범위 `0..10000`, 기본값 `100`
- submit 결과 `ZLINK_SUBMIT_NOT_ADMITTED` (값 13) — target peer 가중치가 `0`이면 반환
- socket monitor 이벤트 `ZLINK_EVENT_PEER_WEIGHT_CHANGED` (bit 15)
- `zlink_spot_node_peer_entry_t.weight` / `zlink_member_peer_entry_t.weight`

바인딩 규칙:
- `weight`는 언어 관례에 맞는 typed option/property surface로 노출한다.
  설정 대상은 `ROUTER`, `DEALER`이다. `SpotNode`와 `Spot`에는 weight 설정
  surface를 노출하지 않는다.
- `NOT_ADMITTED` 를 `SubmitError` 계열에 포함하여 caller 가
  가중치 `0` 거부를 구분할 수 있게 한다.
- `PEER_WEIGHT_CHANGED` 이벤트 bit 은 기존 socket monitor / service
  monitor surface 에 typed value 로 노출한다. `value`는 새 `0..10000`
  가중치다.
- `SpotNodePeerEntry` / `MemberPeerEntry` 도메인 객체는 `weight` 필드를
  포함해야 한다.

## Monitor 정책
- monitor plane도 같은 규칙을 따른다.
- public monitor receive는 `recv()` 하나로 제공한다.
  - blocking/non-blocking 은 flags 파라미터 또는 언어별 관례로 제어한다.
- monitor event는 data plane과 별도지만, blocking/non-blocking 구분 방식은
  동일해야 한다.
- monitor는 socket의 상태 변화, readiness 변화, lifecycle event를 관찰하는
  별도 plane 이다.
- monitor payload는 message data plane payload와 혼동되면 안 된다.
- monitor event type은 typed event surface 또는 동등한 의미 surface로
  노출해야 한다.
- monitor consumer는 raw integer mask만이 아니라 event 의미를 읽을 수 있어야
  한다.
- monitor lifecycle은 관찰 대상 socket lifecycle과의 관계가 설명 가능해야 한다.
  - monitor open 시점
  - monitor close 시점
  - observed socket close 이후의 동작
- monitor는 data plane을 대체하는 API가 아니다.
- monitor의 readiness/state event 의미는 data plane contract와 충돌하지
  않아야 한다.
- monitor sample과 test는 다음을 보여야 한다.
  - event 수신 성공 경로
  - non-blocking empty 경로
  - socket state 변화와 monitor event의 관계

## 오류 정책

### 바인딩 검증 vs Native 오류
- 입력 값의 형식/범위 오류는 바인딩이 즉시 막는다.
- socket 상태, 연결 상태, transport 상태, protocol 상태 오류는 코어가
  결정하고 바인딩은 그대로 caller에 전달한다.

### 바인딩이 검증해야 하는 항목
- truncation 가능성이 있는 값
- overflow 가능성이 있는 값
- fixed-size native struct에 들어가는 값
- 명백한 길이 상한이 있는 값
- offset/length 범위 오류
- null 불가 인자
- enum 범위 밖의 값

이 경우 바인딩 예외를 사용한다.
- Java: `IllegalArgumentException`, `IndexOutOfBoundsException`,
  `NullPointerException`
- .NET: `ArgumentException`, `ArgumentOutOfRangeException`,
  `ArgumentNullException`
- Go: 즉시 `error` 반환 또는 `panic` (프로그래머 오류)
- Rust: compile-time 보장 (`NonZero`, newtype) 또는 `panic!` / `Result<T, E>`

### Native 가 결정하는 항목
- peer 없음
- backpressure
- readiness 부족
- callback mode와 direct recv 충돌
- socket type/state/runtime 문제
- transport, TLS, endpoint, protocol 오류

이 경우 바인딩은 native 오류를 언어별 관용구로 변환하여 caller에 전달한다.
Exception 언어는 예외를 던지고, return-based 언어는 에러 값을 반환한다.
- C++: `throw zlink_error_t`
- Java: `throw ZlinkException`
- .NET: `throw ZlinkException`
- Node: `throw ZlinkError` (extends `Error`)
- Python: `raise ZlinkError` (extends `Exception`)
- Go: `return err` (`ZlinkError` 또는 동등한 typed error)
- Rust: `Err(E)` (`Result<T, E>`; 여러 함수군이 섞일 때만 `ZlinkError`)

### Error Code 표

zlink 에서 사용하는 코드와 의미. 바인딩은 이 코드를 언어별 에러 타입에
매핑하여 caller 가 원인을 구분할 수 있게 한다.

코드는 두 계층으로 나뉜다.

1. **Public result enum 코드 (0–706)** — 공개 C API 함수의 반환 enum 값.
   바인딩이 직접 마주하고 언어별 에러 타입으로 노출해야 하는 값이다.
   전체 정의는 [core/errno-map.md](https://kairos-code-dev.github.io/zlink/en/spec/core/04-errno-map/) 참조.
2. **Internal errno** — `zlink_errno()` 로 조회되는 내부 raw errno.
   `INTERNAL_ERROR` 같은 coarse bucket 의 상세 원인 조회용. 바인딩은 이 값을
   `internalErrno` / `internal_errno` 필드로 노출한다 (디버깅 전용).

#### Public Result Enum 카탈로그

바인딩은 아래 8 개 enum 의 **모든 값을 누락 없이** 언어별 표현으로 매핑해야
한다. OK (0) 는 모든 enum 에 공통이며 에러로 취급하지 않는다.

##### `zlink_submit_result_t` (send, request submit, reply submit)

| 값 | 상수 | 내부 errno | 분류 | 의미 |
|----|------|-----------|------|------|
| 0 | `OK` | — | 성공 | 제출 성공 |
| 1 | `BACKPRESSURED` | `EAGAIN` | 제어 흐름 | send 큐 포화 (HWM) |
| 2 | `NOT_CONNECTED` | `ENOTCONN`, `EHOSTUNREACH` | 제어 흐름 | 대상 peer/경로 미연결 |
| 3 | `NOT_FOUND` | `ENOENT` | 제어 흐름 | 대상 peer/spot/route 없음 |
| 13 | `NOT_ADMITTED` | `ECONNREFUSED` 계열 | 제어 흐름 | target peer 가중치가 `0`이라 신규 submit 거부 |
| 4 | `TERMINATED` | `ETERM` | 런타임/생명주기 | context 종료됨 |
| 5 | `INVALID_HANDLE` | `EFAULT` | caller 계약 위반 | NULL handle / invalid pointer |
| 6 | `INVALID_ARGUMENT` | `EINVAL` | caller 계약 위반 | 잘못된 인자 |
| 7 | `NOT_SUPPORTED` | `ENOTSUP` | caller 계약 위반 | 해당 소켓 타입에서 지원 안 함 |
| 8 | `INVALID_STATE` | `EFSM`, `EBUSY` | caller 계약 위반 | 소켓/handle 상태 오류 |
| 9 | `THREAD_VIOLATION` | `EMTHREAD` | caller 계약 위반 | 잘못된 스레드에서 접근 |
| 10 | `OUT_OF_MEMORY` | `ENOMEM` | 내부 실패 | 메모리 할당 실패 |
| 11 | `SEQ_EXHAUSTED` | `EBUSY` | 내부 실패 | request seq 공간 고갈 |
| 12 | `INTERNAL_ERROR` | `EPROTO` 등 | 내부 실패 | 내부 submit 실패 (상세는 `zlink_errno()`) |

##### `zlink_request_result_t` (request completion callback)

| 값 | 상수 | 내부 errno | 의미 |
|----|------|-----------|------|
| 0 | `OK` | `0` | reply payload 수신 성공 |
| 101 | `TIMED_OUT` | `ETIMEDOUT` | `timeout_ms` 내 reply 미도착 |
| 102 | `NOT_FOUND` | `ENOENT` | 대상 없음, 에러 reply 로 완료 |
| 103 | `TERMINATED` | `ETERM` | (예약) 명시적 종료 완료 경로 |
| 104 | `PROTOCOL_ERROR` | `EPROTO` | reply envelope / error reply payload 손상 |
| 105 | `INTERNAL_ERROR` | `EPROTO` 등 | 내부 request 실패 (상세는 `zlink_errno()`) |
| 106 | `REJECTED` | `EACCES`, `ECONNREFUSED` | 대상이 request를 명시적으로 거절 |
| 107 | `CONFLICT` | `ESTALE` | request 대상 또는 상태 충돌 |
| 108 | `BUSY` | `EBUSY` | request 처리 경로가 일시적으로 바쁨 |
| 109 | `NOT_CONNECTED` | `ENOTCONN`, `EHOSTUNREACH` | 대상 peer/경로 미연결 |
| 110 | `INVALID_ARGUMENT` | `EINVAL`, `EFAULT` | request 인자 또는 envelope 오류 |
| 111 | `INVALID_STATE` | `EFSM` | request를 받을 수 없는 handle 상태 |
| 112 | `NOT_SUPPORTED` | `ENOTSUP`, `EOPNOTSUPP` | request 미지원 대상 |

##### `zlink_recv_result_t` (recv, subscribe, subscription event, monitor recv, timer recv)

| 값 | 상수 | 내부 errno | 의미 |
|----|------|-----------|------|
| 0 | `OK` | — | 수신 성공 |
| 201 | `NO_DATA` | `EAGAIN` | non-blocking recv 데이터 없음 / source 고갈 |
| 202 | `BUSY` | `EBUSY` | handler 이미 attach 됨 |
| 203 | `TERMINATED` | `ETERM` | context 종료됨 |
| 204 | `INVALID_HANDLE` | `EFAULT` | NULL / invalid handle |
| 205 | `NOT_SUPPORTED` | `ENOTSUP` | recv 미지원 소켓 타입 |
| 206 | `INTERNAL_ERROR` | `EPROTO` 등 | 내부 recv 실패 (상세는 `zlink_errno()`) |

##### `zlink_handler_result_t` (handler 등록)

| 값 | 상수 | 내부 errno | 의미 |
|----|------|-----------|------|
| 0 | `OK` | — | handler 등록 성공 |
| 301 | `INVALID_ARGUMENT` | `EINVAL` | NULL handler |
| 302 | `BUSY` | `EBUSY` | handler 이미 attach 됨 |
| 303 | `NOT_SUPPORTED` | `ENOTSUP` | 미지원 subject |
| 304 | `DEADLOCK` | `EDEADLK` | reentrant 호출 (send-ready handler 전용) |
| 305 | `INVALID_HANDLE` | `EFAULT` | NULL / invalid handle |
| 306 | `INTERNAL_ERROR` | `EPROTO` 등 | 내부 handler 등록 실패 (상세는 `zlink_errno()`) |

##### `zlink_close_result_t` (close, destroy)

| 값 | 상수 | 내부 errno | 의미 |
|----|------|-----------|------|
| 0 | `OK` | — | close/destroy 성공 |
| 401 | `BUSY` | `EBUSY` | in-flight callback / API 호출 |
| 402 | `SHUTDOWN` | `ESHUTDOWN` | 이미 close 됨 |
| 403 | `INVALID_HANDLE` | `EFAULT` | NULL / invalid handle |
| 404 | `INTERNAL_ERROR` | `EPROTO` 등 | 내부 close 실패 (상세는 `zlink_errno()`) |

##### `zlink_bind_result_t` (bind)

| 값 | 상수 | 내부 errno | 의미 |
|----|------|-----------|------|
| 0 | `OK` | — | bind 성공 |
| 501 | `INVALID_ARGUMENT` | `EINVAL` | 잘못된 endpoint |
| 502 | `ADDR_IN_USE` | `EADDRINUSE` | 주소 이미 사용 중 |
| 503 | `NOT_SUPPORTED` | `ENOTSUP` | 미지원 transport |
| 504 | `INVALID_HANDLE` | `EFAULT` | NULL / invalid handle |
| 505 | `INTERNAL_ERROR` | `EPROTO` 등 | 내부 bind 실패 (상세는 `zlink_errno()`) |

##### `zlink_connect_result_t` (connect, disconnect, unbind)

| 값 | 상수 | 내부 errno | 의미 |
|----|------|-----------|------|
| 0 | `OK` | — | connect/disconnect/unbind 성공 |
| 601 | `INVALID_ARGUMENT` | `EINVAL` | 잘못된 endpoint |
| 602 | `NOT_SUPPORTED` | `ENOTSUP` | 미지원 transport |
| 603 | `INVALID_HANDLE` | `EFAULT` | NULL / invalid handle |
| 604 | `INTERNAL_ERROR` | `EPROTO` 등 | 내부 connect/disconnect 실패 (상세는 `zlink_errno()`) |
| 605 | `NOT_FOUND` | `ENOENT` | endpoint 또는 peer routing id 없음 |
| 606 | `CONFLICT` | `EADDRINUSE` | peer routing id가 둘 이상의 pipe와 충돌 |
| 607 | `BUSY` | `EBUSY` | lifecycle owner가 수동 변경을 거절 |

##### `zlink_config_result_t` (option set/get, message lifecycle, snapshot, poller mutation, proxy, timer config)

| 값 | 상수 | 내부 errno | 의미 |
|----|------|-----------|------|
| 0 | `OK` | — | 설정 성공 |
| 701 | `INVALID_HANDLE` | `EFAULT` | NULL / invalid handle |
| 702 | `INVALID_ARGUMENT` | `EINVAL`, `EBUSY` | 잘못된 인자 또는 config 계층 conflict |
| 703 | `NOT_SUPPORTED` | `ENOTSUP` | 미지원 옵션 |
| 704 | `INTERNAL_ERROR` | `EPROTO` 등 | 내부 config 실패 (상세는 `zlink_errno()`) |
| 705 | `INVALID_STATE` | `EBUSY`, `ESHUTDOWN` | lifecycle 상태가 config를 거절 |
| 706 | `NOT_FOUND` | `ENOENT` | local lookup 대상 없음 |

##### Non-OK 값 총합

- 총 **59 개** non-OK 코드 (submit 13 + request 12 + recv 6 + handler 6 +
  close 4 + bind 5 + connect 7 + config 6 = 59). 값 범위:
  1–13, 101–112, 201–206, 301–306, 401–404, 501–505, 601–607, 701–706.
- 값 범위는 enum 간 겹치지 않으므로 단일 `int` 로 유일하게 구분된다.
- 바인딩은 59 개 값 모두에 대해 언어별 에러 표현을 제공해야 한다. 누락 시
  caller 가 해당 원인을 구분할 방법이 없다.

언어별 enum/상수 매핑 스타일은 아래 `언어별 ErrorCode 매핑` 절을 참조한다.

#### POSIX 표준 errno

POSIX 에서 해당 상수가 정의되지 않은 플랫폼에서는 `ZLINK_HAUSNUMERO` 기반
대체 값을 사용한다. 바인딩은 상수 이름으로 비교해야 하며 정수 값에 직접
의존하면 안 된다.

| errno | 대체 값 (POSIX 미정의 시) | 의미 | 대표 발생 상황 |
|-------|-------------------------|------|--------------|
| `ENOTSUP` | HAUSNUMERO + 1 | 지원하지 않는 작업 | 해당 소켓 타입에서 불가능한 작업 |
| `EPROTONOSUPPORT` | HAUSNUMERO + 2 | 프로토콜 미지원 | 지원하지 않는 프로토콜 요청 |
| `ENOBUFS` | HAUSNUMERO + 3 | 버퍼 공간 부족 | 내부 버퍼 할당 실패 |
| `ENETDOWN` | HAUSNUMERO + 4 | 네트워크가 다운됨 | transport 레이어 장애 |
| `EADDRINUSE` | HAUSNUMERO + 5 | 주소가 이미 사용 중 | bind 시 endpoint 충돌 |
| `EADDRNOTAVAIL` | HAUSNUMERO + 6 | 주소를 사용할 수 없음 | 잘못된 endpoint 형식 |
| `ECONNREFUSED` | HAUSNUMERO + 7 | 연결 거부됨 | 대상이 연결을 거부 |
| `EINPROGRESS` | HAUSNUMERO + 8 | 작업 진행 중 | 비동기 연결 진행 중 |
| `ENOTSOCK` | HAUSNUMERO + 9 | 소켓이 아닌 대상 | 잘못된 handle 전달 |
| `EMSGSIZE` | HAUSNUMERO + 10 | 메시지 크기 초과 | 메시지가 설정된 최대 크기 초과 |
| `EAFNOSUPPORT` | HAUSNUMERO + 11 | 주소 체계 미지원 | 지원하지 않는 주소 체계 |
| `ENETUNREACH` | HAUSNUMERO + 12 | 네트워크에 도달 불가 | 라우팅 불가 |
| `ECONNABORTED` | HAUSNUMERO + 13 | 연결이 중단됨 | 연결이 비정상 종료 |
| `ECONNRESET` | HAUSNUMERO + 14 | 연결이 재설정됨 | peer 가 연결을 강제 종료 |
| `ENOTCONN` | HAUSNUMERO + 15 | 연결되지 않은 상태 | 연결 전에 send/recv 시도 |
| `ETIMEDOUT` | HAUSNUMERO + 16 | 작업 시간 초과 | request reply timeout, 연결 timeout |
| `EHOSTUNREACH` | HAUSNUMERO + 17 | 대상에 도달할 수 없음 | peer 미연결, 라우팅 불가 |
| `ENETRESET` | HAUSNUMERO + 18 | 네트워크가 재설정됨 | 네트워크 연결 끊김 |
| `EAGAIN` | (POSIX 표준) | 자원이 일시적으로 사용 불가 | non-blocking send 시 HWM 도달 (backpressure) |
| `EINVAL` | (POSIX 표준) | 잘못된 인자 | 범위 초과, 잘못된 옵션 값 |
| `ECANCELED` | (POSIX 표준) | 작업이 취소됨 | caller 가 request 를 취소 |

`ZLINK_HAUSNUMERO` 값은 `156384712` 이다.

#### zlink 전용 errno

zlink 고유 오류 코드. POSIX errno 와 충돌하지 않도록 `ZLINK_HAUSNUMERO`
기반 오프셋을 사용한다.

| 대체 값 | 상수 | 의미 | 대표 발생 상황 |
|--------|------|------|--------------|
| HAUSNUMERO + 51 | `EFSM` | 유한 상태 기계 오류 | 소켓 상태에서 허용되지 않는 작업 (예: callback 모드에서 direct recv) |
| HAUSNUMERO + 52 | `ENOCOMPATPROTO` | 호환되지 않는 프로토콜 | 서로 다른 프로토콜 버전의 peer 연결 |
| HAUSNUMERO + 53 | `ETERM` | 컨텍스트/소켓 종료 | context 또는 소켓이 close 된 상태에서 작업 시도 |
| HAUSNUMERO + 54 | `EMTHREAD` | I/O 스레드 부족 | context 의 I/O 스레드가 부족 |

#### 언어별 ErrorCode 매핑

각 바인딩은 Public Result Enum 카탈로그의 59 개 non-OK 코드를 언어별
enum/상수로 매핑하여 타입 안전한 분기를 제공한다.

| 언어 | 처리 | ErrorCode 타입 | 접근 방식 |
|------|------|---------------|----------|
| C | return | 함수별 typed enum (`zlink_*_result_t`) | 반환값 자체 |
| C++ | throw | 통합 `ErrorCode` enum | `zlink_error_t.code()` |
| Java | throw | 통합 `ErrorCode` enum | `ZlinkException.getCode()` |
| .NET | throw | 통합 `ErrorCode` enum | `ZlinkException.Code` |
| Node | throw | 통합 `ErrorCode` enum (또는 string 상수) | `ZlinkError.code` |
| Python | throw | 통합 `ErrorCode` enum | `ZlinkError.code` |
| Go | return | 통합 `ErrorCode` typed int 상수 | `ZlinkError.Code()` |
| Rust | return (`Result`) | 통합 `ErrorCode` enum variant | `ZlinkError.code()` |

- 통합 enum 의 각 variant 는 Public Result Enum 카탈로그의 59 개 값과
  1:1 대응한다. 원본 C 의 enum 분리 (submit / recv / handler / close /
  bind / connect / config / request) 를 유지하거나, 언어 관용구에 따라
  단일 enum 으로 통합해도 된다. 둘 중 어떤 스타일이든 **값은 누락 없이 모두
  표현해야 한다**.
- 상수/variant 이름은 원본 `UPPER_SNAKE_CASE` 를 그대로 쓰거나 언어 스타일
  (`PascalCase` / `camelCase`) 로 변환한다. 숫자 값과 의미는 고정이다.
- `internalErrno` / `internal_errno` 필드는 별도로 제공하며, 주로
  `INTERNAL_ERROR` 같은 coarse bucket 의 상세 원인 조회용이다.

### Request-Reply 오류 정책

request-reply 는 Per-Function Error Type Hierarchy 의 **`RequestError`**
(request completion) 과 **`SubmitError`** (request submit) 두 하위 타입을
사용한다. `RequestError` 는 `zlink_request_result_t` 에 대응하며,
`SubmitError` 는 `zlink_submit_result_t` 에 대응한다.

오류 코드는 두 계층으로 나뉜다.

**Wire error reply 코드** — peer 가 보내는 protocol-level error reply.
wire 에서 사용 가능한 errno 는 3개로 제한된다: `ENOENT`, `EOPNOTSUPP`, `EINVAL`.

**API/completion 코드** — core 가 callback 에 전달하는 errno:

| errno | 발생 시점 |
|-------|----------|
| `ENOENT` | 대상 peer/spot 을 찾지 못함 (wire 또는 local) |
| `EOPNOTSUPP` | peer 종류 불일치 또는 지원 안 함 |
| `EINVAL` | 잘못된 파라미터 |
| `ETIMEDOUT` | reply 대기 중 timeout 초과 |
| `EPROTO` | envelope parse 실패 또는 잘못된 remote reply |
| `EBUSY` | 수신 표면 충돌 (handler 중복 등록) |

**request 오류 (`RequestError`):**

| 상황 | `request()` |
|------|------------|
| backpressure | writable 대기 (timeout 에 합산) |
| timeout | `RequestError(TIMED_OUT)` |
| 대상 없음 | `RequestError(NOT_FOUND)` |
| remote error reply | `RequestError(해당 코드)` |
| 소켓 close | `RequestError(TERMINATED)` |
| protocol error | `RequestError(PROTOCOL_ERROR)` |
| pending map 에 없는 reply | 무시 |

**reply 오류 (`SubmitError`):**

| 상황 | `reply()` |
|------|-----------|
| 성공 | 정상 반환 |
| backpressure | `SubmitError(BACKPRESSURED)` |
| not connected | `SubmitError(NOT_CONNECTED)` |
| 기타 실패 | `SubmitError(해당 submit 코드)` |

- async request 는 완료 실패를 async completion 경로 (Future reject / await
  error) 로 전달한다.
- callback request 는 **submit 실패를 즉시 throw/return** 하고, submit 성공 후의
  완료 실패만 callback 의 `RequestResult` / `RequestError` 로 전달한다.
- 함수군별 하위 에러 타입을 사용한다 (Per-Function Error Type Hierarchy 참조).
  - submit 실패: `SubmitException` / `SubmitError`
  - request 완료 실패: `RequestException` / `RequestError`
- 언어별 표현:
  - Java: `SubmitException` / `RequestException` — `getCode()` 로 원인 구분 (unchecked)
  - .NET: `ZlinkSubmitException` / `ZlinkRequestException` — `Code` property
  - Node: `SubmitError` / `RequestError` — `code` property
  - Python: `SubmitError` / `RequestError` — `code` attribute
  - C++: `submit_error_t` / `request_error_t` — `.code()` 메서드
  - Go: `*SubmitError` / `*RequestError` — `Code()` 메서드 (interface)
  - Rust: `Err(SubmitError{..})` / `Err(RequestError{..})`, 또는 다중 함수군
    경계에서는 `Err(ZlinkError::Submit(..))` / `Err(ZlinkError::Request(..))`
    — `.code()` 메서드

## 길이와 범위 경계 정책
- 검증 책임은 두 층으로 나눈다.
- 값 객체가 존재하는 타입:
  - 값 객체 생성 시점에 canonical validation을 수행한다.
  - 예: `RoutingId`, typed enum wrapper, bounded identifier
- 값 객체가 존재하지 않거나 호출 문맥 의존 변환이 필요한 타입:
  - native 호출 직전에 검증한다.
  - 예: `Duration -> int millis`, offset/length slicing, output buffer sizing
- native 호출 직전 재검증은 아래 경우에만 필수다.
  - 값 객체를 거치지 않는 raw 경로가 존재하는 경우
  - 값 객체 생성 후 호출 직전 추가 변환이 들어가는 경우
  - 값 객체가 아닌 복합 입력 조합에서 overflow/truncation이 생길 수 있는 경우
- truncation 후 native로 넘기는 동작은 금지한다.

예:
- `RoutingId`는 `zlink_routing_id_t`의 `data[255]` 계약을 넘기지 않아야 한다.
- `Duration -> int millis` 변환은 overflow를 허용하면 안 된다.
- topic, subscription, metadata처럼 고정 출력 버퍼가 개입되는 경로는 길이와
  재할당 정책이 명확해야 한다.

## 소유권 정책
- `Message` ownership은 코어 계약과 일치해야 한다.
- 모든 바인딩은 내부적으로 C API를 호출하므로, GC 언어를 포함한 전 언어에서
  native message의 ownership을 올바르게 관리해야 한다.
- ownership 경로:
  - send 성공: ownership이 native로 이동한다. 바인딩은 이후 접근하면 안 된다.
  - send 실패: restore 가능한 경로와 consume되는 경로를 혼동하지 않는다.
  - recv: native가 생성한 메시지의 ownership을 바인딩이 넘겨받는다. 바인딩이
    해제 책임을 진다.
  - 생성 후 미전송: 바인딩이 직접 생성한 메시지를 전송하지 않았다면 반드시
    명시적으로 close/해제해야 한다. GC가 managed wrapper만 수거할 뿐, native
    메모리는 해제하지 않으므로 누수가 발생한다.
- callback delivery와 direct receive는 동일한 payload shape를 가져야 한다.
- callback 후 frame validity는 계약으로 명확해야 한다.

## 네이밍 정책
- 메서드명은 언어 관례만 반영한다.
- 개념 이름은 바인딩 간 최대한 동일하게 유지한다.
- 아래 목록은 의미 기준 canonical name 이다.
- 실제 바인딩 메서드명은 다음 세 가지 변형만 허용한다.
  1. **케이싱 변형**: 언어 관례에 맞게 camelCase/PascalCase/snake_case를
     변환한다. 단어 구성은 바뀌지 않는다.
     - 예: `connectPeer` → Go: `ConnectPeer`, Python: `connect_peer`,
       C++: `connect_peer`, Rust: `connect_peer`
  2. **overload 불가 언어의 최소 접미사**: Go와 Rust처럼 overloading이 없는
     언어에서, 동일 동작의 파라미터 변형을 구분하기 위해 최소한의 접미사를
     허용한다. 이 접미사는 동작 구분이며, 파라미터 인코딩이 아니다.
     - 예: `send` → Go: `Send` / `SendTo`, Rust: `send` / `send_to`
     - 허용 접미사 범위: `To` 수준의 최소 동작 구분 접미사까지만 허용한다.
       파라미터 타입이나 의미를 풀어쓴 접미사는 금지한다.
       - 허용: `SendTo`, `send_to`
       - 금지: `SendWithRoutingId`, `send_routed`, `send_multipart`
     - 접미사 허용은 overloading도 keyword/optional parameter도 없는
       언어(Go, Rust)에만 적용된다.
     - 접미사 없이 시그니처로 구분 가능한 언어에서는 접미사를 사용하지
       않는다.
       - overloading: Java, C#, C++
       - keyword / optional parameter: Python
       - optional / union type: Node/TypeScript
  3. **언어별 property/getter 관례**: 값을 읽는 accessor는 언어 관례에 맞는
     property 또는 getter 형태를 사용할 수 있다. 단, 개념 이름은 같아야 하고
     새로운 동작 이름을 만들면 안 된다.
     - 예: canonical `getValue` → C++ `value()`, .NET `Value` 또는
       `GetValue()`, Java/Node `getValue()`
     - 예: canonical `routingId`/`getRoutingId` → C++ `routing_id()`,
       Java `routingId()`, Node `getRoutingId()`
- **그 외의 단어 교체, 단어 생략, 다른 단어 대체는 허용하지 않는다.**
  - 금지 예: `setDispatchHandler`를 `spotDispatchHandler`로 바꾸는 것 → 단어 교체
  - 금지 예: `querySnapshot`을 `snapshot`으로 줄이는 것 → 단어 생략이므로,
    canonical 이름 자체를 `snapshot`으로 정의해야 한다
- 케이싱이나 접미사가 달라져도 역할 구분과 의미 계약은 같아야 한다.
- 예: `receiveSubscriptionEvent` → Python: `receive_subscription_event`,
  Go: `ReceiveSubscriptionEvent`
- 추천 canonical 이름:
  - `bind`, `connect`, `close`
  - `send`
  - `recv`
  - `publish`
  - `subscribe`
  - `receiveSubscriptionEvent`
  - `setSubscription`, `unsetSubscription`
  - `setPacketHandler`, `setDispatchHandler`, `setSendReadyHandler`

### 메서드 이름 간결성
- 이 규칙은 public API에 엄격히 적용한다.
- internal/private API는 파라미터 인코딩이 가독성을 높이면 허용한다.
  - 내부 코드는 overloading 없이 명시적 이름이 더 읽기 좋을 수 있다.
  - 예: internal helper에서 `sendRouted(id, msg)`는 허용
- 메서드 이름은 동작(action)만 표현한다.
- 파라미터의 존재, 타입, 개수를 이름에 반복하지 않는다.
- 시그니처가 이미 설명하는 것을 이름에 다시 쓰면 안 된다.
- 동작 자체가 다른 경우(예: `send` vs `publish`)는 이름이 달라야 한다.
- 입력만 다른 경우(예: routing id 유무)는 이름을 늘리지 않는다.

안티패턴과 올바른 패턴:

| 안티패턴 | 올바른 패턴 | 이유 |
|---|---|---|
| `send(message)` | `send().message(message).submit()` | 시작점은 전송 대상만 받고 payload는 builder 단계로 분리 |
| `sendWithRoutingId(id, msg)` | `send(id).message(msg).submit()` | builder가 RoutingId와 payload를 단계로 분리 |
| `sendMultipartMessages(parts)` | `send().message(p1).message(p2).submit()` | builder의 `.message(...)` 반복으로 multipart 표현 |
| `publish(topic, message)` | `publish(topic).message(message).submit()` | topic과 payload를 한 시작점에 섞지 않음 |
| `publishToTopic(topic, msg)` | `publish(topic).message(msg).submit()` | publish는 topic이 있는 동작, builder가 payload를 단계로 분리 |
| `sendToChannel(channel, message)` | `sendToChannel(channel).message(message).submit()` | channel 대상과 payload를 builder 단계로 분리 |
| `requestToChannel(channel, parts, timeout)` | `requestToChannel(channel).message(p1).message(p2).timeout(timeout).submit()` | channel request의 payload와 timeout은 builder 단계 |
| `requestFrame(seq, parts)` | public 표면 금지 | request sequence와 frame layout은 runtime/internal helper 세부사항 |
| `dealer.reply(token, parts)` | `received.reply().message(...).submit()` 또는 router/SPOT reply | DEALER는 특정 peer routing id를 지정할 수 없어 임의 token reply가 개념적으로 맞지 않음 |
| `recvWithTimeout(timeout)` | `recv(timeout)` | 시그니처로 충분 |
| `setLingerTimeoutMilliseconds(ms)` | `setLinger(duration)` | 타입이 단위를 전달 |

송신·요청·응답·게시·Actor 표면은 `Operation Builder Policy` 에 따라 builder
시작점만 노출하고, payload·flags·timeout·callback 등 모든 변형 축은 builder
단계로 표현한다. 시작점 이름은 동작(action)만 담고 파라미터의 존재, 타입,
개수를 이름에 반복하지 않는다.

비-builder public 표면(예: snapshot, lookup, getter/setter) 에서 파라미터
조합이 다를 때 이름을 늘리는 대신 각 언어의 고유 disambiguation 메커니즘을
사용한다.

- Java / C# / C++: overloading
  - 이름은 하나, 시그니처가 구분
- Go: 가변 인자 / functional option / 별도 메서드
  - overloading이 없으므로 동작 의미가 다른 경우에만 최소 접미사를 허용한다
  - 파라미터를 그대로 이름에 넣지 않는다
- Python: keyword argument / optional parameter
  - 이름은 하나, keyword가 구분
- Node/TypeScript: optional parameter / union type
  - 이름은 하나, 타입이 구분
- Rust: trait bound / `Option<T>` / newtype
  - overloading이 없으므로 `impl Into<T>`, `Option<T>`, strong newtype으로 구분
  - 동작 의미가 다른 경우에만 최소 접미사를 허용한다
  - 파라미터를 그대로 이름에 넣지 않는다

언어별 정리:

| 언어 | disambiguation 방식 | 이름에 파라미터 인코딩 |
|---|---|---|
| Java | overloading | 금지 |
| C# | overloading | 금지 |
| C++ | overloading + strong type | 금지 |
| Go | 별도 메서드 / functional option | 금지, 동작 구분 접미사만 허용 |
| Python | keyword / optional | 금지 |
| Node/TS | optional / union | 금지 |
| Rust | trait bound / Option / newtype | 금지, 동작 구분 접미사만 허용 |

## 호환성 정책
- 호환성보다 일관된 public surface를 우선할 수 있다.
- deprecated compatibility layer는 가능한 빨리 제거한다.
- canonical path 외에 동일 기능의 우회 표면을 public 으로 함께 두지 않는다.
- flag 타입 정책:
  - public flags 노출 여부와 형태는 위 `Flags Policy` 절을 따른다.
  - .NET의 `SendFlags` / `RecvFlags` public surface는 canonical 계약이다.
  - 언어별 spec에 없는 legacy flag 타입이나 중복 flag 경로는 추가하지 않는다.

## 언어 간 정렬

### 공유 동작 계약
- blocking send/receive 계열은 실패 시 언어별 에러 경로 (exception 언어는
  예외, return-based 언어는 에러 반환)
- non-blocking receive 는 "데이터 없음"도 동일한 에러 경로로 전달
  (result code 로 구분). 별도 `try*` API 는 제공하지 않는다.
- non-blocking send 는 explicit outcome (submit result code)
- multipart-only
- typed option surface

### 언어별 반환 스타일
- C API
  - raw contract와 함수별 typed result enum
  - multipart-only 기준 surface
  - blocking API + explicit non-blocking entry (`flags` 파라미터)
- C++
  - RAII와 typed wrapper
  - multipart-only 기준 surface
  - 실패는 `throw zlink_error_t` (`SubmitResult` 코드 포함)
- .NET
  - typed option surface + `ZlinkException`
  - multipart-only 기준 surface
  - 실패는 `throw ZlinkException` (`Code` 포함)
- Java
  - domain object + `ZlinkException`
  - multipart-only 기준 surface
  - 실패는 `throw ZlinkException` (`getCode()` 포함)
- Go
  - `(T, error)` + strong type + explicit error check
  - multipart-only 기준 surface
  - 모든 실패는 `error` 반환 (`SubmitResult` 코드 포함)
- Rust
  - `Result<T, E>` + strong newtype + ownership
  - multipart-only 기준 surface
  - 단일 함수군은 `BindError` / `SubmitError` 같은 concrete error,
    다중 함수군은 `ZlinkError`
- Node/Python
  - 언어 관례를 따르되 의미 계약은 동일
  - multipart-only 기준 surface
  - 모든 실패는 `throw` / `raise` (`SubmitResult` 코드 포함)

언어별 표면은 달라도 의미 계약은 같아야 한다.

### 언어 간 Capability 표 (Target)
이 표는 `.NET` 기준으로 정리한 target 역할 표다. 이미 구현된 바인딩의 현재
public surface가 이 표와 다르면, 해당 항목은 구조 정렬 또는 breaking cleanup 작업의
목표로 해석한다. 단, `Internal-only` 항목은 target 상태에서도 public API, sample,
guide, spec signature에 노출하지 않는다.

| Area | C API | C++ | .NET | Java | Go | Rust | Node | Python |
|---|---|---|---|---|---|---|---|---|
| Multipart-only public surface | Required | Required | Required | Required | Required | Required | Required | Required |
| Blocking API named directly | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| Non-blocking receive uses flags + empty result | C raw `DONTWAIT` | Required | Required | Required | Required | Required | Required | Required |
| Non-blocking send explicit outcome | Core enum/result | Required | Required | Required | Required | Required | Required | Required |
| Public flags surface | Raw C flags | `int flags` | `SendFlags` / `RecvFlags` | `SendFlags` overload | `flags SendFlags` | `SendFlags` via `.flags(...)` builder step | `flags?: SendFlags` | keyword `flags` |
| Typed option surface | N/A raw C options | Required | Required | Required | Required | Required | Required | Required |
| Socket TLS helpers | `zlink_set_tls_*` | Required | Required | Required | Required | Required | Required | Required |
| Service TLS helpers | `zlink_set_tls_*` on service handles | Required | Required | Required | Required | Required | Required | Required |
| Socket Capability Matrix 준수 | Core 기준 | Required | Required | Required | Required | Required | Required | Required |
| `onReceive` callback | STREAM raw fn ptr | Internal-only | Internal-only | Internal-only | Internal-only | Internal-only | Internal-only | Internal-only |
| `setPacketHandler` callback registration | STREAM packet fn ptr | Required | Required | Required | Required | Required | Required | Required |
| `setDispatchHandler` callback registration | SPOT raw fn ptr | 구현 시 Required | 구현 시 Required | 구현 시 Required | 구현 시 Required | 구현 시 Required | 구현 시 Required | 구현 시 Required |
| `recvActorLifecycle` | SPOT lifecycle queue | Required | Required | Required | Required | Required | Required | Required |
| `setSendReadyHandler` callback registration | Raw fn ptr | Required | Required | Required | Required | Required | Required | Required |
| StreamSocket `connect` 차단 | N/A | Required | Required | Required | Required | Required | Required | Required |
| StreamSocket `disconnectRid` 차단 | N/A | Required | Required | Required | Required | Required | Required | Required |
| Public `detachStream` 비노출 | N/A | Required | Required | Required | Required | Required | Required | Required |
| Poller result type name | N/A | `poll_event_t` | `PollEvent` | `PollEvent` | `PollEvent` | `PollEvent` | `PollEvent` | `PollEvent` |
| Monitor typed event surface | Raw struct | Required | Required | Required | Required | Required | Required | Required |

## 테스트 정책
바인딩 테스트의 목적은 언어별 테스트 개수를 맞추는 것이 아니다. 목적은 각
바인딩이 자기 public surface에 해당하는 contract를 빠짐없이 같은 수준으로
보장하는지 확인하는 것이다.

테스트 수는 언어별 API 표면, 런타임 ownership 모델, 패키징 방식에 따라 달라질 수
있다. 따라서 테스트 개수는 판단의 보조 신호일 뿐이고, 아래 검증 계층과 Test
Matrix의 의미 계약을 충족하는지가 실제 기준이다. 특정 바인딩의 테스트가 유난히
많거나 적으면 개수 자체를 맞추기보다, 누락된 contract가 있는지 또는 core
correctness 재검증 같은 중복 테스트가 섞였는지를 먼저 확인한다.

zlink 바인딩은 native 함수를 단순 호출하는 얇은 래퍼가 아니다. 각 언어 바인딩은
public facade, helper object, domain object, typed option, callback delivery,
ownership 관리, native loader, package boundary, hot path 최적화를 함께 제공한다.
따라서 테스트도 단순 roundtrip만으로 충분하지 않다. public helper가 제공하는
추가 의미와 최적화 불변식도 binding contract의 일부로 검증해야 한다.

테스트는 아래 계층으로 분류한다.

- `Required`: 모든 바인딩이 반드시 가져야 하는 테스트다.
- `Conditional`: 해당 public API나 배포 단위를 제공하는 바인딩만 반드시 가져야
  하는 테스트다.
- `Language-specific`: 특정 런타임의 수명, 예외, GC, borrow, cgo, native loader
  같은 위험을 검증하는 테스트다. 다른 언어에 억지로 복제하지 않는다.
- `Sample smoke`: 사용자-facing 패턴이 public API로 실행되는지 확인하는 테스트다.
  core correctness를 다시 검증하는 대량 시나리오로 확장하지 않는다.
- `Out of scope`: core 자체의 메시징 correctness, transport matrix 전체 재검증,
  일회성 migration 검증, 자동화할 수 없는 리뷰 항목이다. 이런 항목은 영구
  바인딩 테스트로 남기지 않는다. 다만 바인딩 helper, facade, 최적화 불변식이
  관여하는 경로라면 core 기능과 겹쳐 보여도 바인딩 테스트로 유지한다.

공통 원칙은 아래와 같다.

- public surface test로 canonical public API를 고정한다.
- contract test로 바인딩과 native 경계의 타입 변환, 오류 매핑, handle lifecycle을
  검증한다.
- behavior test로 바인딩 public API가 core 계약을 올바르게 중계하는지 검증한다.
- helper/facade test로 바인딩이 추가로 제공하는 언어 친화 기능의 의미 계약을
  검증한다.
- ownership 테스트는 send 성공, send 실패, receive, callback, multipart 경로를
  모두 포함해야 한다.
- optimization guard test로 hot path가 정책에서 금지한 느린 경로로 퇴행하지
  않았는지 검증한다.
- callback mode와 direct mode가 함께 허용되지 않는 경로는 충돌 규칙을 검증한다.
- option 테스트는 typed option surface와 잘못된 역할 접근 차단을 함께
  검증한다.
- 성능 회귀 검증은 별도 Perf Policy가 담당한다. 기능 테스트가 perf benchmark를
  대체하거나, perf benchmark가 public contract test를 대체하면 안 된다.

테스트 충족 기준은 아래와 같다.

- 각 바인딩은 자신이 제공하는 public API에 대해 Test Matrix의 `Required` 항목을
  모두 검증해야 한다.
- 특정 public API, extension package, sample suite를 제공하면 대응하는
  `Conditional` 항목도 검증해야 한다.
- 언어 런타임 때문에 생기는 ownership, lifetime, loader, callback, GC, borrow,
  cgo 같은 위험은 `Language-specific` 테스트로 검증한다.
- 제공하지 않는 public API에 대한 테스트를 개수 맞추기 목적으로 추가하지 않는다.
- core correctness를 다시 검증하는 테스트는 바인딩 helper, facade, package
  boundary, native loader, 최적화 불변식과 직접 관련이 없으면 바인딩 테스트에서
  제거하거나 core test로 옮긴다.
- 같은 계약을 여러 테스트가 반복해서 검증하면 하나의 깊은 테스트로 합치고,
  서로 다른 계약을 한 테스트가 숨기고 있으면 Matrix 항목이 드러나도록 나눈다.

정책 변경 시 필수 테스트 규칙:

- public surface 변경: public surface test 동반
- contract 계약 변경: contract test 동반
- blocking/non-blocking 계약 변경: behavior test 동반
- ownership/receive shape 변경: callback regression 또는 ownership test 동반
- option surface 변경: typed option surface test와 negative 역할 test 동반
- codec extension 변경: 해당 codec extension test 동반
- helper/facade 변경: helper/facade contract test 동반
- hot path 구현 변경: optimization guard test 또는 perf regression gate 동반

기존 코드에 Test Matrix 바깥의 테스트가 있으면 아래 기준으로 정리한다.

- core 기능 재검증이면 core test로 옮기거나 삭제한다.
- migration 검증이면 migration 완료 후 삭제할 임시 테스트로 표시한다.
- 사용자-facing 패턴 확인이면 sample smoke로 이동한다.
- 바인딩 helper, facade, package boundary, native loader, 최적화 불변식 검증이면
  Test Matrix의 적절한 카테고리로 분류해서 유지한다.
- 특정 언어 런타임 위험을 검증한다면 Language-specific 테스트로 남기고, 이유를
  테스트 이름이나 파일 이름에서 알 수 있게 한다.

### 테스트 실행 스크립트 정책
- 각 바인딩은 전체 테스트를 한번에 실행할 수 있는 스크립트를 제공해야 한다.
- 실행 스크립트는 `bindings/<언어>/tests/` 디렉토리에 위치해야 한다.
- 스크립트는 반복 실행 가능하고 성공/실패를 요약해서 보여줘야 한다.
- 권장 형태:
  - `tests/run_tests.sh`
  - `tests/run_tests.ps1`
  - language-specific test runner entry

### 버그 발견 정책
- 테스트 또는 perf 작성/실행 중 버그를 발견한 경우 다음 절차를 따른다.
- 바인딩 라이브러리 버그:
  - 해당 바인딩에서 직접 수정한다.
  - 수정과 함께 회귀 테스트를 추가한다.
- core 라이브러리 버그:
  - 바인딩에서 core 버그를 직접 수정하지 않는다.
  - `bindings/<언어>/bug/` 디렉토리에 버그 리포트를 작성한다.
  - 리포트에는 최소한 다음을 포함한다.
    - 재현 조건 (소켓 타입, 패턴, 메시지 크기, transport 등)
    - 기대 동작
    - 실제 동작
    - 재현 코드 또는 테스트 참조
  - 바인딩 측에서 workaround가 필요하면 workaround임을 명시하고 bug 리포트를
    참조한다.

## Test Matrix
- 이 섹션은 각 바인딩이 최소한 가져야 할 테스트 항목을 정리한다.
- 바인딩별 표면은 달라도 아래 의미 계약은 모두 검증해야 한다.
- `Surface Tests`, `Contract Tests`, `Behavior Tests`, `Failure Contract Tests`,
  `Helper/Facade Tests`, `Optimization Guard Tests`, `Boundary Validation Tests`,
  `Option Tests`, `Ownership Tests`는 모든 바인딩의 기본 `Required` 항목이다.
- `Callback Tests`, `Monitor Tests`, `Poller Tests`, `Service Tests`, `Codec Tests`,
  `Sample Smoke Tests`는 해당 public API, extension package, sample suite를 제공하는
  바인딩에서 `Conditional` 항목이다.
- `Language Runtime Tests`는 런타임 특성 때문에 위험이 생기는 바인딩에서
  `Language-specific` 항목이다.

### Required: Surface 테스트
- canonical public API surface test
- socket type 역할 분리 확인
- typed option surface 존재 확인
- socket 공통 TLS helper 존재 확인
- service TLS helper 존재 확인
- raw option bag 비노출 확인
- monitor canonical surface 존재 확인
  - `recv()`

### Required: Contract 테스트
- FFI/native 호출 매핑 검증
  - 바인딩 public API 호출이 올바른 C API 함수에 매핑되는지 확인
  - 파라미터 전달과 반환값 변환이 올바른지 확인
- managed ↔ native 경계 타입 변환 검증
  - 언어 타입에서 C 타입으로의 변환이 올바른지 확인
  - C 타입에서 언어 타입으로의 변환이 올바른지 확인
- 리소스 lifecycle 검증
  - context/socket native handle 생성과 해제가 누수 없이 동작하는지 확인
  - 예외/오류 경로에서도 native 리소스가 정리되는지 확인

### Required: Behavior 테스트
- 바인딩 레이어가 core 계약을 올바르게 중계하는지 검증한다.
- 목적은 core 메시징 기능 재검증이 아니라 바인딩 경로의 정확성 확인이다.
- blocking 경로:
  - `send` → core send 중계 성공
  - `recv` → core recv 중계 성공
  - `publish` → core publish 중계 성공
  - `subscribe` → core subscribe 중계 성공
  - routed `send` → routing id 포함 중계 성공
- non-blocking 경로:
  - `recv` non-blocking → 데이터 없음 시 empty 반환
  - `subscribe` non-blocking → 데이터 없음 시 empty 반환
  - `receiveSubscriptionEvent` non-blocking → 데이터 없음 시 empty 반환
  - `send` 실패 시 예외 또는 오류 경로 확인
  - `publish` 실패 시 예외 또는 오류 경로 확인

### Required: Helper/Facade 테스트
- public helper와 facade가 단순 native 호출 이상의 의미를 제공하는 경우 그 의미를
  직접 검증한다.
- `Message`, `Received`, multipart collection, routing id value/codec, typed option
  facade, domain object, request/reply helper, topology snapshot value object 같은
  바인딩 제공 타입의 불변식을 검증한다.
- helper가 native 세부사항을 사용자에게 누출하지 않는지 확인한다.
- helper가 성공/실패, empty payload, one empty message, multipart boundary를
  구분해서 유지하는지 확인한다.
- helper가 public API에 없는 internal sequencing을 사용자에게 요구하지 않는지
  확인한다.
- convenience API가 canonical API와 다른 의미를 만들지 않는지 확인한다.

### Required: Optimization Guard 테스트
- hot path가 High-Performance Binding Policy를 계속 지키는지 검증한다.
- send/recv/request/reply/publish/subscribe 내부 경로가 `*_part` substrate를
  사용하는지 확인한다.
- aggregate native 함수 호출, 숨은 double materialization, 불필요한 eager copy,
  반복 호출마다 생기는 closure/boxing/allocation이 다시 들어오지 않았는지 확인한다.
- callback, dispatch, poller, request completion 경로에서 숨은 blocking wait,
  sleep, busy wait, thread join이 생기지 않았는지 확인한다.
- 이 검증은 항상 micro benchmark일 필요는 없다. 안정적으로 자동화할 수 있으면
  source-level/static check, public API allocation check, stress smoke, perf gate 중
  가장 낮은 비용의 방식을 사용한다.
- perf benchmark는 수치 회귀를 담당하고, optimization guard test는 금지된 구조가
  코드에 들어오지 않도록 막는 역할을 담당한다.

### Required: Failure Contract 테스트
- blocking `send` failure가 예외 또는 언어별 오류 경로로 caller에 전달되는지 확인
- blocking `publish` failure가 예외 또는 언어별 오류 경로로 caller에 전달되는지 확인
- `send` backpressure 예외 확인
- `send` not-ready 예외 확인
- `publish` backpressure 또는 not-ready 예외 확인
- native `NO_DATA` 외 오류가 무시되지 않는지 확인
- callback mode와 direct recv 충돌 시 native 계약대로 오류가 전달되는지 확인
- direct recv 불가 상태에서 empty/null로 숨기지 않는지 확인
- native `NO_DATA`만 empty/non-success 결과로 처리되는지 확인

### Required: Boundary Validation 테스트
- `RoutingId` 최대 길이 경계 (255바이트 OK)
- `RoutingId` 초과 길이 즉시 오류 반환 (256바이트 이상 → 예외)
- `Duration -> int millis` overflow 경계
- offset/length bounds 검증
- null 불가 인자 검증
- enum 범위 밖 값 검증
- `channel_name` 255바이트 초과 즉시 오류 반환 (고정 크기 `char[256]`)
- `endpoint` 255바이트 초과 즉시 오류 반환 (고정 크기 `char[256]`)
- topic/filter에 embedded null 문자 포함 시 즉시 오류 반환

### Required: Option 테스트
- common option typed getter/setter
- socket type별 typed option getter/setter
- 잘못된 소켓 타입에서 option 역할 접근 차단
- raw integer 대신 enum/boolean surface가 제공되는지 확인

### Required: Ownership 테스트
- send 성공 시 ownership 이동 계약 (native에 넘어감, 바인딩이 이후 접근 금지)
- send 실패 시 restore 또는 caller ownership 유지 계약
- 생성 후 send하지 않은 메시지의 명시적 close/해제 (close 없으면 native 메모리 누수)
- recv 결과 ownership 계약 (바인딩이 받아서 해제 책임)
- callback 후 frame validity 계약
- multipart receive shape와 callback delivery shape 일치 여부

### Conditional: Callback 테스트
- public callback API가 있는 경우 callback delivery를 검증한다.
- callback이 받은 message 또는 multipart payload의 ownership을 검증한다.
- callback 예외, panic, rejected promise, delegate exception 같은 언어별 실패가
  문서화된 오류 경로로 전달되는지 확인한다.
- callback delegate/function/object lifetime이 native callback보다 짧아져
  use-after-free를 만들지 않는지 검증한다.
- callback 안에서 금지된 blocking wait나 hidden thread join이 발생하지 않는지
  확인한다.

### Conditional: Monitor 테스트
- blocking monitor `recv` 성공 경로
- non-blocking monitor recv empty path
- monitor callback/state 변화와 data plane readiness 일치 여부

### Conditional: Poller 테스트
- raw socket readiness 또는 fd readiness가 public poller API로 전달되는지 확인한다.
- poller가 지원하지 않는 service-specific handle을 조용히 받아들이지 않는지 확인한다.
- readiness event 값은 data plane contract를 대체하지 않는다는 점을 검증한다.

### Conditional: Service 테스트
- spot/actor public API를 제공하는 바인딩은 해당 service lifecycle을 최소 경로로
  검증한다.
- close/connect/unbind 같은 lifecycle 제약이 public API에서 native 계약대로
  전달되는지 확인한다.
- spot publish/subscribe, spot request/reply, SPOT status/snapshot은 public
  surface가 있으면 roundtrip 또는 snapshot 검증을 수행한다.
- service test는 service layer 바인딩 계약 검증이 목적이다. core service 전체
  matrix를 모든 언어에서 다시 실행하지 않는다.

### Conditional: Codec 테스트
- codec extension package를 제공하는 바인딩은 codec별 payload roundtrip을 검증한다.
- core binding package가 codec dependency를 필수로 끌어들이지 않는지 확인한다.
- serializer 선택 규칙이 있는 언어는 기본 serializer와 오류 경로를 검증한다.

### Conditional: Sample Smoke 테스트
- sample suite를 제공하는 바인딩은 canonical sample set의 실행 smoke를 제공한다.
- sample smoke는 public API 사용 가능성을 확인하는 최소 검증이다.
- sample smoke는 core transport matrix, stress, perf 측정을 대신하지 않는다.

### Language-specific: Runtime 테스트
- .NET: `IDisposable`, `SafeHandle`, delegate lifetime, `GCHandle`, native library
  loader, `ZlinkException` mapping을 검증한다.
- Java: `AutoCloseable`, JNI object lifetime, checked/unchecked exception policy,
  classloader/native loader 경계를 검증한다.
- Go: cgo pointer rule, finalizer에 의존하지 않는 explicit close, `(T, error)`
  mapping을 검증한다.
- Rust: ownership move, borrow lifetime, `Drop`, `Send`/`Sync` 노출 여부, concrete
  error type mapping을 검증한다.
- Python: buffer protocol, reference counting, context manager, exception mapping을
  검증한다.
- Node: native addon lifetime, `Buffer` ownership, async callback error path,
  package export boundary를 검증한다.
- C++: RAII, move-only message ownership, exception type, installed header boundary를
  검증한다.
- C: raw ABI, errno/result code, caller-provided message lifecycle을 검증한다.

### 참고: Performance and Sample Verification
- 성능 회귀 검증은 Perf Policy (`doc/perf/`)가 담당한다. Test Matrix에 중복하지
  않는다.
- sample/helper의 canonical API 준수, send 실패 무시 방지, legacy surface
  우회 방지는 Review Checklist에서 검증한다. 자동화 테스트 항목이 아니다.

## 샘플 정책
- 샘플 제작 규칙은 [`doc/spec/sample/SAMPLE_POLICY.md`](https://kairos-code-dev.github.io/zlink/en/spec/sample/SAMPLE_POLICY/)
  를 단일 기준 문서로 사용한다.
- 이 문서는 `core/samples/`와 `bindings/*/samples/`를 함께 포괄한다.
- 바인딩 샘플을 추가, 수정, 리뷰할 때는 위 문서를 기준으로 판단한다.

## Perf 정책

perf 코드는 데모가 아니라 바인딩 라이브러리의 성능을 측정하고 개선하기 위한
코드다. perf 의 1차 목적은 바인딩 레이어의 비용을 드러내고, 병목과 회귀를
식별하고, 개선 작업의 전후 차이를 측정하는 것이다.

**perf 정책의 단일 기준은 `doc/perf/` 정책 문서다.** CLI 옵션, 기본값, 출력
포맷, RESULT line 형식, 패턴/transport matrix, phase 규칙, 결과 저장, 실패
처리, 환경 변수 등 모든 세부 규격은 아래 문서를 따른다. 본 섹션에서 중복
정의하지 않는다.

- [`doc/perf/PERF_POLICY.md`](../../../doc/perf/PERF_POLICY.md) — 공통 perf 정책
  (공통 원칙, 디렉터리 구조, RESULT 형식, 결과 저장, 출력 형식, 실패 처리,
  환경 변수, 리팩토링 원칙, 언어별 적용 범위)
- [`doc/perf/PERF_SINGLE_TEST_POLICY.md`](../../../doc/perf/PERF_SINGLE_TEST_POLICY.md) — single suite 정책
- [`doc/perf/PERF_MULTI_TEST_POLICY.md`](../../../doc/perf/PERF_MULTI_TEST_POLICY.md) — multi suite 정책

### 바인딩 perf 원칙

- perf 코드는 `doc/perf` 정책을 준수한다.
- `core/perf` 에서 제공하는 패턴과 시나리오를 기준으로 한다.
- core perf와 비교 가능한 시나리오를 유지하면서, 각 언어 스타일에 맞게 작성한다.
- 측정 anchor point, phase 의미, metric 집합, RESULT line 의미를 바꾸지 않는다.
- perf 정책은 성능 측정 surface를 공식 제공하는 바인딩에서는 `Required`다.
  perf 코드를 아직 제공하지 않는 바인딩에는 `Target`으로 본다.

### 바인딩 API Spec 문서

각 바인딩의 API surface는 아래 문서를 참조한다.
perf 정책은 [`doc/perf/PERF_POLICY.md`](../../../doc/perf/PERF_POLICY.md)에서 전 언어 공통으로 관리한다.

| 바인딩 | API Spec |
|--------|----------|
| C | [`c/README.md`](c/README.en.md) |
| C++ | [`cpp/README.md`](cpp/README.en.md) |
| Java | [`java/README.md`](java/README.en.md) |
| .NET | [`dotnet/README.md`](dotnet/README.en.md) |
| Node.js | [`node/README.md`](node/README.en.md) |
| Python | [`python/README.md`](python/README.en.md) |
| Go | [`go/README.md`](go/README.en.md) |
| Rust | [`rust/README.md`](rust/README.en.md) |

### Perf 리뷰 체크리스트

- 이 perf 가 바인딩 라이브러리 비용을 측정하고 있는가
- 핵심 send/recv/callback 경로가 perf 파일 본문에서 직접 읽히는가
- 각 패턴이 별도 파일로 분리되어 있는가
- `core/perf` 패턴과 정렬되어 있는가
- `doc/perf` 정책을 준수하는가

## 스크립트 위치 정책
- 실행 스크립트는 실행 대상과 같은 디렉토리에 위치한다.
- 바인딩 루트가 아니라 각 하위 디렉토리에 둔다.

| 용도 | 위치 | 스크립트 예시 |
|------|------|---------------|
| 테스트 | `bindings/<언어>/tests/` | `run_tests.sh` |
| 샘플 | `bindings/<언어>/samples/` | `run_samples.sh` |
| perf | `bindings/<언어>/perf/` | `run_benchmarks.sh`, `run_benchmarks_multi.sh` |

- Windows 지원이 필요한 경우 `.ps1` 도 함께 제공한다.
- 바인딩 루트(`bindings/<언어>/`)에 `run_samples.sh` 같은 wrapper를
  두지 않는다. 이 위치의 wrapper는 `samples/run_samples.sh`와 중복되고,
  어느 것이 정답인지 혼선을 만든다.
- CI나 전체 검증을 위해 테스트+샘플+perf를 한번에 실행하는 orchestration
  스크립트가 필요하면 `bindings/<언어>/run_all.sh` 같은 이름으로 둘 수 있다.
  이 스크립트는 개별 `tests/run_tests.sh`, `samples/run_samples.sh` 등을
  호출하는 진입점이며, 개별 스크립트를 대체하지 않는다.

## 리뷰 체크리스트
- public API가 multipart-only인가
- blocking/non-blocking이 별도 이름으로 분리되지 않았는가
- 언어별 `Flags Policy`에 없는 public flag type 또는 중복 flag 경로가
  남아 있지 않은가
- raw option bag이 public에 남아 있지 않은가
- option 값이 enum/boolean/value object로 승격되었는가
- 타입별 역할이 제대로 닫혀 있는가
- blocking send 실패가 예외 또는 오류 경로로 반드시 caller에 전달되는가
- `send` 실패가 backpressure/not-ready를 포함해 모든 오류를 예외로 전달하는가
- binding이 truncation/overflow를 선검증하는가
- native 상태 오류를 바인딩이 임의로 추론하지 않는가
- public surface test와 behavior test가 같이 있는가
- 값 객체 검증과 호출 직전 검증의 책임 위치가 설명 가능한가
- 언어별 `Flags Policy`에 없는 legacy flag 타입이 public contract에서 제거되었는가
- sample code가 canonical API만 사용하는가
- helper가 blocking send 실패를 무시하지 않는가
- helper가 deprecated/legacy surface를 우회 호출하지 않는가

## POSD 기반 구현 완성 정책
- 이 섹션은 바인딩 구현을 완성하고 리팩터링할 때 적용하는 POSD 기반 절차를
  정의한다.
- 바인딩은 기능 나열이 아니라 구조적 정확성을 기준으로 완성한다.
- 완성 기준은 Socket Capability Matrix, Callback API Policy, Option Policy,
  Test Matrix, Sample Policy다.
- 리팩터링은 코드를 이동하는 것이 아니라 시스템 복잡도를 줄이는 것이다.

### 완성 순서
- 바인딩 구현은 아래 순서를 따른다.
- 각 단계는 이전 단계의 결과에 의존한다.
- 한 단계를 건너뛰고 다음 단계를 진행하지 않는다.

#### 1단계: Capability Matrix 정렬
- Socket Capability Matrix를 기준으로 각 소켓 타입의 public API를 검토한다.
- 있어야 하는데 없는 API를 추가한다.
- 있으면 안 되는데 노출된 API를 제거하거나 internal로 이동한다.
- 검증: surface test가 matrix와 일치해야 한다.
- 대표 위반 예:
  - StreamSocket에 `connect()` 노출 → 제거
  - StreamSocket에 `disconnectRid()` 노출 → 제거
  - StreamSocket에 `detachStream()` 노출 → 제거
  - Node에 `setSendReadyHandler` 없음 → 추가
  - 잘못된 소켓에 publish/subscribe 노출 → 제거

#### 2단계: 이름 정규화
- Naming Policy와 Callback API Policy 기준으로 canonical 이름을 맞춘다.
- 이름만 다르고 의미가 같은 API는 canonical 이름으로 통일한다.
- deprecated alias는 제거한다.
- 검증: surface test에서 canonical 이름 존재를 확인한다.
- 대표 위반 예:
  - public `recvHandler` / `onReceive` → 제거하거나 internal raw STREAM bridge로 이동
  - `spotDispatchHandler` → `setDispatchHandler`
  - `on_topic_message` → `subscribe`

#### 3단계: 깊은 모듈 구조
- POSD deep module 원칙에 따라 public 타입의 깊이를 확보한다.
- 각 public 타입이 단순 pass-through가 아니라 내부에서 검증, ownership,
  shape 규칙을 캡슐화하는지 확인한다.
- 얕은 래퍼 판별 기준:
  - native 함수를 1:1로 감싸기만 하고 새 의미를 추가하지 않는가
  - 호출자가 native 계약(시퀀스, 크기, 인코딩)을 알아야 사용할 수 있는가
  - 동일 규칙이 여러 소켓 타입에 중복 구현되어 있는가
- 얕은 래퍼를 발견하면:
  - 검증을 값 객체 또는 facade 내부로 이동한다
  - 중복 규칙을 한 모듈에 모은다
  - pass-through만 하는 public 타입은 제거하거나 internal에 병합한다
- 대표 위반 예:
  - RoutingId 길이 검증이 각 소켓 타입마다 중복 → RoutingId 값 객체 하나로 모은다
  - monitor event가 raw int → typed event surface로 승격한다
  - option value가 raw int → enum/boolean/Duration으로 승격한다

#### 4단계: 변경 파급 제거
- 같은 규칙이 여러 곳에 흩어진 지점을 찾아서 한 모듈에 모은다.
- 판별 기준:
  - 정책 하나가 바뀌면 2개 이상의 파일을 고쳐야 하는가
  - 새 소켓 타입을 추가할 때 기존 코드를 N곳 수정해야 하는가
- 대표 위반 예:
  - send failure contract 규칙이 소켓 타입마다 별도 구현
  - blocking/non-blocking 분기가 소켓 타입마다 별도 구현
  - option validation이 각 option setter마다 별도 구현

#### 5단계: 정보 은닉 강화
- public API가 native 세부사항을 노출하는 지점을 찾아서 facade 뒤로 숨긴다.
- 판별 기준:
  - 사용자가 errno, flag 상수, native struct 크기를 알아야 하는가
  - 사용자가 internal sequencing(호출 순서)을 기억해야 하는가
  - public API에 native handle, raw pointer, raw buffer가 노출되는가
- 대표 위반 예:
  - raw `setSockOptRaw` / `setOption(int, byte[])` 가 public
  - monitor event에 raw int mask가 그대로 노출
  - 언어별 `Flags Policy`에 없는 legacy flag 타입이 public 타입으로 남아 있음

#### 6단계: 테스트 Matrix 완성
- Test Matrix의 `Required` 카테고리는 모든 바인딩에서 작성하거나 보강한다.
- 해당 public API, extension package, sample suite를 제공하는 바인딩은 관련
  `Conditional` 카테고리도 작성하거나 보강한다.
- 언어 런타임의 수명, 예외, native loader 위험이 있는 바인딩은 관련
  `Language-specific` 카테고리를 작성하거나 보강한다.
- 완성 기준:
  - Surface test가 Socket Capability Matrix를 검증한다
  - Contract test가 FFI 매핑과 lifecycle을 검증한다
  - Behavior test가 blocking/non-blocking 경로를 검증한다
  - Helper/Facade test가 바인딩 제공 helper의 의미 계약을 검증한다
  - Optimization Guard test가 hot path 최적화 불변식을 검증한다
  - Failure Contract test가 send/receive 오류 계약을 검증한다
  - Boundary test가 값 경계를 검증한다
  - Option test가 typed surface를 검증한다
  - Ownership test가 send/recv ownership을 검증한다
  - 해당 public API가 있으면 Callback, Monitor, Poller, Service, Codec test가
    public contract를 검증한다
  - sample suite가 있으면 Sample Smoke test가 canonical API 실행을 검증한다

#### 7단계: 샘플 정렬
- Canonical Sample Set 기준으로 샘플을 완성한다.
- 각 샘플이 canonical API만 사용하는지 확인한다.
- 1-5단계에서 이름이나 API가 바뀌었다면 샘플도 같이 갱신한다.

### 리팩터링 판단 기준
- 다음 질문에 "예"이면 리팩터링이 필요한 지점이다.
  - 이 public 타입을 제거하면 사용자가 잃는 것이 없는가 → 얕은 래퍼
  - 이 규칙을 고치면 3개 이상의 파일을 건드려야 하는가 → 변경 파급
  - 사용자가 이 API를 쓰려면 다른 API의 내부 동작을 알아야 하는가 → 정보 누출
  - 같은 능력이 2개 이상의 이름으로 노출되는가 → 중복 surface
  - 사용자가 호출 순서를 기억해야 올바르게 동작하는가 → 시간 순서 의존

### 리팩터링 종료 조건
- 리팩터링은 아래 조건이 모두 충족될 때까지 반복한다.
- 하나라도 남아 있으면 완료가 아니다.
- 판단은 POSD 관점에서 수행한다.
- 종료 조건의 범위는 해당 바인딩이 구현하기로 한 scope에 한정한다.
  - `Required` 항목: 모든 바인딩에 적용
  - `Conditional` 항목: 해당 public API, extension package, sample suite를
    제공하는 바인딩에 적용
  - `Language-specific` 항목: 해당 런타임 위험이 있는 바인딩에 적용
  - `Recommended` 항목(예: 샘플): 공개 배포 바인딩에 적용

1. **Capability Matrix 완전 정렬**
   - Socket Capability Matrix의 모든 `Y` 항목이 public API에 존재한다.
   - Socket Capability Matrix의 모든 `—` 항목이 public API에 노출되지 않는다.
   - 해당 바인딩이 구현하는 서비스 계층 컴포넌트의 Capability Matrix도
     동일하게 정렬한다.
     바인딩이 구현하지 않으면 종료 조건에서 제외한다.
   - Surface test가 이를 검증하고 통과한다.

2. **이름 정규화 완료**
   - 모든 public API가 Naming Policy의 canonical 이름을 사용한다.
   - deprecated alias가 남아 있지 않다.
   - Callback API Policy의 canonical 이름(`setPacketHandler`,
     `setDispatchHandler`, `setSendReadyHandler`)이
     해당 역할에 맞게 존재한다.

3. **얕은 래퍼 제거**
   - native 함수를 1:1로 감싸기만 하는 public 타입이 없다.
   - 모든 public 타입이 검증, ownership, shape 규칙 중 하나 이상을 캡슐화한다.
   - `RecvPart`, `RecvRoutedPart`, `SubscribePart` 또는 언어별 동등 이름이
     public API에 없다. part 단위 수신은 runtime/internal substrate로만 존재한다.
   - `requestFrame(...)`처럼 protocol envelope을 그대로 드러내는 helper가 public
     표면에 없다.
   - `dealer.reply(requestToken, parts)`처럼 DEALER의 송신 능력과 맞지 않는 reply
     helper가 public 표면에 없다.

4. **변경 파급 해소**
   - 동일 규칙이 2개 이상의 모듈에 중복 구현되어 있지 않다.
   - 정책 변경 시 수정해야 할 파일이 1개다.

5. **정보 은닉 확보**
   - public API에 raw option bag, 정책 밖 legacy/raw flag, raw native struct,
     raw errno가 노출되지 않는다.
   - 사용자가 internal sequencing을 알지 않아도 API를 올바르게 사용할 수 있다.

6. **Test Matrix 완성**
   - 모든 `Required` 테스트가 존재하고 통과한다.
   - 해당 바인딩의 scope에 포함되는 `Conditional` 테스트가 존재하고 통과한다.
   - 해당 런타임 위험에 필요한 `Language-specific` 테스트가 존재하고 통과한다.

7. **Sample 정렬 완료**
   - Canonical Sample Set의 모든 샘플이 존재한다.
   - 해당 바인딩이 구현하는 서비스 계층 샘플도 포함한다.
   - 구현하지 않는 `Target` 컴포넌트의 샘플은 제외한다.
   - 모든 샘플이 canonical API만 사용한다.
   - deprecated/legacy 경로를 사용하는 샘플이 없다.

8. **Dead code 제거 완료**
   - 리팩터링 과정에서 발생한 모든 불필요한 코드가 제거되었다.
   - deprecated alias, legacy wrapper, 사용되지 않는 import/using/require가
     남아 있지 않다.
   - Capability Matrix에서 `—`로 표시된 API의 구현 코드가 internal에도 불필요하게
     남아 있지 않다.
   - 이름 정규화로 교체된 옛 이름의 함수/메서드/타입이 남아 있지 않다.
   - 호출되지 않는 private/internal helper가 남아 있지 않다.
   - 참조되지 않는 상수, enum 값, 타입 alias가 남아 있지 않다.
   - 주석으로 처리된 코드 블록(`// removed`, `// deprecated`, `// remove later`)이
     남아 있지 않다.
   - 빈 파일, 빈 클래스, 빈 모듈이 남아 있지 않다.
   - dead code는 "나중에 쓸 수 있으니까" 남겨 두지 않는다. 필요하면 git
     history에서 복원한다.

### 리팩터링 반복 규칙
- 1-7단계를 한 번 수행한 뒤, 종료 조건을 다시 점검한다.
- 앞 단계의 변경이 뒤 단계에 영향을 줄 수 있으므로, 종료 조건이 하나라도
  미충족이면 해당 단계부터 다시 수행한다.
- 종료 조건 8개가 모두 충족될 때까지 반복한다.
- "더 고칠 곳이 보이지 않는다"가 아니라 "종료 조건 8개가 모두 통과한다"가
  완료 기준이다.

### 리팩터링 금지 사항
- 구조 개선을 이유로 의미 계약을 바꾸면 안 된다.
- 내부 리팩터링으로 public API의 시그니처가 달라지면 안 된다.
  - 시그니처가 달라져야 하면 그것은 API 변경이지 리팩터링이 아니다.
- 성능 개선을 이유로 correctness를 타협하면 안 된다.
- "나중에 쓸 수 있으니까" 미리 추상화를 만들면 안 된다.
- 한 번만 쓰이는 코드를 utility/helper로 빼면 안 된다.

## 구현 리뷰 체크리스트
- 이 섹션은 public API 정책을 구현에 반영했는지 확인하는 리뷰 체크리스트다.
- 아래 항목은 새로운 public API 제안이 아니다. 이미 정의된 계약과 경계 규칙을
  구현, 테스트, 샘플이 지키는지 확인하는 기준이다.
- 항목은 바인딩별 리뷰와 리팩터링 작업의 기본 체크리스트로 사용한다.

### 공개 vs 내부 경계 후속 작업

- Java:
  - public package에 남아 있는 internal 성격 타입(`SocketCore`,
    `MessagePlane`, request/reply support helper 등)을 internal package 또는
    implementation package로 이동해야 한다.
  - JPMS를 사용한다면 documented public package만 export 하도록 정리해야 한다.
- .NET:
  - `InternalsVisibleTo`는 test 지원 범위로만 제한해야 한다.
  - perf 프로젝트가 internal surface에 접근하지 않도록 assembly visibility를
    다시 닫아야 한다.
- C:
  - helper substrate와 public C binding header가 실제로 분리되면,
    `core/include/zlink.h` 중심 설명을 public C binding header 기준으로 다시
    정리해야 한다.
  - 설치되는 public header와 private substrate header의 경계를 문서와 패키징에
    함께 반영해야 한다.

### 값 검증 후속 작업
- `RoutingId`
  - 값 객체 생성 시 길이 상한 검증
  - raw 경로가 남아 있다면 native 호출 직전 재검증
- `Duration` 기반 옵션
  - `int millis` 변환 overflow 검증
  - 음수 허용/비허용 계약 명시
- topic/filter/string identifier
  - 고정 크기 output buffer 경로의 재할당 정책 점검
  - truncation 없이 전체 문자열을 처리하는지 점검
- offset/length 기반 byte API
  - bounds 검증 일관화
- enum wrapper가 없는 raw 정수 옵션
  - enum 또는 boolean 승격 후보 조사

### 공개 표면 후속 작업
- legacy flag 타입
  - 언어별 `Flags Policy`에 없는 public flag type 또는 중복 flag 경로 제거 여부 재확인
  - 필요한 경우 internal 이동 여부 결정
- monitor plane
  - `recv()` canonical surface 유지 여부 확인
- callback API
  - callback payload shape가 direct receive shape와 동일한지 재확인
- 단일 메시지 편의 메서드
  - public receive/subscribe 편의 오버로드 잔존 여부 점검

### 옵션 표면 후속 작업
- raw option bag 잔존 여부 조사
- socket type별 option 역할 누수 여부 조사
- option value가 아직 `int`에 머무는 항목 목록화
- context option도 같은 기준으로 typed facade 적용 여부 검토

### 오류 계약 후속 작업
- binding validation 예외와 native 예외가 혼재된 경로 조사
- 바인딩이 errno를 임의로 해석하는 경로 조사
- native `NO_DATA` 외 오류를 잘못 empty/bool 경로로 숨기는 코드 조사
- blocking send 실패를 무시하는 helper/sample 조사

### 성능 후속 작업
- hot path send/recv 경로의 숨은 복사 조사
- `Message`, `Received`, `TopicMessage` 생성 과정의 불필요한 컬렉션/배열
  할당 조사
- callback path와 direct path 비용 차이 조사
- string/topic/routing-id 변환의 인코딩/디코딩 비용 조사
- sample과 helper가 느린 대체 경로를 기본 사용법처럼 노출하는지 조사

### POSD 후속 작업
- 얕은 래퍼만 제공하는 public 타입 조사
- 한 규칙이 여러 모듈에 흩어진 변경 파급 지점 조사
- 사용자가 internal sequencing을 알아야 하는 temporal API 조사
- facade 뒤로 숨길 수 있는 raw/native 개념 누수 지점 조사

### 소유권과 콜백 후속 작업
- send failure restore 경로와 consume 경로가 문서와 일치하는지 점검
- callback 후 frame validity 계약 재검증
- callback mode와 direct recv 충돌 시 native 계약대로 오류가 전달되는지 점검

### 테스트 후속 작업
- public surface 변경마다 public surface test 존재 여부 확인
- value boundary 검증 테스트 추가
  - 예: `RoutingId` 최대 길이
  - 예: `Duration` overflow
- option negative 역할 테스트 보강
- ownership/callback regression 유지 여부 확인

## 바인딩 요구사항

| Binding | 언어 버전 | 런타임/프레임워크 | 빌드 툴 |
|---------|-----------|-------------------|---------|
| C++ | C++20 | — | CMake 3.10+ |
| .NET | C# 12 | .NET 8.0 | MSBuild |
| Java | Java 22 | JDK 22 | Gradle 8.10.2 |
| Go | Go 1.22+ | — | Go modules |
| Rust | Rust 2024 edition | MSRV 1.85+ | Cargo |
| Node | TypeScript 5.8 | Node 22+ | npm |
| Python | Python 3.9 | CPython 3.9+ | setuptools 68+ |
- 각 바인딩의 정확한 버전은 해당 프로젝트 설정 파일이 기준이다.
  - C++: `CMakeLists.txt`
  - .NET: `Zlink.csproj` (`PackageId` / `RootNamespace`: `Systems.Zlink`)
  - Java: `build.gradle`, `gradle-wrapper.properties`
  - Go: `go.mod`
  - Node: `package.json`, `tsconfig.json`
  - Python: `pyproject.toml`

## API 레퍼런스

각 바인딩은 해당 언어의 표준 문서 도구로 API 레퍼런스를 생성한다.

| Binding | 문서 도구 | 생성 명령 | 출력 위치 |
|---------|-----------|-----------|-----------|
| C++ | Doxygen | `doxygen Doxyfile` | `cpp/doxygen/html/` |
| Java | Javadoc (Gradle) | `./gradlew javadoc` | `java/build/docs/javadoc/` |
| Python | Sphinx + autodoc | `sphinx-build -b html docs docs/_build/html` | `python/docs/_build/html/` |
| Node | TypeDoc | `npx typedoc` | `node/typedoc/html/` |
| .NET | DocFX | `docfx docfx.json` | `dotnet/_site/` |
| Go | godoc / pkgsite | `go doc ./...` | (동적 서버) |
| Rust | rustdoc | `cargo doc --no-deps` | `rust/target/doc/zlink/` |

- 생성 명령은 각 바인딩 디렉터리에서 실행한다.
- 출력 디렉터리는 `.gitignore`로 추적에서 제외한다.
- 각 바인딩의 `README.*.md` 파일에 상세 생성 절차와 스코프가 명시되어 있다.

## Routing ID로 Peer 끊기

- 모든 바인딩은 connectable raw socket 타입에 대해 core의 peer-rid disconnect 표면을 노출한다.
- raw socket API는 `zlink_disconnect_rid()`에, SpotNode API는 `zlink_spot_node_disconnect_peer_rid()`에 매핑한다.
- `StreamSocket`은 bind-only이며 peer-rid disconnect를 노출하지 않는다.
- Spot facade 타입도 별도의 peer-rid disconnect 메서드를 노출하지 않는다. peer mesh 소유권은 SpotNode에 있기 때문이다.

| Language | Raw socket name | SpotNode name |
|---|---|---|
| C | `zlink_disconnect_rid` | `zlink_spot_node_disconnect_peer_rid` |
| C++ | `disconnect_rid` | `disconnect_peer_rid` |
| Python | `disconnect_rid` | `disconnect_peer_rid` |
| Node | `disconnectRid` | `disconnectPeerRid` |
| Go | `DisconnectRID` | `DisconnectPeerRID` |
| Rust | `disconnect_rid` | `disconnect_peer_rid` |
| Java | `disconnectRid` | `disconnectPeerRid` |
| .NET | `DisconnectRid` | `DisconnectPeerRid` |

바인딩은 `ZLINK_OPT_RID_DUPLICATE_POLICY`, `ZLINK_RID_DUPLICATE_REJECT`,
`ZLINK_RID_DUPLICATE_HANDOVER`, 그리고 connect 결과 값 `NOT_FOUND`, `CONFLICT`,
`BUSY` 를 각 언어의 일반적인 enum/오류 매핑 스타일로 노출해야 한다.

- C 바인딩은 native socket option contract를 통해 `ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES` 값 `0x3034` 를, context option contract를 통해 `ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES` 값 `18` 을 노출한다.
- 상위 바인딩은 이 기능을 typed context option facade로 노출해야 한다.
- socket, SpotNode, Spot 의 public facade는 메시지 단위 옵션을 추가하지 않는다.
- 호환성을 위해 raw socket 경로를 남겨 둔다면 canonical API와 명확히 분리하고, 새 문서·샘플·테스트에서는 사용하지 않으며, C 계약(`int` bytes, raw 기본값 `0`, 음수 값은 `EINVAL` 로 실패)을 그대로 유지해야 한다.

## 관련 문서
- `bindings/cpp/`
- `bindings/dotnet/`
- `bindings/java/`
- `bindings/go/`
- `bindings/rust/`
- `bindings/node/`
- `bindings/python/`

## Core API Surface 6.0.0 정렬

- Actor create와 join payload는 aggregate multipart payload를 사용한다.
- 공개 바인딩 API는 remote actor create, actor join, actor join receive, actor join reply에 대해 메시지 컬렉션을 받는다.
- 단일 메시지 편의 경로는 해당 언어 README가 그 편의 표면을 명시적으로 유지하기로 한 경우에만 허용한다. 그렇지 않으면 breaking alignment 과정에서 canonical multipart 경로 쪽으로 정리하면서 제거한다.
- 유지하는 경우에도 내부적으로는 multipart 경로를 호출해야 하며, empty payload와 비어 있는 메시지 하나가 계속 구분될 수 있어야 한다.
- admission handler는 callback 동안만 유효한 borrowed payload view를 받는다.

Public Registry scalar 설정은 core 8.4.3에서 공개 Discovery/Registry C API와 함께
제거되었다. 바인딩은 registry option 표면, 이름 있는 registry setter,
compatibility alias를 현재 공개 API로 유지하면 안 된다.

## Spot Route Bridge API

- 바인딩은 `SpotNode`가 channel socket을 소유하지 않도록 `SpotRouteBridge` 또는 같은 의미의 typed handle을 노출해야 한다.
- bridge는 caller/channel runtime이 소유한 `ROUTER` socket을 참조하고, Spot route packet을 보내거나 channel receive loop에서 받은 SPOT relay packet을 SpotNode로 넘긴다.
- bridge를 닫아도 등록된 channel socket은 닫히지 않는다.

언어별 API는 다음 의미를 빠뜨리지 않아야 한다.

- `createRouteBridge(options)` 또는 동등한 생성자
- `attachRouterChannel(channelName, routerSocket)`
- `sendToSpot(targetNode, targetSpot, parts)`
- `requestToSpot(targetNode, targetSpot, parts, replyHandler, timeout)`
- `handleRouterReceived(channelName, received)`
- `close` 또는 `dispose`

`timeout == 0`은 bridge 기본 timeout을 사용한다. `handleRouterReceived`가 handled
결과를 반환하면 바인딩은 payload 소유권이 bridge로 넘어갔음을 호출자에게 분명히 표현해야
한다.

SpotNode에 router channel peer를 직접 붙이는 예전 C API는 공개 계약에 없다.
framework adapter는 그 경로를 새 구현에 사용하면 안 된다.
