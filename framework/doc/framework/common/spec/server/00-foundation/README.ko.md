---
title: "Foundation"
---

# Foundation

[스펙 목차](../README.ko.md) · [다음: 01. 공개 계약 관리](01-public-contract-governance.ko.md)

> 이 스펙 전체가 공유하는 기반 — 계약을 누가 소유하는지, 공통 용어가 무엇을 뜻하는지,
> Framework가 상위에서 무엇을 하는지, operation의 대상과 완료를 어떻게 정하는지, 메시지의
> 형태와 public API family가 무엇인지, 공통 오류와 runtime 계층 경계가 무엇인지 — 이
> 주제는 그 기반 여덟 가지를 다룬다.

## 1. 무엇을 다루는가

다른 모든 주제(execution, channel-transport, spot-actor, session, location-relocation,
observability)는 이 주제가 정의하는 것을 전제로 쓴다. 공개 계약을 누가 소유하고 어떤
절차로 바꾸는지, 스펙 전체가 공유하는 domain term이 정확히 무엇을 뜻하는지, Framework가
언어마다 독립적으로 구현하는 상위 계층에서 무엇을 하는지, 메시지 하나를 보낼 때 대상을
어떻게 고르고 언제 완료로 보는지, 그 메시지의 typed payload·metadata·codec 형태가
무엇인지, application host가 root에 무엇을 등록해야 하는지, send·request가 실패하면
어떤 공통 오류를 받는지, runtime 코드가 어떤 덩어리로 나뉘고 어떤 값을 하나로 합치면
안 되는지를 정의한다.

