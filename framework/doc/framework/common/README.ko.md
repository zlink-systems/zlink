<!-- framework-adapter-nav:start -->
[가이드 홈](../index.ko.md) | [이전: ZLink Framework](../index.ko.md) | [다음: ZLink Framework Overview](spec/02-overview.ko.md)
<!-- framework-adapter-nav:end -->

[가이드 홈](../index.ko.md)

[스펙 목차](spec/README.ko.md) | [공개 계약 관리](spec/00-public-contract-governance.ko.md) | [공통 내부 구조](internals/README.ko.md) | [개요](spec/02-overview.ko.md) | [상호작용 모델](spec/03-interaction-model.ko.md) | [메시지 모델](spec/04-message-model.ko.md) | [channel topology](spec/07-channel-topology.ko.md) | [framework API](spec/06-framework-api.ko.md) | [비동기 실행](spec/05-async-execution-policy.ko.md) | [Actor 모델](spec/14-actor-model.ko.md) | [Spot Actor Join / Relocation](spec/15-spot-actor.ko.md) | [Session Actor Dispatch 사용성](spec/20-session-actor-dispatch.ko.md) | [메시지 흐름 추적](spec/26-message-flow-tracing.ko.md) | [location runtime](spec/21-location-runtime.ko.md) | [Redis store](spec/22-location-store-redis.ko.md) | [spot 주소 메시징](spec/16-spot-address-messaging.ko.md) | [언어별 공개 계약](spec/server/languages/README.ko.md) | [Sample/E2E 설정 정책](sample-e2e-configuration-policy.ko.md) | [공통 샘플](sample/README.ko.md) | [Scenario E2E](e2e/README.ko.md) | [Performance 테스트](perf/README.ko.md)

# ZLink Framework 공통 스펙

> 이 문서 묶음은 언어 중립 **공통 스펙**이다. 여기서 정의한 의미는 언어별
> 문서가 재정의하지 않고 각 언어 표면으로만 구체화한다.

## 1. 목적

이 묶음은 zlink의 `.NET`, `Java`, `Kotlin`, `Node.js`, `C++` 바인딩 위에
`ASP.NET Core`, `Spring Boot`, `NestJS`, C++ zlink framework host를 사용하는 개발자를 위한
`ZLink Framework` 방향을 정리한다.
제품 개요와 핵심 가치는 [01-overview.ko.md](spec/02-overview.ko.md)를 참고한다.

## 2. 버전 기준

지원 언어와 런타임 버전은 binding 문서가 먼저 명시해야 한다. 특히 `.NET`
문서에서는 아래 기준을 공통으로 적용한다.

- 최소 지원 런타임은 `.NET 8` (`net8.0`)이다.
- 주 개발 기준은 `.NET 10` (`net10.0`)이다.
- 최소 지원 C# 언어 버전은 `C# 12`다.
- 문서와 샘플은 최소 지원 버전에서 성립하지 않는 `preview`, `latest`,
  `C# 13`, `C# 14` 전용 문법을 전제로 쓰지 않는다.

바인딩 구현과 샘플이 더 높은 런타임에서 함께 개발되더라도, 공개 framework
계약은 먼저 "어디까지를 최소 지원으로 볼 것인가"를 분명히 적어야 한다.

## 3. 문서 구성

아래 문서들은 각각 한 가지 주제만 다루며, 서로 범위가 겹치지 않게 구성했다.
번호 순서대로 읽으면 전체 그림을 자연스럽게 따라갈 수 있다.

