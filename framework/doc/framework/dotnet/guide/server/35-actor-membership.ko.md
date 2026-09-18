---
title: "Actor membership · C#/.NET"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/35-actor-membership.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Actor membership

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 활성화와 수명](34-activation-lifetime.ko.md) | [다음: Timer와 worker](36-timer-worker.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/35-actor-membership.ko.md) · **C#/.NET** · [Java](../../../java/guide/server/35-actor-membership.ko.md) · [Kotlin](../../../kotlin/guide/server/35-actor-membership.ko.md) · [Node/TypeScript](../../../node/guide/server/35-actor-membership.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    Actor를 Spot 사이로 옮기고, 그 이동을 받는 쪽에서 승인하거나 거절할 수 있다.
    이 장의 코드는 TicTacToe 샘플에서 가져왔다.

[Actor](22-actor.ko.md)는 언제나 어떤 Spot 안에 있고, 만들어진 직후에는 Entry Spot에 있다.
이 장은 **그 Actor를 User Spot으로 옮기는 절차**를 다룬다 — 게임 방이 그 예다 — 누가 승인하고, 언제 실행되며, 무엇이
그 이동을 막는가.

## 1. 이동의 대상 — 실행 위치만 바뀐다

Actor가 User Spot에 들어간다는 것은 **그 Actor의 callback이 실행되는 Spot이 바뀐다**는 뜻이다. Actor
자체는 그대로 있고, id도 그대로다. User Spot을 떠나면 Entry Spot으로 돌아간다.

받는 쪽 Spot이 먼저 승인하거나 거절하고, 승인한 뒤 membership이 확정되면 간 쪽에 알림이 간다.

| 단계 | 어디서 | 무엇을 정한다 |
| --- | --- | --- |
| 승인 | 가려는 User Spot의 `OnActorJoinAsync` | 받아들일지, 어떤 답을 함께 돌려줄지 |
| 확정 | Framework | 위치 기록과 membership을 함께 바꾼다 |
| 알림 | 간 쪽의 `OnJoinedActorAsync`, 떠난 쪽의 `OnLeaveActorAsync` | 확정 뒤에 실행된다 |

Entry Spot으로 돌아가는 길에는 승인 절차가 없다. 기본 membership이기 때문이다 —
[활성화와 수명](34-activation-lifetime.ko.md)이 종류별 callback을 다룬다.

<iframe class="zlink-diagram" src="/common/diagrams/35-actor-join.html" title="join은 예약이고, 실행은 handler가 끝난 뒤다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/35-actor-join.html" target="_blank">↗ 크게 보기</a></p>

## 2. 예약 등록 — handler가 끝난 뒤에 실행된다

join 호출에는 결과를 그 자리에서 기다리는 형태가 없다. **예약만 하고 지금 handler를
끝낸다.** 예약은 handler가 정상적으로 끝난 뒤에 실행된다.

```csharp
--8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/PlayActorJoinGameHandler.cs:doc-join-defer"
```

### 2.1 기다리는 형태를 제공하지 않는 이유

join은 그 Actor의 위치와 membership을 바꾼다. 가려는 Spot의 owner가 다른 node면 같은 동작 안에서
Actor의 이동까지 수행하므로, 위치 조회와 받는 쪽 승인과 기록 확정이 모두 들어간다.

**그 완료를 지금 turn 안에서 기다리면 자기 자신을 막는다.** Actor는 자기 queue의 작업을 한 번에
하나씩 실행한다. 지금 실행 중인 handler가 join 완료를 기다리면, 그 join은 이 handler가 끝나야
진행할 수 있으므로 양쪽이 서로를 기다린다.

### 2.2 예약한 뒤 handler가 끝나는 방식

예약이 실행될지는 handler가 어떻게 끝나느냐가 정한다.

handler가 정상적으로 끝나면 예약한 join이 활성화되어 실행을 시작한다. 예외·취소·응답 인코딩
실패로 끝나면 예약을 폐기하고 join은 시작되지 않는다.

예약은 **지금 handler의 등록 구간이 열려 있는 동안에만** 등록할 수 있다. handler가 끝난 뒤나
handler와 분리해 실행하는 background 작업에서 호출하면 `InvalidOperation`이다.

| 호출할 수 있다 | 호출할 수 없다 |
| --- | --- |
| Actor의 send·request handler | factory와 구성 단계 |
| User·Entry Spot의 packet·request·구독·timer handler | lifecycle callback |
| | relocation adapter |
| | Instance Spot handler |
| | handler에서 떼어낸 background 작업 |

!!! warning "분리해 실행한 작업의 예약 호출은 검출을 보장하지 않는다"

    handler가 끝나기 전에 검출되지 않을 수 있다. 위 표에서 "호출할 수 없다"로 적은 자리에서는
    호출하지 않는다.

같은 호출에서 예약을 두 번 등록하면 `InvalidOperation`이고, 그 Actor에 이미 다른 membership 전환이
걸려 있으면 `Unavailable`이다. **이미 그 Spot에 속한 Actor가 같은 Spot에 join하면** 위치를 바꾸지
않고 성공으로 끝난다 — 기록도 membership도 건드리지 않고 승인·확정·이탈 callback도 실행하지
않는다.

### 2.3 결과를 받는 자리

결과는 Actor의 join 완료 callback으로 온다. **어느 Actor가 그 callback을 실행하는지는 결과가
정한다.**

