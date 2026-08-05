---
title: "Stage wrapper on Spot"
---

# Stage wrapper on Spot

[스펙 목차](README.ko.md) · [이전: Spot 주소 메시징](16-spot-address-messaging.ko.md) · [다음: Spot·Actor routing](18-object-routing.ko.md)

> **이 장이 정의하는 것** — public Spot 계약 위에 room·stage·zone 같은 상위 실행
> 모델을 만드는 공통 계약.


## 1. 이 문서가 정의하는 범위

이 문서는 ZLink Framework의 public Spot 계약 위에 room·stage·zone 같은 상위 실행 모델을 만드는
공통 계약을 정의한다. Stage wrapper는 [Spot](01-glossary.ko.md#spot)이 소유한 상태를 안전하게
유지하면서, 선택한 User Spot 실행 모드에 맞는 Spot·Actor·timer 실행 경계를 보존해야 한다.

Framework는 별도 Stage runtime이나 공통 Stage base type을 제공하지 않는다. application의 domain
wrapper가 Spot의 public 등록·메시징·timer·lifecycle 표면을 조합한다. 언어별 wrapper 형태는 각 언어의
공개 인터페이스 문서가 정한다.

## 2. 책임 경계

| 책임 | 소유자 |
|---|---|
| Spot identity, 생성·종료와 application turn | Framework Spot runtime이 소유한다. |
| Spot direct와 Logical Multicast dispatch | Framework Spot runtime이 소유한다. |
| timer admission과 callback turn | Framework Spot runtime이 소유한다. |
| Actor queue와 Actor 업무 handler | Framework Actor runtime이 소유한다. |
| Actor join·leave와 lifecycle control | Framework가 lifecycle 작업을 처리하는 Spot·Actor 전용 queue가 소유한다. |
| 입장 권한, stage state, membership 정책과 broadcast 내용 | Stage wrapper 또는 application이 소유한다. |
| domain key를 global Spot ID에 대응시키는 정책 | Stage wrapper와 application domain store가 소유한다. |

Stage wrapper는 transport RID, endpoint, internal queue, native timer handle과 message storage reference를
public surface에 노출하지 않는다.

## 3. Spot turn 보존

Stage가 소유하는 상태를 읽거나 바꾸는 callback은 target Spot의 application turn에서 실행해야 한다.

- [Spot direct](01-glossary.ko.md#spot-direct) handler
- Logical Multicast subscription handler
- Spot timer callback
- Actor join·leave와 lifecycle control callback
- Stage wrapper가 명시적으로 Spot에 제출한 domain operation

User Spot의 실행 mode는 factory를 등록할 때 고정한다. 기본 `SpotWide`에서는 위
작업과 member Actor handler가 같은 Spot gate를 사용하므로 callback 두 개가 동시에
Spot 상태를 변경하지 않는다.

`PerActor`에서는 Spot direct·Logical Multicast·lifecycle control이 Spot lane에서
직렬화되고 각 member Actor의 payload는 Actor별 lane에서 직렬화된다. 같은 timer의
callback은 timer별 lane에서 직렬화된다. 서로 다른 Actor lane, Spot lane과 서로 다른
timer lane은 동시에 실행될 수 있으므로 application이 공유 Stage state의 동기화를
책임진다.

Callback이 비동기 작업을 기다리는 동안 turn을 유지하거나 반납하는 의미는
[비동기 실행 정책](05-async-execution-policy.ko.md)이 정한다. Wrapper는
별도 scheduler나 lock 규칙으로 그 계약을 바꾸지 않는다.

request reply continuation이 Spot 상태를 바꾸면 원래
[Spot turn](01-glossary.ko.md#spot-turn)으로 다시 제출되어야 한다. transport 또는
completion thread에서 Spot 상태를 직접 변경하지 않는다.

## 4. Actor 경계

Actor가 Stage 역할의 Spot에 join해도 Actor 업무 payload는 Actor queue로 직접 전달된다. Actor payload를
Spot callback으로 변환하거나 Spot application queue에 넣지 않는다. 따라서 Actor handler는 Stage의
mutable state를 직접 참조하지 않는다.

Actor가 Stage state를 바꾸려면 Stage Spot으로 명시적인 send/request를 제출한다. 해당 handler가 Spot
turn에서 [membership](01-glossary.ko.md#membership), score, world state와 broadcast 결정을 수행한다.
`SpotWide`에서는 Actor handler도 같은 Stage 공통 execution gate를 사용한다.
`PerActor`에서는 Actor별 gate와 Spot lane이 독립적으로 실행될 수 있으므로 Actor
handler가 mutable Stage state를 직접 참조하지 않는 경계를 유지한다.

Framework는 Actor의 join, leave, relocation과 lifecycle notification을 업무
message와 분리된 전용 queue에서 처리한다. 이 queue에서는 Actor의 일반 업무
handler를 실행하지 않으며, 업무 payload를 lifecycle callback으로 바꾸지도 않는다.
자세한 Actor queue와 lifecycle 처리 계약은
[22 Actor 모델](14-actor-model.ko.md)이 소유한다.

## 5. Timer

Stage timer는 Spot lifecycle 안에서 등록하고 tick을 Spot application queue에
제출한다. `SpotWide`에서는 timer callback이 Spot direct, member Actor와 다른 timer
callback을 포함한 Stage 전체와 직렬화된다. `PerActor`에서는 같은 timer callback만
직렬화하고 서로 다른 timer, Actor lane과 Spot lane은 동시에 실행할 수 있다.

- Spot 종료가 신규 timer tick admission을 닫는다.
- 이미 수락한 tick과 종료 callback의 순서는 Spot lifecycle 규칙으로 정한다.
- fixed-rate, delay, catch-up과 overrun option은 언어별 timer 공개 계약으로 표현한다.
- wrapper는 native handle이나 scheduler thread를 application에 노출하지 않는다.

`Yield`는 `SpotWide` User Spot과 Instance Spot callback에서만 현재 Spot gate를
반납한다. `SpotWide` Stage에서는 Channel·Spot·Actor request 또는 CPU·I/O worker
결과를 기다릴 때 이를 사용할 수 있다. Continuation은 같은 execution gate에서 새
turn으로 재개한다. `PerActor` Stage와 Entry Spot callback에서는 `Yield`를 사용할 수
없다.

Member Actor handler가 `Yield`할 때도 현재 Actor queue head를 실행할 권한은 유지한다. 다른 Actor, Spot
handler와 timer는 Stage 공통 gate를 사용할 수 있지만 같은 Actor의 다음 job은 continuation이 gate를 다시
얻어 현재 job을 완료할 때까지 실행하지 않는다. 같은 Actor 자신에게 보낸 request도 현재 queue head를
앞질러 실행하거나 inline으로 재진입하지 않는다.

같은 gate가 필요한 request를 `Async`로 기다리거나 자신에게 보낸 request를
기다리는 호출은 submit 전에 `InvalidOperation`으로 거부한다.

Host `Relocate`가 시작되어도 relocation을 시작할 실행 권한을 아직 얻지 못한 Stage
Spot은 기존 message와 timer turn을 계속 처리한다. Framework가 relocation 준비
상태를 알리기 위해 사용하는 내부 notification은 application event가 아니므로 Stage
callback을 실행하지 않는다.

실행 권한을 얻어 새 turn 수락을 닫은 뒤에는 아직 실행하지 않은 timer tick과 timer
등록 정보를 relocation payload에 포함한다. Target Framework가 이를 자동으로
복원하므로 Stage wrapper의 `Restore`가 같은 timer를 다시 등록하지 않는다.

## 6. 생성과 membership

Stage wrapper는 User Spot manager의 explicit Create·GetOrCreate에 stable type과
domain 생성 payload를 전달하고 생성 callback 안에서 초기 Stage state를 만든다.
여러 node가 같은 Spot을 동시에 만들려고 해도 Framework는 생성 권한을 얻은 factory
하나만 실행한다. 새 작업을 허용하는 조건과 재활성 뒤 복원할 업무 상태는 domain
규칙으로 결정한다.

Actor join은 Framework의 lifecycle 전용 queue에서 Stage membership 정책을
검사한다. Join이 성공하면 Actor의 현재 Spot 위치와 Stage가 소유한 member state를
함께 갱신한다. 동시 변경을 하나로 확정하는 방법과 relocation 중 message 수락
경계는 [23 Spot Actor](15-spot-actor.ko.md)가 소유한다.

Stage 전체 알림은 다음 중 의미에 맞는 경로를 사용한다.

- 같은 ChannelName의 여러 Spot에 알릴 때는 Logical Multicast를 사용한다.
- Stage 하나의 member state를 기준으로 알릴 때는 Spot turn에서 대상 Actor 또는 bound session을 고르고
  명시적인 메시지를 제출한다.

Logical Multicast를 Stage member 목록의 durable source로 사용하지 않는다.

## 7. Location과 수명

외부 service는 domain key에서 global [Spot ID](01-glossary.ko.md#spot-id)를 얻어 Stage Spot에 메시지를 보낸다. Exact incarnation을
종료하거나 운영 정보로 표시할 때는 manager lookup이 반환한 `SpotRef`를 사용한다. Owner RID와 endpoint는
wrapper 상태에 저장하지 않는다. 위치 갱신과 stale route의 의미는
[24 Spot 주소 메시징](16-spot-address-messaging.ko.md)이 정한다.

Stage 종료는 신규 application admission과 신규 join을 닫고, 이미 수락한 Spot turn과 membership 정리를
drain deadline 안에서 완료한다. 종료 뒤의 timer, [subscription](01-glossary.ko.md#subscription)과 direct 메시지는 Stage callback을 새로
만들지 않는다.

## 8. Metadata와 관측

Stage wrapper는 [메시지 모델](04-message-model.ko.md)의 immutable metadata
snapshot을 handler에 그대로 제공하고 transport frame이나 storage ownership을
해석하지 않는다.

관측 정보는 MeshName, Stage type, Spot turn backlog, timer 지연, membership control 결과와 종료 state를
구분해야 한다. Stage ID와 Actor ID는 metric label로 사용하지 않는다.

## 9. 구현 및 contract test 검증 요구

- Spot direct, Logical Multicast, timer와 explicit Stage operation이 같은 Spot turn을 보존한다.
- `SpotWide`에서 member Actor handler도 같은 Spot gate를 사용한다.
- `PerActor`에서 Spot lane, Actor별 lane과 timer별 lane이 각자의 FIFO 직렬성을
  유지하면서 서로 동시에 실행될 수 있다.
- `SpotWide` Actor의 `Yield`는 Spot gate만 반납하고 Actor queue head의 실행 권한은
  유지한다.
- 같은 gate가 필요한 `Async`와 self-awaited request는 submit 전에
  `InvalidOperation`으로 거부한다.
- Actor payload가 Stage Spot callback이나 [Spot application queue](01-glossary.ko.md#spot-application-queue)를 거치지 않는다.
- Actor handler가 Stage state를 바꿀 때 명시적인 Spot 호출을 사용한다.
- Framework의 Spot lifecycle 전용 queue에는 join·leave와 lifecycle control만
  포함하며 Actor 업무 payload를 넣지 않는다.
- request continuation이 transport thread에서 Stage state를 직접 변경하지 않는다.
- Stage wrapper가 Framework의 public Spot·Actor·timer·location 표면만 사용한다.
- Spot 종료 뒤 신규 timer와 message callback이 실행되지 않는다.
- Relocation permit 전에는 Stage Spot을 seal하지 않고, seal 뒤 timer registration과 pending tick을 target에서 자동
  복원한다.