| 순서 | 문서 | 다루는 범위 |
|:----:|------|------------|
| 1 | [01-overview.ko.md](spec/02-overview.ko.md) | 제품 개요, 핵심 차별점, 현재 우선 범위. "ZLink Framework가 무엇이고, 왜 필요한가"에 답한다. |
| 2 | [02-interaction-model.ko.md](spec/03-interaction-model.ko.md) | 사용자에게 보이는 request-response, command, publish-subscribe, stream 모델의 의미를 정의한다. |
| 3 | [03-message-model.ko.md](spec/04-message-model.ko.md) | 서버 간 multipart `header + payload` 메시지 구조, STREAM 단일 packet 경계, header 필드, payload codec 방향과 codec extension 정책을 다룬다. |
| 4 | [10-channel-topology.ko.md](spec/07-channel-topology.ko.md) | channel grouping, Discovery, 수동 연결, 상호작용 모델과 내부 transport 매핑을 다룬다. |
| 5 | [05-framework-api.ko.md](spec/06-framework-api.ko.md) | `ASP.NET Core`, `Spring Boot`, `NestJS`, `C++` standalone host 기준의 API 표면 방향을 다룬다. |
| 6 | [비동기 실행과 coroutine 정책](spec/05-async-execution-policy.ko.md) | async submit, blocking 대안 금지, coroutine/adapter의 공통 의미를 정의한다. |
| 7 | [22-actor-model.ko.md](spec/14-actor-model.ko.md) | actor 라이프사이클, session bind, user Spot join, outbound actor 호출과 등록 표면을 정의한다. |
| 8 | [Spot Actor Join / Relocation](spec/15-spot-actor.ko.md) | actor가 Entry Spot과 user Spot 사이를 이동할 때의 admission, commit, callback 순서와 장애 처리를 정의한다. |
| 9 | [Session Actor Dispatch](spec/20-session-actor-dispatch.ko.md) | session actor dispatch의 typed handler, route resolver, helper, `SessionProxy`, error 의미를 정의한다. |
| 10 | [메시지 흐름 추적과 dispatch 관측](spec/26-message-flow-tracing.ko.md) | 메시지 흐름 추적의 mode, event, observer, 성능, 런타임 토글과 correlation 계약을 정의한다. |
| 11 | [40-location-runtime.ko.md](spec/21-location-runtime.ko.md) | peer/spot/actor/route 위치, owner lease, store/resolver, 자동 연결과 운영 조회 계약을 정의한다. |
| 12 | [41-location-store-redis.ko.md](spec/22-location-store-redis.ko.md) | 공식 Redis location store extension의 key, lease, 원자 write, 오류와 테스트 계약을 정의한다. |
| 13 | [24-spot-address-messaging.ko.md](spec/16-spot-address-messaging.ko.md) | spot/actor 대상 주소, 조회와 재조회, 실패 분류와 이동 경계를 정의한다. |
| 14 | [Sample/E2E 설정 정책](sample-e2e-configuration-policy.ko.md) | 설정 파일, 환경 변수 금지, 언어별 typed binding과 runner 책임을 정의한다. |
| 15 | [공통 샘플 시나리오](sample/README.ko.md) | 정본 6종의 언어 중립 업무 흐름, 서버 역할, 메시지와 검증 기준을 정의한다. |
| 16 | [Scenario E2E 테스트](e2e/README.ko.md) | scale-out, 실패, lifecycle, 관측성 조합을 실제 multi-process 구조로 검증한다. |
| 17 | [Performance 테스트](perf/README.ko.md) | 모든 framework 언어가 같은 조건과 메트릭으로 성능을 측정하는 규격을 정의한다. |
| 18 | [.NET 문서](../dotnet/README.ko.md) | `.NET`과 `ASP.NET Core` 전용 문서 진입점. |
| 19 | [Java 문서](../java/README.ko.md) | `Java`, `Kotlin`, `Spring Boot` 전용 문서 진입점. |
| 20 | [Node.js 문서](../node/README.ko.md) | `Node.js`와 `NestJS` 전용 문서 진입점. |
| 21 | [C++ 문서](../cpp/README.ko.md) | `C++` zlink framework host 전용 문서 진입점. |

개요(1)로 전체 그림을 잡고, 상호작용과 메시지 모델(2-3), topology(4), API와
비동기 실행(5-6), actor와 Spot 계약(7-9), 관측과 위치 관리(10-13)를 순서대로 본다.
그다음 설정 정책(14)으로 sample/E2E의 설정 전달 경계를 확인한다. 공통 sample(15)로 정본 업무
흐름을 확인하고, E2E(16)와 performance(17)로 검증 기준을 확인한 뒤 언어별 상세(18-21)로
내려간다.

언어별 상세 문서를 새로 읽을 때는 아래 순서를 기본으로 본다.

1. 공통 문서로 의미를 먼저 이해한다.
2. 해당 언어 `README.ko.md`로 진입한다.
3. 그 언어의 인터페이스 기준 문서, 주제 문서, 샘플 문서를 순서대로 읽는다.

## 4. 각 문서의 범위 원칙

각 문서가 다루는 내용이 겹치지 않도록, 아래 원칙을 따른다.

| 개념 | 다루는 곳 | 다른 문서에서는 |
|------|----------|---------------|
| 제품 정의, 차별점, transport 축, 우선 범위 | overview | 필요하면 overview를 링크 |
| 상호작용 모델 분류와 모델별 의미 | interaction-model | 필요하면 interaction-model을 링크 |
| 메시지 구조, header 필드, codec | message-model | 필요하면 message-model을 링크 |
| channel grouping, Discovery, 내부 매핑 | channel-topology | 필요하면 channel-topology를 링크 |
| 프레임워크별 API 표면, DI, handler 등록 | framework-api, dotnet/ | 필요하면 해당 문서를 링크 |
| actor 개념, 라이프사이클, session bind | actor-model | 필요하면 actor-model을 링크 |
| Spot actor join/relocation 완료 조건과 callback 순서 | spot-actor | 필요하면 spot-actor를 링크 |
| session actor dispatch | session-actor-dispatch | 필요하면 session-actor-dispatch를 링크 |

