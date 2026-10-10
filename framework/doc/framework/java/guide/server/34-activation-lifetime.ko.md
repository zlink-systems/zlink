---
title: "활성화와 수명 · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/34-activation-lifetime.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 활성화와 수명

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Backpressure — 처리보다 도착이 빠를 때](33-backpressure.ko.md) | [다음: Actor membership](35-actor-membership.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/34-activation-lifetime.ko.md) · [C#/.NET](../../../dotnet/guide/server/34-activation-lifetime.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/34-activation-lifetime.ko.md) · [Node/TypeScript](../../../node/guide/server/34-activation-lifetime.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    Spot 종류마다 언제 만들어지고 어떤 callback을 받는지, 그리고 그 안에 주입한 서비스가
    얼마나 사는지 알 수 있다.
    이 장의 코드는 [언어별 예제 저장소의 TicTacToe 샘플](https://github.com/zlink-systems/zlink-java-examples/tree/main/samples/TicTacToe)에서 가져온다.

[Spot](21-spot.ko.md)은 **application이 명시적으로 만드는 Spot**을 다뤘다. 이 장은
나머지 종류와의 차이, 종류마다 받는 lifecycle callback, 그리고 Spot이 살아 있는 동안 유지되는
주입 scope를 다룬다.

## 1. Spot의 종류

Spot은 종류와 무관하게 id와 상태를 가지고 callback을 순서대로 실행한다. 생성 시점, Actor membership,
종료 계약이 다르다.

| | Entry Spot | User Spot | Instance Spot |
| --- | --- | --- | --- |
| 생성 시점 | Object Server가 시작할 때 Framework가 만든다 | application이 spot manager로 명시적으로 만든다 | 그 id로 첫 메시지가 도착할 때 만든다 |
| Spot id | Framework가 발급한다 | 만들기는 Framework가, 찾아서 만들기는 호출하는 쪽이 정한다 | 호출하는 쪽이 메시지의 대상 id로 정한다 |
| stable type | 등록하지 않는다 | 필수다 | 필수다 |
| Actor membership | 지원한다. Actor가 만들어진 직후의 기본 실행 위치다 | 지원한다. Actor가 join·leave로 옮겨 다닌다 | 지원하지 않는다 |
| application이 닫기 | 제공하지 않는다 | 닫는 호출이나 자기 context에서 닫는다 | 자기 handler·timer context에서 닫는다 |
| 주로 사용하는 자리 | 아직 User Spot에 속하지 않은 Actor의 기본 위치 | room, stage, zone | matchmaking worker처럼 id 단위로 요청을 처리하는 단위 |

<iframe class="zlink-diagram" src="/common/diagrams/34-spot-kinds.html" title="세 종류의 Spot — 무엇이 만드는가" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/34-spot-kinds.html" target="_blank">↗ 크게 보기</a></p>

Instance Spot에는 만드는 호출이 따로 없다. 첫 메시지에 instance type을 적으면 Framework가 있던
instance를 고르거나 필요한 위치에 만든 뒤 **그 같은 메시지를 처리한다.**

### 1.1 Instance Spot의 단일 활성 범위

**같은 Location Store의 위치 정보를 공유하는 node들은 같은 Instance Spot id를 동시에
두 곳에서 활성화하지 않는다.** 현재 처리하는 node를 owner라고 한다. Framework는 위치
저장소에서 생성 권한을 먼저 확보한 node 하나만 factory를 실행하고, 생성 확정 뒤에 메시지를
처리한다. 이 보장은 해당 저장소의 위치 정보와 owner 자격을 기준으로 적용된다.
서로 독립된 Location Store를 사용하는 배포를 하나의 직렬 실행 단위로 묶지는 않는다.
Owner가 일정 기간 작업을 받을 자격은 갱신하는 owner lease로 확인한다.

단일 활성은 장애가 나도 계속 처리할 수 있다는 뜻이 아니다. 중복 생성 대신 대기하거나
실패하는 경우를 함께 고려해야 한다.

| 상황 | 메시지 처리 결과 |
| --- | --- |
| 서로 다른 node에 같은 id의 첫 메시지가 동시에 도착 | 생성 권한을 얻은 node만 만든다. 경쟁에서 진 node의 request는 `Unavailable`로 끝나며, 원래 deadline이 끝났으면 timeout을 유지한다. 이미 송신 수락이 끝난 one-way send의 실패는 diagnostics에 기록한다. |
| 같은 target에서 activation이 진행 중 | 뒤따른 요청은 같은 activation에 합류한다. `Ready` 뒤 도착 순서대로 queue에 들어가며, activation이 실패하면 원래 완료 계약으로 끝난다. Resolver가 생성 중인 위치를 발견한 호출도 activation을 기다린다. |
| `Ready` owner가 강제 종료되거나 owner lease가 무효 | 새 요청은 `Unavailable`이다. Lease가 남아 있는 동안뿐 아니라 만료된 뒤에도 위치 정보를 자동 해제하거나 다음 메시지로 다른 node에 재생성하지 않는다. |
| Location Store 연결 또는 변경 응답 유실 | 변경 결과를 확인하기 전에는 성공을 추측하거나 다른 node를 만들어 처리하지 않는다. 기존 node 목록을 유지하는 유예 시간인 `storeFailureGrace`는 owner 자격을 연장하지 않는다. 유효한 owner는 허용 시각까지 처리할 수 있지만, 그 시각을 넘으면 새 message·timer callback 시작과 상태 변경을 막는다. 이미 수락한 작업의 결과 처리와 정리는 진행할 수 있다. 결과 확인을 기다리는 request도 원래 timeout·취소·실패 완료 조건을 따른다. |
| 계획된 relocation 진행 중 | Source는 현재 callback을 마친 뒤 새 callback 실행을 멈춘다. 대기 작업과 새 메시지는 target으로 전달·보관하고, target은 복원과 owner 변경 확정 뒤 실행을 시작한다. 실패 시점에 따라 source를 유지하거나 작업이 실패하며, 두 node가 함께 handler를 실행하지 않는다. 상세 결과는 [Relocation](37-relocation.ko.md)이 다룬다. |

`Missing`은 위치 정보가 없는 상태이며, 이 상태에서만 Instance intent 메시지로 새 instance를
만든다. 기존 `Ready` instance는 명시적 close로 위치 해제가 끝나야 `Missing`이 된다.
`Ready`가 되기 전의 생성 복구는 같은
target 실행에서 이어질 수 있으며 factory가 같은 입력으로 다시 호출될 수 있다.
생성 중 target 실행이 끝난 reservation은 해제된 뒤 `Missing`이 된다.
따라서 factory의 외부 저장소 변경도 중복 호출을 고려해야 한다.

위치 저장소를 수동으로 비우거나 위치·세대 정보를 유실한 상황의 단일 활성은 보장 범위에
포함하지 않는다. 그 상태를 정상적인 close나 owner 장애 복구로 취급해서는 안 된다.

### 1.2 영속 상태와 공개 세대 값

**공동 잭팟처럼 돈이 걸린 영속 상태는 application 저장소에서도 보호한다.** Instance Spot은
그 Spot으로 들어오는 callback을 직렬로 실행한다. 다른 서비스의 DB 변경이나 이미 시작한
외부 I/O까지 하나의 transaction으로 묶지는 않는다. 잭팟 잔액 갱신과 업무 요청 id의 중복
기록을 같은 DB transaction에 두고, row lock 또는 조건부 갱신으로 다른 쓰기 경로도 보호한다.
Timeout이나 연결 유실 뒤에는 기존 요청의 결과를 조회하거나 같은 업무 요청 id로 중복 영향을
막은 뒤 새 요청을 시작한다.

공개 Instance Spot context에서 읽는 `ObjectGeneration`은 **같은 id의 재생성을 구분하는 값**이다.

`ZLinkInstanceSpotContext.objectGeneration()`으로 읽는다.

Relocation은 같은 object를 옮기므로 `ObjectGeneration`이 바뀌지 않는다. 따라서 이 값을
owner 교체를 구분하는 fencing token으로 사용하지 않는다. Fencing은 저장소가 이전 쓰기
권한으로 온 변경을 거부하는 방식이다. 그 보호가 필요하면 application 저장소에서 권한을
발급하고 모든 쓰기에서 검증한다. Framework context가 owner 교체용 token을 제공한다고
가정하지 않는다. 위치 저장소의 세대 정보를 유실했을 때도 이전 값과의 순서를 가정하지 않는다.

## 2. 종류마다 받는 lifecycle callback

이름은 언어를 따르고, 호출 조건과 순서는 같다.

| callback | Entry | User | Instance | 언제 |
| --- | :---: | :---: | :---: | --- |
| `configure` | O | O | O | handler를 등록하는 구성 단계 |
| `onCreate` | X | O | X | 새 User Spot 생성 요청을 확인하고 받아들일지 정한다. 있던 Spot을 찾은 경우에는 호출하지 않는다 |
| `onInitialize` | O | O | O | 만들어진 instance의 초기화. Instance Spot은 `onCreate` 없이 이것만 받는다 |
| `onClosing` | O | O | O | 아직 유효한 local instance가 정리되기 전 |
| `onActorJoin` | X | O※ | X | 이미 있는 Actor가 이 User Spot으로 오려 할 때 승인하거나 거절한다 |
| `onCreateActor` | O※ | X | X | 새 Actor의 최초 Entry Spot membership을 승인하거나 거절한다 |
| `onJoinedActor` | O※ | O※ | X | join commit이 끝났음을 **간 쪽** Spot에 알린다 |
| `onLeaveActor` | O※ | O※ | X | commit 뒤 **떠난 쪽** Spot에 알린다. Actor가 사라졌다는 뜻이 아니다 |
| `onDisconnectActor` | O※ | O※ | X | 그 Spot 소속 Actor의 연결이 끊겼을 때 |

※ Actor type을 지정해 Actor membership을 지원하는 Spot에만 해당한다.

**membership callback은 떠난 Spot과 간 Spot에서 나뉘어 실행된다.** 그래서 User Spot에 있던
Actor가 Entry Spot으로 돌아가도 **Entry Spot의 `onCreateActor`와 `onActorJoin`은 호출되지
않는다** — Entry Spot 복귀는 기본 membership이라 승인 절차가 없다. 양쪽 모두 commit 뒤 간 쪽의
`onJoinedActor`와 떠난 쪽의 `onLeaveActor`만 실행된다.

!!! info "relocation은 들어오고 나간 사건이 아니다"

    Relocation으로 Actor가 다른 node의 Entry Spot에 복원될 때는 이 callback을 호출하지 않는다.
    membership을 그대로 둔 채 실행 위치만 옮기는 것이기 때문이다 —
    [Relocation](37-relocation.ko.md)이 다룬다.

## 3. Entry Spot — Framework가 만드는 Spot

Object Server마다 하나이고, Actor 생성 요청을 승인하거나 거절하며 Actor가 들어오고 나가는
lifecycle을 처리한다.

```java
--8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/spots/entryspot/PlayEntrySpot.java:doc-entry-spot"
```

이 코드에서 Entry Spot은 Actor 생성 승인과 소멸에 필요한 lifecycle callback을 제공한다.

### 3.1 Entry Spot에는 Actor별 상태를 두지 않는다

**Entry Spot에는 Actor별 상태를 두지 않는다.** Actor의 상태는 Actor가 소유하고, Entry Spot은
handler와 membership callback만 제공한다. Entry Spot은 Object Server마다 하나뿐이므로 여기에
Actor마다의 값을 쌓으면 그 Object Server가 다루는 Actor 수만큼 자란다.

### 3.2 Actor의 생성과 소멸은 Entry Spot에서 진행된다

Actor 생성 요청은 Entry Spot이 승인하거나 거절한다. Actor를 소멸시키는 호출도 Entry Spot context가
제공한다. User Spot에 있는 Actor는 먼저 Entry Spot으로 돌아온 뒤 소멸시킨다. 그 이동은 Actor의
[membership](35-actor-membership.ko.md)이 처리한다.

소멸 호출은 현재 instance를 받고 membership callback을 다시 실행하지 않는다. Framework의 등록 기록과
묶인 session 경로를 정리한다.

## 4. User Spot — application이 만드는 Spot

stable type을 지정해 만들며, 돌아오는 id가 그 뒤 모든 호출의 주소다. 생성 callback에서 거절하면
호출이 실패하고 Spot은 남지 않는다.

```java
--8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/api/handlers/CreateGameHttpHandler.java:doc-create"
```

이 호출이 돌려준 id가 이후 User Spot을 호출하는 주소가 된다.

## 5. 주입한 서비스의 수명

Framework는 Spot을 활성화할 때 주입 scope를 하나 만들고, Spot 본체와 Spot handler의 의존성을 그
scope에서 만든다. Scope는 Spot이 닫히거나 다른 node로 옮겨 갈 때 함께 정리된다. 따라서
**scoped로 등록한 서비스는 그 Spot이 살아 있는 동안 instance 하나다.** HTTP 요청마다 새로
만들어지는 것과 다르다.

Actor handler는 별도의 Actor 활성화 scope를 사용한다. 서로 다른 Actor는 handler도 scoped 의존성도
공유하지 않는다. Actor가 떠나거나 사라지거나 옮겨 가면 출발 쪽 scope를 정리하고 도착 쪽에서 다시
만든다.

**ORM context를 Spot이나 Spot handler의 생성자로 주입하면 문제가 생긴다.** User Spot 하나가 몇 시간 유지되면 그 context도 같은 기간 유지된다.

| 증상 | 내용 |
| --- | --- |
| 메모리 증가 | change tracker가 조회한 entity를 계속 추적한다 |
| 오래된 값 조회 | 같은 키를 다시 조회해도 추적 중인 이전 instance를 돌려준다 |
| 오류 상태 고착 | 저장 실패로 context가 오염되면 Spot 수명 내내 복구되지 않는다 |

handler type을 transient나 singleton으로 등록해도 Framework가 정한 수명은 바뀌지 않는다.
Framework가 handler를 만들고 의존성만 활성화 scope에서 만들기 때문이다.

**저장소에 Spot에서 직접 접근하지 않는 편을 먼저 고른다.** 저장과 조회는 channel handler가 있는
서비스에 요청하고, Spot은 메모리 상태와 실행 순서만 소유한다. Channel handler는 dispatch마다
scope를 가지므로 ORM을 생성자로 받아도 된다 —
[Channel 메시징](20-channel-messaging.ko.md)의 handler가 그 자리다.

## 6. 관련 문서

- id로 호출하는 상태 객체 — [Spot](21-spot.ko.md) · [Actor](22-actor.ko.md)
- 무엇이 함께 실행되는가 — [실행 모델](32-execution-model.ko.md)
- Actor가 Spot 사이를 옮기는 규칙 — [Actor membership](35-actor-membership.ko.md)
- 실행 위치를 옮기는 것 — [Relocation](37-relocation.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
