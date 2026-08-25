---
title: "공개 계약 관리"
---

# 공개 계약 관리

[Foundation 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [다음: 02. Framework 메시징 용어집](02-glossary.ko.md)

> Framework 공개 계약을 누가 소유하고, 새 계약을 어떻게 고정하고 검증하는지를 정의한다.

## 1. 공개 계약이란 무엇인가

공개 계약은 application이 호출하는 타입과 operation만을 뜻하지 않는다. timeout, 취소, 오류,
callback 실행 순서, ownership과 완료 조건까지 포함한다. 이 문서는 이 계약의 소유권과 검증
규칙을 정의한다.

## 2. 계약 소유권

공개 계약은 공통 의미와 언어별 표현으로 나눈다.

| 위치 | 소유하는 내용 |
|---|---|
| 이 디렉터리와 package별 공통 스펙 | 언어와 무관한 기능, 상태, 완료 조건, 오류 의미 |
| package의 `languages/<lang>/` | 실제 public 타입, 메서드 시그니처, generic·nullable 규칙, 언어별 비동기 표현 |
| Core 정식 spec | context, message, raw socket, transport, poller와 generic monitoring 계약 |
| 이 디렉터리의 구현 스펙 서술 | 공개 계약을 만족시키기 위해 모든 언어 service runtime이 공통으로 따르는 배선, 상태 기계, protocol 처리와 thread·executor 구조. 새 공개 계약은 소유하지 않는다. |

공통 스펙은 특정 언어의 문법을 표준으로 삼지 않는다. 각 언어는 같은 기능과 관찰 가능한 결과를
자기 언어의 관례로 표현한다. 여러 MeshNode가 참여해 node와 Channel message를 주고받는 범위인
[RouteMesh](02-glossary.ko.md#routemesh)와, 그 RouteMesh에 참여해 message를 보내거나 받는
runtime node인 [MeshNode](02-glossary.ko.md#meshnode)의 .NET 정확한 시그니처는
[.NET RouteMesh·MeshNode 인터페이스](../languages/dotnet/interfaces/03-configuration-topology.ko.md)가
소유한다.

- **사용자가 관찰하는 동작은 이 디렉터리의 공통 spec과 언어별 interface에 한 번만
  정의한다.** 같은 문서가 그 동작을 만드는 구현 구조를 이어서 서술할 수 있지만, 공개 동작을
  다시 규정하거나 더 강한 보장으로 확장하지는 않는다 — 공개 계약을 소유하는 자리가 하나로
  유지되지 않기 때문이다.
- **구현 구조 서술이 공개 동작·public API와 충돌하면 그 충돌은 결함이다.**
  - 어느 한쪽이 우선한다고 적어 충돌을 남겨 두지 않는다.
  - 계약을 기준으로 구현 구조 서술을 고치고, 구현이 그 구조를 따르기 어려워 공개 동작
    자체를 바꿔야 한다면 [§5 공개 계약 절차](#5-공개-계약-절차)를 다시 따른다.
  - Gap report는 공개 계약 차이, 구현 구조 차이와 검증 증거 부족을 구분해 기록한다.

## 3. Production source owner

Application이 compile 시점에 참조하는 것 — interface, 호출, context, option, result와 error —
은 배포 package마다 **소스 파일이 놓이는 자리가 한 곳**이어야 한다. 그 자리를 이 문서에서
"계약 소스"라고 부른다. Directory 이름은 언어의 관례를 따르되 다음은 모든 언어에서 지킨다.

- **Application이 쓰는 계약의 소스는 그 계약을 배포하는 package 안에 둔다.**
- **Server, HTTP client와 Stream Connector는 서로 다른 배포 package이므로 계약 소스도 각각
  자기 package 안에 둔다.** 한 package의 계약 파일을 다른 package의 계약 자리로 쓰지 않는다 —
  그러면 그 package를 받는 쪽이 쓰지도 않는 package에 의존하게 된다.
- **Public constructor, factory, builder 진입점, free function, extension, DTO, value, enum과
  public error·result도 interface와 같은 자리에 둔다.** 이것들을 interface와 다른 곳에 흩어
  두면 계약이 놓인 자리가 한 곳이 아니게 된다.
- **Runtime 구현이 계약을 참조한다. 계약 소스는 runtime 구현을 참조하지 않는다.** 이 방향이
  뒤집히면 계약이 구현 세부에 매이고, 구현을 바꿀 때마다 계약이 함께 흔들린다.
- **`runtime/internal` 아래 선언은 다른 package에서 가져올 수 없어야 한다.** Directory 이름만
  `internal`로 바꾸고 실제 접근 범위는 public으로 두면 이 규칙을 지킨 것이 아니다.
- **외부 provider가 구현해야 하는 최소 SPI는 별도 package로 떼어 낼 수 있다.** 각 Spot의 현재
  owner와 lifecycle 상태를 여러 node가 함께 확인하도록 보관하는 저장소인
  [Location Store](02-glossary.ko.md#location-store) provider처럼 Framework 밖에서 만드는
  구현체는 그 package 하나만 참조하면 되기 때문이다.
  다만 application이 직접 쓰는 계약까지 그 package로 옮기지는 않는다 — application이 provider
  package에 의존하게 된다.
- **여러 package가 같은 타입으로 주고받아야 하는 codec과 error만 어느 package에도 속하지 않는
  공용 package에 둘 수 있다.** 같은 이름의 타입을 package마다 따로 선언하면 서로 다른 타입이
  되어 값을 주고받을 수 없다. 그 밖의 계약을 공용 package로 옮기면 각 계약이 어느 package
  소유인지 흐려진다.

Namespace와 package의 **전체 이름** — `systems.zlink.framework.actors`처럼 최상위부터 이어
붙인 이름 — 은 언어별 interface가 정한다. Source directory를 정리한다는 이유로 이 이름을 바꾸지
않는다. 이름은 그 package를 참조하는 쪽의 코드에 그대로 박혀 있어서, 바꾸면 계약을 바꾸지
않았는데도 받아 쓰는 쪽이 깨진다.

Layout을 바꾸려면 세 가지를 함께 통과해야 한다 — 기록해 둔 public API 목록과의 대조, 그
package를 받아 쓰는 쪽의 build, 그리고 소유자 검토.

각 언어 interface 문서는 package마다 계약 소스가 어디에 있고 어떤 선언을 public으로 내보내는지
기록한다. 어떤 선언을 그 자리가 아닌 곳에 두어야 한다면 그 선언 하나와 그렇게 해야 하는 의존
관계를 개별로 기록한다. Directory나 assembly 전체를 한꺼번에 예외로 두지 않는다 — 그러면 무엇이
예외인지 추적할 수 없다.

이 경계는 [bindings public/internal 경계](../../../../../../../bindings/doc/spec/README.ko.md#공개-vs-내부-api-경계)와
같은 방향을 사용한다. Framework와 bindings는 서로 다른 배포 package이므로 source root는 각각의
spec이 소유하지만, public contract가 runtime 구현에 역으로 의존하지 않는다는 원칙은 동일하다.

## 4. 새 계약에 고정할 항목

공개 기능을 정의할 때 다음 항목을 함께 고정한다.

- 기능이 속하는 package와 runtime owner
- operation 입력, 결과와 유효한 호출 시점
- timeout, cancellation과 backpressure 의미
- callback 실행 순서와 직렬화 범위
- 메시지와 결과 객체의 ownership
- 설정 오류와 runtime 오류의 구분
- 자동 discovery와 manual peer의 선택 기준
- contract test와 E2E에서 관찰할 결과

함수 이름만 나열해서는 계약이 완성되지 않는다. 사용자가 성공으로 판단할 수 있는 시점과 실패를
받는 위치까지 설명해야 한다.

## 5. 공개 계약 절차

공개 계약을 추가하거나 바꿀 때는 다음 순서를 따른다.

1. 공통 기능과 사용자가 관찰하는 결과를 공통 스펙에 기록한다.
2. 영향을 받는 각 언어 스펙에 정확한 public interface를 기록한다.
3. 현재 checkout과 목표 계약의 차이를 임시 전환 inventory에 기록한다. 정식 스펙은 이 inventory를
   참조하거나 구현 진행 상태를 설명하지 않는다.
4. Core 또는 bindings 계약이 필요하면 해당 package의 정식 스펙을 먼저 맞추고 public header와
   구현을 그 계약에 맞춘다.
5. contract test, 공통 E2E와 sample이 공개 표면만 사용하는지 검증한다.
6. 배포 package의 실제 export와 문서의 시그니처를 대조한다.
7. 독립 리뷰에서 계약, 구현, test와 package 사이의 차이가 없음을 확인한 뒤 변경을 승인한다.

**공통 E2E와 다른 언어 코드는 계약 해석을 검증하는 자료일 뿐 public interface의 근거가
아니다.** public interface는 반드시 정식 스펙에 근거해야 한다.

## 6. 언어별 표현 원칙

언어별 인터페이스는 기능을 같게 유지하면서 다음 관례를 따른다.

- .NET은 `Task`, `ValueTask`, `CancellationToken`과 DI 관례를 사용한다.
- Java는 Java type system과 `CompletionStage` 관례를 사용한다.
- Kotlin은 coroutine을 제공하는 표면에서 `suspend`, `Flow`와 coroutine 취소 규칙을 사용한다.
- Node.js는 `Promise`, TypeScript optional 표현과 필요한 장기 operation의 `AbortSignal`을
  사용한다.
- C++는 명시적인 ownership, value type과 coroutine 규칙을 사용한다.

**한 언어의 타입 이름과 overload 구성을 다른 언어에 그대로 복제하지 않는다.** 기능, 완료
조건과 오류의 의미가 같으면 같은 공통 계약을 투영한 것으로 본다.

## 7. 설계 검토 기준

새 public interface는 호출자가 알아야 하는 결정을 줄여야 한다.

- **node direct, channel select-one과 Logical Multicast는 선택과 submit을 하나의 operation으로
  제공한다.** 호출자가 후보를 조회한 뒤 별도 send를 다시 호출하게 만들지 않기 위해서다.
- **transport endpoint, peer 선택, packet encoding과 reply correlation은 runtime이 소유한다.**
  application이 이 값을 직접 관리하면 언어마다 다른 배선이 공개 표면에 새어 나온다.
- **message를 받을 대상을 나타내는 논리 instance인 [Spot](02-glossary.ko.md#spot), Actor와
  STREAM session의 주소와 generation은 typed handle이나 context가 보존한다.** 호출마다 값을
  다시 조합하게 하지 않기 위해서다.
- **같은 기능을 이름만 달리한 interface로 반복하지 않는다.**
- **유효하지 않은 상태 조합을 여러 nullable 값과 boolean으로 나누어 표현하지 않는다.**
- **timeout이나 metadata처럼 operation마다 허용 범위가 다른 설정은 해당 call object에만 둔다.**

비자명한 설계는 두 가지 이상을 비교하고, public interface가 더 작으며 transport 지식이 덜
노출되는 방식을 선택한다.

언어별 interface 문서에도 같은 경계가 적용된다.

- **언어별 interface 문서는 application이 직접 사용하는 API와 외부 provider package가
  반드시 구현해야 하는 SPI만 기록한다.** runtime 내부 배선, 저장 row와 key, 상태 전이용
  command, change watch, publisher, dispatcher invocation과 native diagnostic은 public
  contract가 아니다. 이런 타입은 구현에 필요하더라도 package 내부에 두고, 그 책임과 동작은
  이 디렉터리의 구현 스펙 서술이나 언어별 구현 문서에서 설명한다.
- **하나의 provider가 같은 일관성 경계를 구현할 수 있는 기능을 세부 capability interface로
  나누어 application registration에 노출하지 않는다.** 외부 provider 구현에 필요한 최소
  operation은 하나의 깊은 SPI에 모으고, 선택 기능은 기본 구현이나 capability query로 흡수한다.
- **외부에서 구현하거나 호출할 공개 declaration이 하나도 남지 않은 언어별 interface 문서는
  삭제한다.** 이 기준은 .NET, Java, Kotlin, Node.js와 C++에 동일하게 적용한다.

## 8. 검증 요구

각 언어의 contract test와 실제 배포 package(외부 package에서 가져올 수 있는 public export,
package별로 정한 contract source, 시그니처, 비동기 결과, [owner](02-glossary.ko.md#owner)별
메시징 계약)만으로 다음을 확인한다.

**Export와 dependency 방향**

- 외부 package에서 가져올 수 있는 public export는 package별로 정한 contract source 안에서만
  나온다 — contract source 밖의 application-facing public declaration이 없다.
- contract source에서 runtime, internal 또는 native bridge source로 향하는 역방향 dependency가
  없다.
- `runtime/internal` source의 declaration은 실제 package·module·assembly visibility로
  차단되어 외부에서 가져올 수 없다.

**Signature와 표현**

- public 타입과 메서드 시그니처가 언어별 interface와 일치한다.
- generic, nullable, optional, 기본값과 overload가 언어별 interface가 정한 그대로 노출된다.

**완료와 오류**

- 비동기 결과, timeout과 취소가 언어별 관례([§6](#6-언어별-표현-원칙))대로 관찰된다.
- 공개 오류 kind와 lifecycle callback이 문서가 정한 대로 호출된다.

**메시징과 저장소 등록**

- 하나의 RouteMesh 물리 연결 그룹을 식별하는 이름인
  [MeshName](02-glossary.ko.md#meshname), message를 보낼 Channel 범위를 식별하는 이름인
  [ChannelName](02-glossary.ko.md#channelname), RID와 [owner](02-glossary.ko.md#owner)별
  메시징 계약이 문서와 일치한다.
- Redis location store의 명시 등록과 manual peer 구성이 문서와 일치한다.

검증은 source tree만 보지 않는다. 실제 배포 package를 외부 consumer가 참조해 같은 public
surface와 동작을 얻는지 확인한다.

---

[Foundation 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [다음: 02. Framework 메시징 용어집](02-glossary.ko.md)