## 5. 문서 작성 원칙

- 새 공통 동작이 필요하면 먼저 공통 spec에 계약 근거가 있는지 확인한다.
- framework의 목표 public contract는 구현보다 먼저 정식 spec과 언어별 exact
  interface에 고정한다. 현재 구현과의 차이는 `90-implementation-gap.ko.md`에서만
  추적한다.
- 구현이 끝난 업무 흐름은 `sample/`에, 구현 검증 요구사항은 `e2e/`에 반영한다.

이 문서 묶음은 "API를 먼저 적고 나중에 용도를 붙이는" 방식이 아니라,
"용도를 먼저 적고 API를 그 용도에 맞춰 좁히는" 방식을 따른다.

## 6. 언어별 상세 문서 작성 규칙

이 공통 묶음은 `.NET` 하나만 위한 문서가 아니다. `Java`, `Kotlin`, `Node.js`,
`C++` 상세 문서도 이 묶음을 기준으로 같은 수준으로 작성할 수 있어야 한다.

그래서 언어별 디렉토리는 아래 규칙을 따른다.

### 6.1 공통 문서를 다시 정의하지 않는다

언어별 문서는 아래 의미를 새로 정의하면 안 된다.

- 상호작용 모델 이름과 의미
- message header의 공통 의미
- channel grouping과 Discovery/수동 연결의 기본 관계
- 정본 sample 시나리오와 E2E 검증 기준

이 의미를 바꾸고 싶으면 먼저 공통 문서를 수정해야 한다.

### 6.2 언어별 문서는 공통 개념을 구체화한다

언어별 문서는 아래 내용을 해당 언어 관용구로 내려 적어야 한다.

- registration API 이름
- client / publisher / manager 인터페이스 이름
- context, options, attribute/decorator/interface 같은 언어별 표면
- DI 또는 lifecycle 통합 방식
- 샘플 코드에서의 실제 호출 모양

공통 문서가 "무슨 의미를 가져야 하는가"를 정하면, 언어 문서는
"그 의미가 이 언어에서 어떤 시그니처와 샘플로 보이는가"를 적는다.

### 6.2.1 네이밍 규칙

framework 문서의 public 이름 규칙은
[bindings/doc/spec/README.ko.md](../../../../bindings/doc/spec/README.ko.md)의
`Naming Policy`를 그대로 따른다. 이 공통 문서와 언어별 상세 문서는 아래 규칙을
같이 지켜야 한다.

- 개념 이름은 바인딩 간 최대한 동일하게 유지한다.
- 실제 언어 문서에서 허용하는 변형은 언어별 케이싱 차이와, overloading이 없는
  언어에서의 최소 접미사뿐이다.
- 단어 교체, 단어 생략, 의미가 같은 별도 이름 추가는 허용하지 않는다.
- 파라미터 조합이 다르다는 이유만으로 이름을 늘리지 않는다.
- async submit, blocking 대안 금지, coroutine adapter의 공통 의미는
  [비동기 실행과 coroutine 정책](spec/05-async-execution-policy.ko.md)을 따른다.
- builder terminator 이름은 공통 의미를 유지하되, 각 언어의 fluent API 관례에 맞춰
  투영한다. 예를 들어 `.NET` awaitable terminator는 `Async(...)`, Java는
  `submit(...)` / `await(...)`, C++ coroutine terminator는 `async()`, Node.js
  `Promise` terminator는 `submit(...)`이다.

문서에서 우선 따라야 할 언어별 케이싱은 아래와 같다.

- Java: 메서드는 `camelCase`, 클래스와 annotation은 `PascalCase`
- C#: public API 전체를 `PascalCase`
- Kotlin: 메서드는 `camelCase`, 클래스와 annotation은 `PascalCase`
- C++: 메서드는 `snake_case`, 타입은 `_t` 접미사
- Node/TypeScript: 메서드는 `camelCase`, 클래스는 `PascalCase`

framework adapter 문서도 `sendWithRoutingId`, `request_callback`,
`publishToTopic`, `recvTimeout` 같은 이름을 쓰지 않고, 가능하면 canonical
action 이름을 유지해야 한다. 예를 들면 아래처럼 맞춘다.

