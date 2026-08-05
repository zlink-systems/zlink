<!-- framework-adapter-nav:start -->
[문서 목록](../../../README.ko.md) | [이전: Regression Test Matrix](regression-test-matrix.ko.md)<!-- framework-adapter-nav:end -->

[스펙 목차](../../common/README.ko.md)

[.NET 묶음](../README.ko.md) | [인터페이스](../../common/spec/server/languages/dotnet/interfaces/README.ko.md) | [Runtime Lifecycle](../../common/internals/README.ko.md) | [Regression Matrix](regression-test-matrix.ko.md)

# ZLink Framework .NET Backend Dependency Policy

## 1. 목적

지금 시점에서 가장 현실적인 선택은 `bindings/dotnet` 라이브러리를 backend 로
그대로 활용하는 것이다. 다만 framework 의 public contract 가 이 backend 구현체에
지나치게 얽혀 버리면 곤란하다. 나중에 저수준 라이브러리를 교체할 때 public API
까지 같이 깨질 수밖에 없기 때문이다.

이 문서는 다음 두 가지를 동시에 만족시킬 수 있도록 기준을 정리한다.

- 지금은 `bindings/dotnet` 을 활용해서 구현한다.
- 나중에는 다른 저수준 `.NET` 라이브러리로 backend 를 교체할 수 있어야 한다.

## 2. 기본 원칙

backend 라이브러리는 언제든 교체할 수 있는 구현체로 보고, 그 위에 framework 의
public contract 를 안정적으로 얹는 것을 목표로 한다. 구체적으로는 다음 네 가지
원칙을 따른다.

- framework 의 public contract 가 우선이다. backend 라이브러리는 언제든 교체할
  수 있는 구현체로 본다.
- public API 에 backend 의 서비스 객체를 직접 노출하지 않는다.
- backend 가 바뀌면 함께 바뀌기 쉬운 타입은 framework 경계 안쪽으로 숨긴다.
- 이미 public contract 에 남아 있는 transport primitive[^transport-primitive] 는
  "backend 와 독립된 의미를 가진 핵심 값" 으로 본다.

## 3. 현재 backend 정책

- 현재 backend 구현은 `bindings/dotnet` 이다.
- framework runtime은 bindings가 공개한 raw `DEALER`, `ROUTER`, `PUB`, `SUB`, `STREAM` socket API만
  사용한다.
- 다만 framework 사용자가 이런 객체를 생성자 파라미터나 public property 로
  직접 받게 만들지는 않는 것이 기본이다.

즉 "지금은 `bindings/dotnet` 을 써서 구현한다" 는 사실과, "framework public API
가 곧 `bindings/dotnet` 의 객체 모델 그대로여야 한다" 는 주장은 서로 다른
이야기다.

## 4. Public API 에 남겨도 되는 것

현재 문서 기준에서 아래 타입은 public contract 에 그대로 남겨 둔다.

- `RoutingId`
- `Message`
- `SendFlags`
- `ActorRef`

이 타입들은 특정 runtime 객체가 아니다. transport identity[^transport-identity],
payload, submit option, actor handle 처럼 의미가 분명한 기초 primitive 에 해당한다. 즉 나중에
backend 가 교체되더라도 같은 의미를 유지하도록
compatibility layer[^compatibility-layer] 를 끼워 줄 수 있는 종류다.

## 5. Public API 에 직접 새어 나오면 안 되는 것

다음 타입과 객체 모델은 framework 의 public contract 에 직접 노출하지 않는다.

- raw socket instance
- `SpotNode`
- `Spot`
- 하부의 timer, recv loop, raw socket monitor 같은 객체

이 객체들은 backend 구현 세부에 가깝다. 따라서 public API 표면[^public-surface]
에 한 번이라도 새어 들어가면 곤란하다. 다음 backend 교체가 곧바로
breaking change[^breaking-change] 로 이어지기 때문이다.

## 6. 진단 / 운영 타입 정책

monitoring, registry query, spot status 같이 하부와 가까운 값은 일부가 public
surface 에 남을 수 있다. 그래도 다음 원칙으로 폭을 좁힌다.

- source 이름, timestamp, logical event kind 같은 값은 framework 가 의미를
  소유하는 값으로 본다.
- 반면 native enum 이나 raw status 값은 optional diagnostic detail 로만 남긴다.
- backend 가 교체될 때 동일 의미를 유지하기 어려운 값은 다르게 다룬다. 이때는
  framework 쪽에서 정의한 synthetic enum[^synthetic-enum] 과
  snapshot DTO[^dto] 를 우선 드러내고, native detail 은 줄이는 편을 기본으로 본다.

즉 monitoring public API 는 두 가지 구조 가운데 후자가 backend 교체에 더 안전하다.

- "backend 의 raw event 를 그대로 재수출하는 구조"
- "framework 가 한 번 재해석한 typed runtime event + 꼭 필요할 때만 추가되는
  native detail" 구조