이 주제는 "무엇을 계약하는가"를 정의하며, "그 계약을 어떻게 물리적으로 전달하는가"(연결
topology, wire framing)와 "그 계약 위에서, 주소와 상태를 가진 논리 instance인
[Spot](02-glossary.ko.md#spot)·Actor가 어떻게 사는가"(생성, relocation)는 각각
channel-transport·spot-actor 주제가 정의한다.

## 2. 이 주제의 문서

| 문서 | 다루는 것 |
|---|---|
| [01. 공개 계약 관리](01-public-contract-governance.ko.md) | 공개 계약의 정의, 소유권 4분류, 새 계약에 고정할 항목, 공개 계약 절차 7단계, 언어별 표현 원칙, 설계 검토 기준 |
| [02. Framework 메시징 용어집](02-glossary.ko.md) | 이 스펙 전체가 공유하는 domain term(Spot, Actor, owner, generation, authority …) 정의의 단일 기준 |
| [03. Framework 개요](03-overview.ko.md) | Framework가 하는 일, [MeshName](02-glossary.ko.md#meshname)·[ChannelName](02-glossary.ko.md#channelname)·[RouteMesh](02-glossary.ko.md#routemesh) — mesh·channel 범위를 식별하는 이름과 그 물리 연결 범위 —, 메시지 대상 선택, 실행 owner, 연결 관리, Framework가 숨기는 것 |
| [04. 상호작용 모델](04-interaction-model.ko.md) | Operation 대상 선택과 완료의 공통 모델, send/request, Spot [Logical Multicast](02-glossary.ko.md#logical-multicast), [classic fanout](02-glossary.ko.md#classic-fanout), [STREAM session](02-glossary.ko.md#stream-session), handler 실패와 종료의 영향 |
| [05. 메시지 모델](05-message-model.ko.md) | Typed 메시지, `MessageContext`, `ActorRef`/`SpotRef` JSON, `framework-json-v1` typed payload profile, application metadata, ownership과 크기 제한 |
| [06. Framework API](06-framework-api.ko.md) | 언어 중립 public API family — root 등록, RouteMesh 등록, 메시징 API, handler 등록·filter, codec, Store 등록, Spot·Actor·STREAM owner 등록, startup validation |
| [07. Framework 오류 모델](07-framework-error-model.ko.md) | 공통 `ErrorKind`, Send·Request의 완료·실패 경계, `CapacityExceeded` vs `Unavailable`, 재시도 판단 |
| [08. 계층 경계와 식별자](08-layering.ko.md) | 모든 언어 runtime이 따르는 binding 경계, 종료 절차와 정리 순서, 등록 선언 검증 시점, 식별자를 합치지 않는 기준(구현 스펙) |

## 3. 질문으로 찾기

| 질문 | 답이 있는 절 |
|---|---|
| 이 스펙 문서들은 왜 이렇게 나뉘어 있고, 계약과 구현은 어떻게 구분하는가 | [공개 계약 관리 「1. 공개 계약이란 무엇인가」](01-public-contract-governance.ko.md#1-공개-계약이란-무엇인가) · [「2. 계약 소유권」](01-public-contract-governance.ko.md#2-계약-소유권) |
| 새 public 계약을 추가하거나 바꾸려면 어떤 절차를 거치는가 | [공개 계약 관리 「5. 공개 계약 절차」](01-public-contract-governance.ko.md#5-공개-계약-절차) |
| 이 스펙 전체에서 반복해서 나오는 용어(Spot, Actor, owner, generation …)는 정확히 무엇을 뜻하는가 | [Framework 메시징 용어집](02-glossary.ko.md) |
| Framework는 무엇을 하는 계층이고, 언어마다 무엇을 독립적으로 구현하는가 | [Framework 개요 「1. Framework가 하는 일」](03-overview.ko.md#1-framework가-하는-일) |
| MeshName·ChannelName·RouteMesh는 각각 무엇을 가리키는가 | [Framework 개요 「2. MeshName·ChannelName·RouteMesh」](03-overview.ko.md#2-meshnamechannelnameroutemesh) |
| 메시지를 보낼 때 대상은 어떻게 정해지고, 언제 "완료됐다"고 보는가 | [상호작용 모델 「1. 공통 모델 — 대상 선택과 완료」](04-interaction-model.ko.md#1-공통-모델--대상-선택과-완료) |
| send와 request는 무엇이 다르고, 각각 어떤 조건에서 실패하는가 | [상호작용 모델 「4. Send와 request」](04-interaction-model.ko.md#4-send와-request) |
| Logical Multicast·classic fanout은 서로 무엇이 다른가 | [Framework 개요 「4. Logical Multicast와 classic fanout」](03-overview.ko.md#4-logical-multicast와-classic-fanout) · [상호작용 모델 「5. Spot Logical Multicast」](04-interaction-model.ko.md#5-spot-logical-multicast) · [「6. Classic fanout」](04-interaction-model.ko.md#6-classic-fanout) |
| 보낸 메시지의 형태(typed payload, metadata, reply)는 어떤 규칙을 따르는가 | [메시지 모델 「1. Typed 메시지」](05-message-model.ko.md#1-typed-메시지) |
| `ActorRef`·`SpotRef`의 JSON 표현과 codec 규칙은 무엇인가 | [메시지 모델 「4. Global object reference JSON」](05-message-model.ko.md#4-global-object-reference-json) · [「5. framework-json-v1 typed payload profile」](05-message-model.ko.md#5-framework-json-v1-typed-payload-profile) |
| Application host는 root에 무엇을 등록해야 Framework가 시작되는가 | [Framework API 「2. Root 등록」](06-framework-api.ko.md#2-root-등록) |
| Handler는 어떤 key로 등록되고 filter는 언제 적용되는가 | [Framework API 「9. Handler 등록과 dispatch」](06-framework-api.ko.md#9-handler-등록과-dispatch) · [「10. Handler filter」](06-framework-api.ko.md#10-handler-filter) |
| Send·Request가 실패하면 Application은 어떤 공통 오류를 받는가 | [Framework 오류 모델](07-framework-error-model.ko.md) |
| `CapacityExceeded`와 `Unavailable`은 어떻게 구분하는가 | [Framework 오류 모델 「5. Request 완료와 실패」](07-framework-error-model.ko.md#5-request-완료와-실패) |
| runtime 코드는 어떤 덩어리로 나뉘고, 어떤 값을 하나로 합치면 안 되는가 | [계층 경계와 식별자 「6. 식별자를 합치지 않는다」](08-layering.ko.md#6-식별자를-합치지-않는다) |
| startup에서 검증하는 것과 runtime에 검증하는 것은 어떻게 다른가 | [Framework API 「22. Startup validation」](06-framework-api.ko.md#22-startup-validation) · [계층 경계와 식별자 「5. 등록 선언은 시작할 때 한 번만 검증한다」](08-layering.ko.md#5-등록-선언은-시작할-때-한-번만-검증한다) |

## 4. 읽는 순서

**처음 읽는 개발자**

1. 이 문서 §1로 이 주제가 다루는 범위를 잡는다.
2. [Framework 개요](03-overview.ko.md)로 Framework가 상위에서 하는 일과 MeshName·
   ChannelName·RouteMesh, 실행 owner 개념을 읽는다.
3. [상호작용 모델 「1. 공통 모델」](04-interaction-model.ko.md#1-공통-모델--대상-선택과-완료) ~
   [「4. Send와 request」](04-interaction-model.ko.md#4-send와-request)로 메시지 대상 선택과
   완료 조건을 읽는다.
4. 모르는 용어가 나올 때마다 [용어집](02-glossary.ko.md)에서 정확한 정의를 확인한다.

**새 언어로 porting하는 개발자** — 아래 문서가 모든 runtime이 같은 구조로 따라야 하는
공개 계약과 구현 결정을 담고 있으므로, 언어별 구현 전에 반드시 읽는다.

1. [공개 계약 관리](01-public-contract-governance.ko.md) 전체 — 계약 소유권 경계와 공개
   계약 절차를 먼저 이해해야 새 언어의 언어별 interface를 어디에 기록할지 알 수 있다.
2. [Framework API](06-framework-api.ko.md) 전체 — public API family와 등록 규칙이 이
   주제에서 분량이 가장 크고, 다른 모든 언어 runtime이 이미 구현한 기준이다.
3. [Framework 오류 모델](07-framework-error-model.ko.md) 전체 — 공통 `ErrorKind`와 완료
   경계는 언어별 exception·result 표현이 그대로 따라야 하는 계약이다.
4. [계층 경계와 식별자](08-layering.ko.md) 전체 — binding 경계, 종료 절차와 식별자 분리는
   구현 스펙이며 검증 요구(§7)를 포함한다.

**application 개발자**

1. [Framework 개요](03-overview.ko.md)로 전체 그림을 잡는다.
2. [Framework API 「2. Root 등록」](06-framework-api.ko.md#2-root-등록) ~
   [「6. 메시징 API family」](06-framework-api.ko.md#6-메시징-api-family)로 root 등록과
   메시지를 보내는 public API를 읽는다.
3. [Framework 오류 모델](07-framework-error-model.ko.md)로 실패했을 때 받는 공통 오류를
   확인한다.

## 5. 이 주제가 정의하지 않는 것

| 내용 | 소유 문서 |
|---|---|
| RouteMesh·ClientServer의 물리 연결, wire framing, transport liveness | [channel-transport 주제](../02-channel-transport/README.ko.md) |
| Spot·Actor의 생성, membership과 relocation 절차 | [spot-actor 주제](../03-spot-actor/README.ko.md) |
| STREAM session과 Actor binding의 세부 절차 | [Session 주제](../04-session/README.ko.md) |
| Actor·Spot relocation의 source·target 실행 흐름 | [location-relocation 주제](../05-location-relocation/README.ko.md) |
| Runtime monitoring, message flow tracing, flow correlation | [observability 주제](../06-observability/README.ko.md) |
| Send·Request의 완료 경쟁, execution turn, backpressure 세부 규칙 | [execution 주제](../01-execution/README.ko.md) |

---

[스펙 목차](../README.ko.md) · [다음: 01. 공개 계약 관리](01-public-contract-governance.ko.md)
