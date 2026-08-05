---
title: "Framework 공개 계약 관리"
---

# Framework 공개 계약 관리

[스펙 목차](README.ko.md) · [다음: Framework 메시징 용어집](01-glossary.ko.md)

> **이 장이 정의하는 것** — Framework 공개 계약의 소유권과 검증 규칙.


## 1. 목적

이 문서는 ZLink Framework 공개 계약의 소유권과 검증 규칙을 정의한다. 공개 계약은
사용자가 호출할 수 있는 타입과 operation뿐 아니라 timeout, 취소, 오류, callback 순서, ownership과
완료 조건을 포함한다.

## 2. 계약 소유권

공개 계약은 공통 의미와 언어별 표현으로 나눈다.

| 위치 | 소유하는 계약 |
|---|---|
| 이 디렉토리와 package별 공통 스펙 | 언어와 무관한 기능, 상태, 완료 조건, 오류 의미 |
| package의 `languages/<lang>/` | 실제 public 타입, 메서드 시그니처, generic·nullable 규칙, 언어별 비동기 표현 |
| Core 정식 spec | context, message, raw socket, transport, poller와 generic monitoring 계약 |
| Framework internals | 언어별 service runtime의 배선, 상태 기계, protocol 처리와 thread·executor 구조 |

공통 스펙은 특정 언어의 문법을 표준으로 삼지 않는다. 각 언어는 같은 기능과 관찰 가능한 결과를
자기 언어의 관례로 표현한다. .NET RouteMesh·MeshNode의 정확한 시그니처는
[.NET RouteMesh·MeshNode 인터페이스](server/languages/dotnet/interfaces/03-configuration-topology.ko.md)가 소유한다.

### 2.1 Production source owner

각 배포 package는 application이 compile하는 interface, call, context, option, result와 error의
production source owner를 하나만 둔다. Source directory 이름은 언어의 관례를 따르되 다음 방향은
모든 언어에서 지킨다.

- Application contract source는 해당 배포 package가 소유한다.
- Server, HTTP client와 Stream Connector는 각각 독립된 배포 package이므로 각 package가 자기
  contract source owner를 둔다. 한 package의 contract artifact를 다른 package 전체의 contract
  owner로 사용하지 않는다.
- Public constructor, factory, builder entrypoint, free function, extension, DTO, value, enum과
  public error/result도 interface와 같은 contract source가 소유한다.
- Runtime 구현은 contract를 참조한다. Contract source는 runtime 구현을 참조하지 않는다.
- `runtime/internal` 아래 declaration은 외부에서 import할 수 없어야 한다. Directory 이름만
  `internal`로 바꾸고 public visibility를 유지하면 이 규칙을 충족하지 않는다.
- 외부 provider가 구현해야 하는 최소 SPI는 별도 abstraction artifact가 소유할 수 있다.
  Application-facing contract 전체를 SPI artifact로 옮기지 않는다.
- 여러 package가 같은 type identity를 사용해야 하는 codec·error 같은 최소 contract만
  package-neutral artifact가 소유할 수 있다.

Namespace나 package FQN은 exact interface가 정한다. Source를 정리한다는 이유로 FQN을 바꾸지 않는다.
Layout 변경은 public API snapshot, package consumer build와 owner gate를 함께 통과해야 한다.
각 언어 exact interface는 package별 contract source owner와 public projection을 기록한다. 예외 owner가
필요하면 대상 declaration과 dependency 이유를 개별적으로 기록하며 directory나 assembly 전체를
포괄하는 예외를 두지 않는다.