## 7. 구현 지침

framework 와 backend 사이에는 항상 어댑터 한 층을 두고, 역할을 깔끔하게
나누어 둔다.

- framework 내부에는 backend adapter layer[^backend-adapter] 를 둔다.
- registration, lifecycle[^lifecycle], monitoring, query 같은 framework 본연의
  역할은 framework service 가 맡는다. 실제 backend 호출은 adapter layer 가
  떠맡는다.
- Adapter는 bindings의 public raw socket API만 호출한다. Core service C API, `NativeMethods`, non-public
  reflection, native symbol 직접 호출과 service binding object를 사용하지 않는다.
- 샘플 문서가 low-level binding 타입을 직접 보여 주더라도, 그 설명이 framework
  의 public API 표면 설명과 섞이지 않게 분리한다.

## 8. 교체 시 규칙

나중에 저수준 라이브러리를 교체할 때는 아래 순서를 지킨다.

1. framework 의 public contract 를 먼저 그대로 유지한다.
2. 기존 backend adapter 와 새 backend adapter 를 같은 contract 뒤에 나란히
   꽂는다.
3. public API 에 남아 있는 primitive 가 새 backend 에서도 그대로 유지될 수 있는지
   확인한다.
4. 유지 불가능한 타입이 있으면, backend 교체와 같은 작업에서 즉시 없애지 말고
   먼저 framework wrapper 를 도입한 뒤에 교체한다.

즉 backend 교체는 어디까지나 adapter layer 의 교체를 기본으로 본다. public API
자체의 교체는 그것과는 별개의 breaking change 작업으로 분리해서 진행하는 편을
원칙으로 삼는다.

## 9. 회귀 테스트

backend 의존 정책은 framework 의 public API 와 adapter factory 두 축으로
점검한다. 점검 기준은 두 가지다.

- 구현체가 바뀌더라도, 사용자는 backend 의 concrete type 을 알 필요가 없어야
  한다.
- native binding wrapper 의 생성은 adapter 내부에서만 일어나야 한다.

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `ScaffoldSmokeTests.PublicSurface_DoesNotExpose_BackendConcreteTypes` | 허용한 값 타입을 제외하면, backend concrete type 이 public surface 에 나타나지 않는다. |
| `BackendAdapterFactoryTests.BackendFactory_Creates_Channel_Spot_And_Stream_Wrappers` | backend factory 가 channel, SPOT, STREAM wrapper 를 모두 만들어 낸다. |
| `BackendAdapterFactoryTests.BackendFactory_Creates_MonitoringAdapter` | monitoring adapter 생성 경로가 backend 내부 안에 머문다. |
| `BackendDependencyTests.Runtime_Uses_Only_Public_Raw_Binding_Surface` | service C API, private member와 reflection 참조가 0건이다. |

[^public-contract]: public contract 는 외부 사용자에게 공개되어 변경 시 호환성을 책임져야 하는 API 표면을 뜻한다.
[^backend]: backend 는 framework 가 실제 동작을 위임하는 저수준 구현체를 가리킨다. 여기서는 `bindings/dotnet` 이 backend 다.
[^transport-primitive]: transport primitive 는 메시지 전송 계층에서 의미가 단단하게 굳어진 기초 값(예: `RoutingId`, `Message`, `SendFlags`)을 가리킨다.
[^transport-identity]: transport identity 는 전송 계층에서 누가 누구에게 보내는지를 식별하는 값이다. `RoutingId` 가 대표적인 예다.
[^compatibility-layer]: compatibility layer 는 내부 구현이 바뀌어도 외부에서 보이는 의미가 같게 유지되도록 끼워 넣는 중간 코드를 가리킨다.
[^public-surface]: public surface 는 외부 사용자에게 노출되는 모든 타입·메서드·attribute 의 총합을 가리킨다. 본문에서는 가능한 한 public API 표면이라고 풀어 쓴다.
[^breaking-change]: breaking change 는 기존 사용자 코드를 그대로 다시 빌드하거나 실행할 수 없게 만드는 비호환 변경을 뜻한다.
[^synthetic-enum]: synthetic enum 은 backend 가 내려 주는 원시 값을 그대로 쓰지 않고, framework 쪽에서 다시 정의한 의미 단위로 만든 enum 을 가리킨다.
[^dto]: DTO(Data Transfer Object) 는 계층 간에 값을 옮기는 용도의 단순한 데이터 구조다.
[^backend-adapter]: backend adapter layer 는 framework 의 표면과 실제 저수준 backend 사이를 잇는 중간 계층이다. backend 가 바뀌어도 public API 가 흔들리지 않게 해 준다.
[^lifecycle]: lifecycle 은 컴포넌트가 시작·동작·종료되는 전체 수명 주기와, 각 단계에서 일어나는 일들을 가리킨다.

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../README.ko.md) | [이전: Regression Test Matrix](regression-test-matrix.ko.md)<!-- framework-adapter-nav:bottom:end -->
