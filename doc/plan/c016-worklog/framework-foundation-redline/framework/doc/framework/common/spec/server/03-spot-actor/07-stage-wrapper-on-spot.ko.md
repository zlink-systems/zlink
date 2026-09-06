---
title: "Stage wrapper on Spot"
---

# Stage wrapper on Spot

[Spot·Actor 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 06. Spot 주소 메시징](06-spot-address-messaging.ko.md) · [다음: 08. Spot·Actor routing](08-routing.ko.md)

> Framework의 public, 주소와 상태를 가진 논리 instance인
> [Spot](../00-foundation/02-glossary.ko.md#spot) 계약 위에 room·stage·zone 같은 상위
> 실행 모델을 만들 때, application의 domain wrapper가 지켜야 하는 실행 경계를 정의한다.

## 1. Stage wrapper 개요

Stage wrapper는 Spot이 소유한 상태를 안전하게 유지하면서, 선택한 User Spot 실행 모드에 맞는
Spot·Actor·timer 실행 경계를 보존해야 한다.

Framework는 별도 Stage runtime이나 공통 Stage base type을 제공하지 않는다. application의
domain wrapper가 Spot의 public 등록·메시징·timer·lifecycle 표면을 조합한다. 언어별 wrapper
형태는 각 언어의 공개 인터페이스 문서가 정한다.

## 2. 책임 경계

| 책임 | 소유자 |
|---|---|
| Spot identity, 생성·종료와 application turn | Framework Spot runtime이 소유한다. |
| Spot direct와 [Logical Multicast](../00-foundation/02-glossary.ko.md#logical-multicast)(ChannelName과 topic으로 같은 Channel의 여러 Spot에 message 하나를 전달하는 방식) dispatch | Framework Spot runtime이 소유한다. |
| timer admission과 callback turn | Framework Spot runtime이 소유한다. |
| Actor queue와 Actor 업무 handler | Framework Actor runtime이 소유한다. |
| Actor join·leave와 lifecycle control | Framework가 lifecycle 작업을 처리하는 Spot·Actor 전용 queue가 소유한다. |
| 입장 권한, stage state, membership 정책과 broadcast 내용 | Stage wrapper 또는 application이 소유한다. |
| domain key를 global Spot ID에 대응시키는 정책 | Stage wrapper와 application domain store가 소유한다. |

**Stage wrapper는 transport RID, endpoint, internal queue, native timer handle과 message storage
reference를 public surface에 노출하지 않는다.** wrapper의 공개 표면은 Framework의 public
Spot·Actor·timer·location 표면만으로 구성해야 하기 때문이다.

## 3. Spot turn 보존

Stage가 소유하는 상태를 읽거나 바꾸는 callback은 target Spot의 application turn, 즉 Spot callback
하나가 application queue의 execution gate를 점유해 실행되는 단위인
[Spot turn](../00-foundation/02-glossary.ko.md#spot-turn)에서 실행해야 한다. 다음 callback이 이
규칙의 대상이다.

- [Spot direct](../00-foundation/02-glossary.ko.md#spot-direct) handler
- Logical Multicast subscription handler
- Spot timer callback
- Actor join·leave와 lifecycle control callback
- Stage wrapper가 명시적으로 Spot에 제출한 domain operation

실행 mode의 등록은 [Spot 모델](01-spot-model.ko.md)을 따른다.
Gate·turn·continuation은 [실행 계약 §2](../01-execution/02-handler-turn-and-execution-gate.ko.md#execution-gate),
Yield와 Actor claim은 [§3](../01-execution/02-handler-turn-and-execution-gate.ko.md#yield-gate-and-claim)이 소유한다.

- **Wrapper는 별도 scheduler나 lock 규칙으로 이 실행 계약을 바꾸지 않는다.** Framework
  callback을 감싼 뒤에도 동일한 실행 순서가 유지되어야 하기 때문이다.
- **`PerActor`의 공유 Stage state 동기화는 application이 책임진다.** 실행 계약이 허용한
  서로 다른 gate의 callback이 같은 domain 상태에 접근할 수 있기 때문이다.

## 4. Actor 경계

Actor가 Stage 역할의 Spot에 join해도 Actor 업무 payload는 Actor queue로 직접 전달된다.
Actor payload를 Spot callback으로 변환하거나 Spot application queue에 넣지 않는다. 따라서 Actor
handler는 Stage의 mutable state를 직접 참조하지 않는다.

Actor가 Stage state를 바꾸려면 Stage Spot으로 명시적인 send/request를 제출한다. 해당 handler가
Spot turn에서, Actor가 현재 어느 Stage Spot에 속하는지를 나타내는
[membership](../00-foundation/02-glossary.ko.md#actor-membership)과 score, world state와
broadcast 결정을 수행한다.

Framework는 Actor의 join, leave, relocation과 lifecycle notification을 업무 message와 분리된
전용 queue에서 처리한다. 이 queue에서는 Actor의 일반 업무 handler를 실행하지 않으며, 업무
payload를 lifecycle callback으로 바꾸지도 않는다. 자세한 Actor queue와 lifecycle 처리 계약은
[Actor 모델](04-actor-model.ko.md)이 소유한다.

## 5. Timer와 Yield

Stage timer 등록과 tick 처리는 [Spot timer](10-spot-timer.ko.md)의 public 계약을 따른다.

- **Spot 종료가 신규 timer tick admission을 닫는다.** 이미 수락한 tick과 종료 callback의 순서는
  Spot lifecycle 규칙으로 정한다.
- fixed-rate, delay, catch-up과 overrun option은 언어별 timer 공개 계약으로 표현한다.
- wrapper는 native handle이나 scheduler thread를 application에 노출하지 않는다.

`Yield` 제공 범위와 재진입 오류는
[실행 계약 §16](../01-execution/02-handler-turn-and-execution-gate.ko.md#yield-call-eligibility)을 따른다.
Wrapper에 별도 Yield·Actor claim 규칙을 두지 않는다.

Host `Relocate`가 시작되어도 relocation을 시작할 실행 권한을 아직 얻지 못한 Stage Spot은 기존
message와 timer turn을 계속 처리한다. Framework가 relocation 준비 상태를 알리기 위해 사용하는
내부 notification은 application event가 아니므로 Stage callback을 실행하지 않는다.

실행 권한을 얻어 새 turn 수락을 닫은 뒤에는 아직 실행하지 않은 timer tick과 timer 등록 정보를
relocation payload에 포함한다. Target Framework가 이를 자동으로 복원하므로 Stage wrapper의
`Restore`가 같은 timer를 다시 등록하지 않는다.

## 6. 생성과 membership

Stage wrapper는 User Spot manager의 explicit Create·GetOrCreate에 stable type과 domain 생성
payload를 전달하고, 생성 callback 안에서 초기 Stage state를 만든다. 여러 node가 같은 Spot을
동시에 만들려고 해도 Framework는 생성 권한을 얻은 factory 하나만 실행한다. 새 작업을 허용하는
조건과 재활성 뒤 복원할 업무 상태는 domain 규칙으로 결정한다.

Actor join은 Framework의 lifecycle 전용 queue에서 Stage membership 정책을 검사한다. Join이
성공하면 Actor의 현재 Spot 위치와 Stage가 소유한 member state를 함께 갱신한다. 동시 변경을
하나로 확정하는 방법과 relocation 중 message 수락 경계는
[Spot과 Actor membership](05-spot-actor-membership.ko.md)이 소유한다.

Stage 전체 알림은 다음 중 의미에 맞는 경로를 사용한다.

- 같은 ChannelName의 여러 Spot에 알릴 때는 Logical Multicast를 사용한다.
- Stage 하나의 member state를 기준으로 알릴 때는 Spot turn에서 대상 Actor 또는 bound session을
  고르고 명시적인 메시지를 제출한다.

**Logical Multicast를 Stage member 목록의 durable source로 사용하지 않는다.**

## 7. Location과 수명

외부 service는 domain key에서 global [Spot ID](../00-foundation/02-glossary.ko.md#spot-id)를 얻어 Stage Spot에
메시지를 보낸다. 그 incarnation을 종료하거나 운영 정보로 표시할 때는 manager lookup이 반환한
`SpotRef`를 사용한다. owner RID와 endpoint는 wrapper 상태에 저장하지 않는다. 위치 갱신과 stale
route의 의미는 [Spot 주소 메시징](06-spot-address-messaging.ko.md)이 정한다.

Stage 종료는 신규 application admission과 신규 join을 닫고, 이미 수락한 Spot turn과 membership
정리를 drain deadline 안에서 완료한다. 종료 뒤의 timer, [subscription](../00-foundation/02-glossary.ko.md#subscription)과
direct 메시지는 Stage callback을 새로 만들지 않는다.

## 8. Metadata와 관측

Stage wrapper는 [메시지 모델](../00-foundation/05-message-model.ko.md)의 immutable metadata snapshot을
handler에 그대로 제공하고 transport frame이나 storage ownership을 해석하지 않는다.

관측 정보는 하나의 [RouteMesh](../00-foundation/02-glossary.ko.md#routemesh) 물리 연결 그룹을 식별하는
이름인 [MeshName](../00-foundation/02-glossary.ko.md#meshname), Stage type, Spot turn backlog, timer
지연, membership control 결과와 종료 state를 구분해야 한다. Stage ID와 Actor ID는 metric label로
사용하지 않는다.

## 9. 구현 및 contract test 검증 요구

공개 표면(Stage wrapper가 사용하는 Framework의 public Spot·Actor·timer·location 표면, Yield 반환
시점, submit 시점 오류)만으로 다음을 확인한다. 각 항목은 contract test 하나로 이어진다.

**Spot turn과 Yield**

Wrapper callback의 실행 순서·continuation·Yield·재진입 관찰은
[실행 계약 §16](../01-execution/02-handler-turn-and-execution-gate.ko.md#16-검증-요구)을 참조한다.

**Actor 경계**

- Actor payload가 Stage Spot callback이나 [Spot application queue](../00-foundation/02-glossary.ko.md#spot-application-queue)를
  거치지 않는다.
- Actor handler가 Stage state를 바꿀 때 명시적인 Spot 호출을 사용한다.
- Framework의 Spot lifecycle 전용 queue에는 join·leave와 lifecycle control만 포함하며 Actor
  업무 payload를 넣지 않는다.

**표면 준수와 종료**

- Stage wrapper가 Framework의 public Spot·Actor·timer·location 표면만 사용한다.
- Spot 종료 뒤 신규 timer와 message callback이 실행되지 않는다.
- Relocation permit 전에는 Stage Spot을 seal하지 않고, seal 뒤 timer registration과 pending
  tick을 target에서 자동 복원한다.

---

[Spot·Actor 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 06. Spot 주소 메시징](06-spot-address-messaging.ko.md) · [다음: 08. Spot·Actor routing](08-routing.ko.md)