- `Send`, `Request`, `Publish`
- `SendToNode`, `RequestToNode`
- `SendToChannel`, `RequestToChannel`
- `Connect`, `Bind`, `Close`
- `CreateAsync`, `GetAsync`, `ListAsync`

정리하면:

- 공통 문서는 이름의 의미와 canonical 단어 구성을 정한다.
- 언어 문서는 그 이름을 각 언어 케이싱 규칙에 맞게만 변환한다.
- 샘플 코드와 본문 설명도 같은 규칙을 따라야 한다.

### 6.3 언어별 디렉토리의 최소 문서 세트

`.NET` 수준의 상세 문서을 새 언어로 만들려면 최소한 아래 문서 세트가 필요하다.

| 문서 종류 | 역할 |
|----------|------|
| `README.ko.md` | 그 언어 묶음의 진입점. 문서 구조, 역할 분담, 핵심 방향을 정리한다. |
| 인터페이스 기준 문서 | 공용 interface / context / configuration surface / attribute 또는 decorator를 한 곳에 모은다. 공용 계약과 내부 runtime 구현의 분리 기준은 [05-framework-api.ko.md §1.1](spec/06-framework-api.ko.md#11-public-contract와-runtime-implementation의-경계)을 따른다. |
| channel messaging 주제 문서 | channel 등록, handler 모델, outbound client, dispatch 흐름을 설명한다. |
| channel messaging 샘플 문서 | 등록부터 handler, client 호출까지 한 번에 보이는 샘플을 둔다. |
| `SPOT` 주제 문서 | 해당 언어에서 `SPOT`을 지원하면 lifecycle, publish/subscribe, RouteMesh 등록을 설명한다. |
| `SPOT` 샘플 문서 | room/stage/zone 같은 실제 흐름을 코드로 보여 준다. |
| Actor / Entry Spot 주제 문서 | actor factory, Entry Spot registry, user Spot registry, actor packet handler, join/leave lifecycle handler를 설명한다. |
| Actor / Entry Spot 샘플 문서 | Entry Spot에서 인증 또는 target Spot 선택을 처리하고, user Spot에서 domain packet을 처리하는 흐름을 한 예시 안에 보여 준다. |
| `STREAM` 주제 문서 | framework Header 기반 packet session과 공개 계약을 설명한다. |
| `STREAM` 샘플 문서 | 등록과 handler 코드를 한 번에 보여 준다. |
| Monitoring 주제 문서 | socket/discovery/registry/spot runtime event와 등록 모델을 설명한다. |
| Registry 주제 문서 | embedded/standalone, query surface, topology 조회를 설명한다. |

언어 특성상 어떤 축이 아직 미구현이어도 목표 계약을 정식 spec에서 빼지 않는다.
현재 구현과의 차이와 후속 계획은 `90-implementation-gap.ko.md`에서만 추적한다.

### 6.3.1 대표 프레임워크 기준

언어별 상세 문서은 아래 대표 프레임워크 또는 host를 기준으로 먼저 작성한다.

| 언어 | 대표 기준 |
|------|-----------|
| `.NET` | `ASP.NET Core` |
| `Java` | `Spring Boot` |
| `Kotlin` | `Spring Boot` |
| `Node.js` | `NestJS` |
| `C++` | zlink framework host |

`C++`는 다른 언어처럼 기존 웹 프레임워크 위 adapter로 보기보다,
zlink framework host가 lifecycle과 dispatch loop를 직접 소유하는 방식으로 설명한다.
이 경우에도 application이 runtime 구현을 직접 시작하는 public contract는 두지 않는다.
각 언어별 상세 문서는 application이 접근하는 public contract와 adapter 내부 runtime
구현을 구분해야 한다. runtime 구현 타입이 테스트나 adapter 내부에 남아 있더라도,
사용자 guide와 package/module entrypoint에서는 직접 runtime을 만들거나 시작하는 경로를
노출하지 않는다.

### 6.4 언어별 문서의 최소 체크리스트

언어별 상세 문서은 아래 항목을 명확히 적어야 한다.

- local channel을 어떻게 등록하는가
- outbound channel을 어떻게 등록하는가
- 자동 연결과 수동 연결을 어떻게 설정하는가
- request/send/event 호출 시 기본 packet key를 어떻게 해석하는가
- timeout, packet override 같은 변형을 어떤 `options` 또는 동등한 구조로 두는가
- send/publish의 내부 submit 정책과 `SendTimeout` 기반 backpressure 처리를
  어떻게 설명하는가
- handler dispatch가 어떤 ingress를 기준으로 설명되는가
- outbound reply 수신은 어떤 경로로 처리되는가
- outbound-only 앱이 가능한가
- `STREAM`을 지원하면 Framework 내부 recv loop가 packet을 managed queue로 넘긴 뒤
  session callback을 실행하는지, 같은 session callback 직렬성이 보장되는지
  설명하는가
- actor/session 모델을 지원하면 actor가 `Spot`에 attach된 뒤 actor callback이
  해당 Actor의 직렬 실행 문맥에서 실행되는지 설명하는가
- actor/session 모델을 지원하면 Entry Spot public 표면을 별도 섹션으로 설명하는가
- Entry Spot에서 actor packet handler를 등록하는 API와 예시가 있는가
- user Spot에서 actor packet handler를 등록하는 API와 예시가 있는가
- Entry Spot과 user Spot의 actor packet handler 인자 차이를 설명하는가
- actor join/leave lifecycle을 `OnJoinedActor` / `OnLeaveActor`에 해당하는 Spot
  member callback으로 설명하는가
- Entry Spot registry와 user Spot registry가 서로 다른 namespace라서 같은 actor
  type과 packet 이름을 다르게 매핑할 수 있음을 설명하는가
- 같은 registry 안에서 동일 actor type과 packet name의 actor packet handler를
  중복 등록하면 startup validation 오류임을 설명하는가
- actor/session 모델의 회귀 테스트는 join 직후 packet, spot 이동 직후 packet,
  stale session packet을 구분해서 검증하는가
- stream session 회귀 테스트는 callback task dispatch, 같은 session callback
  직렬성, enqueue 진입점만 허용되는지 검증하는가
- `SPOT`을 지원하면 Spot 타입 기준 factory 등록, `RoutingId` 기준 생성과 조회,
  lifecycle timer, 외부 spot publish 표면을 어떻게 설명하는가
- monitoring을 지원하면 socket/discovery/registry/spot runtime event를 어떤
  typed event와 등록 surface로 설명하는가
- 샘플 코드가 실제 registration API와 인터페이스 이름과 맞는가

이 체크리스트를 만족하지 않으면, 공통 개념이 언어 표면으로 충분히 내려오지
않은 것으로 본다.

### 6.5 언어별 목표 계약과 구현 차이 처리 규칙

framework의 목표 public contract는 아직 구현되지 않았더라도 공통 spec과 언어별
exact interface에 먼저 고정한다. 구현이 없다는 이유로 현재 언어들의 최소 공통분모로
계약을 축소하지 않는다. 현재 구현과 목표 계약의 차이, 누락 사유와 후속 계획은
`90-implementation-gap.ko.md`에서만 추적한다.

공통 spec이나 guide에 근거가 없는 새 public API 후보는 정식 계약에 바로 추가하지
않고 별도 draft에서 검토한다. 계약으로 승인하면 정식 spec과 모든 언어 exact
interface를 먼저 갱신한 뒤 구현과 contract test를 맞춘다. core 공개 계약까지 바뀌는
설계는 루트 `doc/spec/draft/`의 작성 규칙을 따른다.

## 7. 언어별 공개 계약

Framework server package의 공통 동작이 각 언어의 public API에서 어떤 정확한
형태로 제공되는지는 아래 언어별 문서가 정의한다. 여기 기록한 signature는 해당
언어 구현과 contract test가 따라야 하는 정식 계약이다.

| 언어 | 공개 계약 |
|------|-----------|
| `.NET` | [dotnet](spec/server/languages/dotnet/README.ko.md) |
| Java | [java](spec/server/languages/java/README.ko.md) |
| Kotlin | [kotlin](spec/server/languages/kotlin/README.ko.md) |
| Node.js framework | [node](spec/server/languages/node/README.ko.md) |
| C++ | [cpp](spec/server/languages/cpp/README.ko.md) |

Client package의 public interface는 여기서 정의하지 않는다. Stream connector는
[언어별 Stream connector 계약](spec/stream-connector/README.ko.md), HTTP
client는 [언어별 HTTP client 계약](spec/http-client/README.ko.md)이 각각
소유한다.

언어별 스펙은 서로의 시그니처를 복사하는 문서가 아니다. 같은 공통 동작을 해당
언어 사용자가 자연스럽게 사용할 수 있는 public contract로 고정한다. 계약을
변경하는 절차는 [공개 계약 관리](spec/00-public-contract-governance.ko.md)를
따른다.

---
<!-- framework-adapter-nav:bottom:start -->
[가이드 홈](../index.ko.md) | [이전: ZLink Framework](../index.ko.md) | [다음: ZLink Framework Overview](spec/02-overview.ko.md)
<!-- framework-adapter-nav:bottom:end -->