이 경계는 [bindings public/internal 경계](../../../../../bindings/doc/spec/README.en.md#public-vs-internal-api-boundary)와
같은 방향을 사용한다. Framework와 bindings는 서로 다른 배포 package이므로 source root는 각각의
spec이 소유하지만, public contract가 runtime 구현에 역으로 의존하지 않는다는 원칙은 동일하다.

## 3. 공통 계약의 필수 항목

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

## 4. 공개 계약 절차

공개 계약을 추가하거나 바꿀 때는 다음 순서를 따른다.

1. 공통 기능과 사용자가 관찰하는 결과를 공통 스펙에 기록한다.
2. 영향을 받는 각 언어 스펙에 정확한 public interface를 기록한다.
3. 현재 checkout과 목표 계약의 차이를 임시 전환 inventory에 기록한다. 정식 스펙은 이 inventory를
   참조하거나 구현 진행 상태를 설명하지 않는다.
4. Core 또는 bindings 계약이 필요하면 해당 package의 정식 스펙을 먼저 맞추고 public header와 구현을
   그 계약에 맞춘다.
5. contract test, 공통 E2E와 sample이 공개 표면만 사용하는지 검증한다.
6. 배포 package의 실제 export와 문서의 시그니처를 대조한다.
7. 독립 리뷰에서 계약, 구현, test와 package 사이의 차이가 없음을 확인한 뒤 변경을 승인한다.

공통 E2E와 다른 언어 코드는 계약 해석을 검증하는 자료다. 그 자체만으로 public interface를 만들지는
않는다. public interface는 반드시 정식 스펙에 근거해야 한다.

## 5. 언어별 표현 원칙

언어별 인터페이스는 기능을 같게 유지하면서 다음 관례를 따른다.

- .NET은 `Task`, `ValueTask`, `CancellationToken`과 DI 관례를 사용한다.
- Java는 Java type system과 `CompletionStage` 관례를 사용한다.
- Kotlin은 coroutine을 제공하는 표면에서 `suspend`, `Flow`와 coroutine 취소 규칙을 사용한다.
- Node.js는 `Promise`, TypeScript optional 표현과 필요한 장기 operation의 `AbortSignal`을 사용한다.
- C++는 명시적인 ownership, value type과 coroutine 규칙을 사용한다.

한 언어의 타입 이름과 overload 구성을 다른 언어에 그대로 복제하지 않는다. 기능, 완료 조건과 오류의
의미가 같으면 같은 공통 계약을 투영한 것으로 본다.

## 6. 설계 검토 기준

새 public interface는 호출자가 알아야 하는 결정을 줄여야 한다.

- node direct, channel select-one과 Logical Multicast는 선택과 submit을 하나의 operation으로 제공한다.
- transport endpoint, peer 선택, packet encoding과 reply correlation은 runtime이 소유한다.
- Spot, Actor와 STREAM session의 주소와 generation은 typed handle이나 context가 보존한다.
- 같은 기능을 이름만 달리한 interface로 반복하지 않는다.
- 유효하지 않은 상태 조합을 여러 nullable 값과 boolean으로 나누어 표현하지 않는다.
- timeout이나 metadata처럼 operation마다 허용 범위가 다른 설정은 해당 call object에만 둔다.

비자명한 설계는 두 가지 이상을 비교하고, public interface가 더 작으며 transport 지식이 덜 노출되는
방식을 선택한다.

### 6.1 언어별 exact interface의 공개 경계

언어별 exact interface 문서는 application이 직접 사용하는 API와 외부 provider package가 반드시
구현해야 하는 SPI만 기록한다. runtime 내부 배선, 저장 row와 key, 상태 전이용 command, change watch,
publisher, dispatcher invocation과 native diagnostic은 public contract가 아니다. 이런 타입은 구현에
필요하더라도 package 내부에 두고 공통 또는 언어별 internals에서 책임과 동작을 설명한다.

하나의 provider가 같은 일관성 경계를 구현할 수 있는 기능을 세부 capability interface로 나누어
application registration에 노출하지 않는다. 외부 provider 구현에 필요한 최소 operation은 하나의 깊은
SPI에 모으고, 선택 기능은 기본 구현이나 capability query로 흡수한다. 외부에서 구현하거나 호출할 공개
declaration이 하나도 남지 않은 exact interface 문서는 삭제한다. 이 기준은 .NET, Java, Kotlin,
Node.js와 C++에 동일하게 적용한다.

## 7. 검증

각 언어의 contract test는 최소한 다음을 확인한다.

- 외부 package에서 import할 수 있는 public export
- package별로 정한 contract source 밖의 application-facing public declaration
- contract source에서 runtime, internal 또는 native bridge source로 향하는 역방향 dependency
- runtime/internal source의 declaration이 실제 package·module·assembly visibility로 차단되는지 여부
- public 타입과 메서드 시그니처
- generic, nullable, optional, 기본값과 overload
- 비동기 결과, timeout과 취소
- 공개 오류 kind와 lifecycle callback
- MeshName, ChannelName, RID와 [owner](01-glossary.ko.md#owner)별 메시징 계약
- Redis location store의 명시 등록과 manual peer 구성

검증은 source tree만 보지 않는다. 실제 배포 package를 외부 consumer가 참조해 같은 public surface와
동작을 얻는지 확인한다.

## 8. 11.0 spec-first 정본 규칙

11.0의 Core service 이관과 service runtime 재구성은 정식 spec과 internals를 목표 상태의 정본으로 먼저
확정한다. 계획 문서, draft, 현재 구현과 다른 언어의 구현은 계약의 출처가 아니다. 구현은 승인된 정본과의
차이를 채우며, 구현 과정에서 계약을 바꿔야 할 제약이 확인되면 source를 우회하지 않고 영향을 받는 spec과
internals를 다시 검토해 함께 수정한다.

정식 spec은 사용자가 관찰하는 목표 계약만 설명한다. 현재 언어별 구현 누락, 제거 진행률과 test 상태는
언어별 audit·실행 ledger가 소유한다. Internals는 목표 runtime의
책임 경계, 데이터 흐름과 불변 조건을 설명하며 migration 이력이나 진행표를 포함하지 않는다.

Core는 raw transport를 소유하고, C++·.NET·JVM·Node.js Framework는 각 언어의 service runtime을
독립적으로 소유한다. 공통 native Framework runtime, private C SPI와 service C ABI를 만들지 않는다. 다섯
언어 public contract는 공통 Framework spec을 투영하고, 네 runtime은 공통 protocol schema와 fixture로
관찰 가능한 결과를 맞춘다. Java와 Kotlin은 public contract는 각각 제공하지만 Java binding과 JVM runtime
구현을 공유한다.

Core service runtime을 언어별 Framework 내부로 옮기는 작업은 기존 Framework public contract를 바꾸는
근거가 아니다. Channel·[Spot](01-glossary.ko.md#spot)·Actor·STREAM의 기존 public symbol, signature와 완료 의미는 그대로 유지한다.
11.0에서 공개 변경이 필요한 경우에는 maintenance처럼 새 application 의도나 기존 계약과 직접 충돌하는
기능 근거를 공통 spec에 별도로 제시해야 한다. 단순한 내부 이관에서 생긴 public rename, wrapper와
backend 선택 option은 허용하지 않는다.

공통 spec의 개념 이름과 자료형은 언어별 기존 공개 타입을 바꾸라는 지시가 아니다. 같은 의미를 이미
표현하는 public type, generic, callback 인자와 기본 interface 구현이 있으면 그 표면을 유지한다. 예를 들어
공통 Actor membership record를 새로 정의했다는 이유로 `.NET`의 typed
`IZLinkUserSpotActorLifecycle<TActor>`를 non-generic callback으로 바꾸지 않는다. 공통 의미를 기존 표면으로
표현할 수 없는 경우에만 별도 기능 근거와 POSD 검토를 거쳐 최소 member를 추가한다.

언어별 정확한 인터페이스는 `languages/<lang>/interfaces/` 아래의 범주별 문서가 소유한다. 파일 분할과
목차 변경은 API 변경이 아니다. 기존 단일 카탈로그를 분할할 때는 public member를 손실 없이 한 번만
옮기고, 같은 declaration을 여러 범주에 중복하지 않는다.