| 결과 | 실행하는 Actor |
| --- | --- |
| 받아들여짐 | 위치 변경을 확정한 **간 쪽** Actor |
| 거절됨, 확정 전 실패 | 그대로 남은 **떠난 쪽** Actor |

다른 node의 Spot으로 가는 join이 성공하면 완료를 받는 것은 도착 node의 Actor다. 그래서 join을
예약한 handler 안에서 결과를 받는 형태가 성립하지 않는다 — 그 handler가 있던 Actor는 그 시점에
이미 정리 중이다.

예약이 활성화된 뒤에 도착한 일반 message는 완료 callback보다 먼저 실행되지 않는다. join이
끝날 때까지 그 Actor의 일반 처리는 대기한다.

완료 callback은 재시도된 결과인지 구분하는 id를 함께 받는다. 같은 id의 callback이 다시 실행되어도
안전하도록 처리한다.

User Spot에서 Entry Spot으로 돌아갈 때도 같은 방식이다.

## 3. 한 handler가 예약할 수 있는 양

| 무엇 | 상한 |
| --- | --- |
| 한 handler의 join 예약 수 | 64개 |
| join 요청 하나의 인코딩 크기 | 1 MiB |
| 한 handler가 예약한 요청 크기의 합 | 8 MiB |
| 다른 node로 가는 join의 응답 | 1 MiB |
| timeout 기본값 | 5초. 지정하면 유한한 양수여야 한다 |

**상한을 넘기면 그 자리에서 오류로 끝난다.** 일부만 등록되고 나머지가 빠지는 상태는 만들지
않는다. 요청과 응답의 상한은 서로 독립이라 하나로 합쳐 계산하지 않는다.

## 4. 예약한 Actor에 대한 요청 제한

예약을 등록한 Actor에게 **같은 handler에서 요청을 보내고 응답을 기다리면 순환 대기**가 된다. 요청은
예약 뒤에서 기다리고, 예약은 이 handler가 끝나야 열리는데, handler는 응답을 기다리느라 끝나지
못한다.

Framework가 그 요청을 **제출하기 전에 `InvalidOperation`으로 거절한다.** 멈추지 않고 오류로
끝나므로, 이 오류를 보면 예약과 요청의 대상이 같은 Actor인지 확인한다.

## 5. 예약이 살아남지 못하는 경우

예약은 **지금 process의 메모리에만 있다.** join이 실행되거나 기록에 반영되기 전에 process가 종료되면 그 예약은 재생되지 않는다. Actor의 위치와 membership은 원래대로 남는다 — 절반만 옮겨진
상태로 남지 않는다.

이동이나 종료 절차와 겹치면 **먼저 확정된 쪽을 따른다.**

| 먼저 확정된 것 | 결과 |
| --- | --- |
| join | 유지 보수 절차가 join이 끝날 때까지 기다린다 |
| relocation의 차단 | join이 `Unavailable`로 끝난다 |
| shutdown의 차단 | join이 `ShuttingDown`으로 끝난다 |

## 6. User Spot 안의 Actor에게 보내기

Actor가 어느 Spot과 node에 있는지 몰라도 actor id로 message를 보낸다. User Spot에 있는 Actor 앞으로 온 packet은 그 Spot의 handler가 받는다.

```csharp
--8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/PlayActorPlaceMarkHandler.cs:doc-actor-packet-handler"
```

이 handler는 Spot과 Actor를 함께 받는다. Spot의 execution mode가 `SpotWide`면 Spot의 상태와
Actor의 상태를 한 turn 안에서 함께 다룰 수 있다 — [실행 모델](32-execution-model.ko.md)이 그 범위를
다룬다.

### 6.1 옮겨 가는 중에 보낸 message

Actor가 다른 node로 옮겨 가는 중에도 보내는 쪽은 actor id만 지정한다. Framework는 호출할 때마다
기록에 남은 **지금 owner**를 다시 조회해 그 node로 보낸다.

옮겨 가기 직전의 위치를 들고 있던 쪽이 이전 owner로 보낸 message도 버려지지 않는다. **그
message를 받은 이전 owner가 새 owner에게 대신 전달한다** — 보내는 쪽에 새 주소를 알려 주고 다시
보내게 하는 방식이 아니라, 받은 node가 넘겨주는 방식이다. 이 전달에는 유효 구간이 있고, 그
뒤에 도착한 message는 경로가 낡았을 때의 일반 실패로 처리한다. Application은 node 식별자를
추적하지 않는다 — [Relocation](37-relocation.ko.md#5-절차와-되돌릴-수-있는-지점)이 그 구간을
다룬다.

**옮겨 가는 중에 보낸 요청도 원래 보낸 쪽에서 완료된다.** 도착 쪽이 만든 응답은 원래 보낸 쪽으로
연결되고, timeout은 보낸 쪽의 기존 경로를 그대로 따르며, 늦게 도착한 응답은 버린다. 이동 중에
응답을 기다리는 요청 수는 runtime metric으로 관측한다 —
[운영과 lifecycle](12-operations.ko.md#1-런타임-메트릭)이 그 자리다.

## 7. 관련 문서

- id로 호출하는 대상 — [Actor](22-actor.ko.md)
- 옮겨 갈 자리 — [Spot](21-spot.ko.md)
- 종류별 membership callback — [활성화와 수명](34-activation-lifetime.ko.md)
- 실행 위치를 옮기는 또 다른 절차 — [Relocation](37-relocation.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
