# Relocation

!!! info "이 장을 읽고 나면"

    살아 있는 Spot과 Actor를 다른 node로 옮길 수 있고, 옮기는 동안 무엇이 유지되고 무엇을
    application이 직접 넘겨야 하는지 알 수 있다. 이 장의 코드는 TicTacToe 샘플에서 가져왔다.

[Spot](21-spot.ko.md)과 [Actor](22-actor.ko.md)는 만들어진 node에서 계속 실행되는 것으로 다뤘다.
운영에서는 그 node를 종료해야 하는 때가 있다. **Relocation은 논리 id를 그대로 둔 채 실행 위치만
옮기는 절차**이고, 이 장은 그 절차와 application이 맡는 부분을 다룬다.

## 1. 옮겨도 그대로 남는 것

호출하는 쪽이 사용하던 것은 바뀌지 않는다.

| 유지되는 것 | 뜻 |
| --- | --- |
| spot id · actor id와 세대 값 | 호출하는 쪽이 사용하던 논리 id가 그대로다 |
| 아직 실행하지 않은 message와 받아들인 기록 | 차단 시점에 queue에 남아 있던 작업을 도착 쪽에서 이어서 실행한다 |
| timer 등록과 대기 중인 tick | 이름·주기·옵션·커서를 함께 옮기므로 도착 쪽에서 다시 등록하지 않는다 |
| 묶인 STREAM session의 경로 | client 연결은 그대로 두고 경로가 새 owner를 가리키도록 바꾼다 |
| application 상태 | factory에 등록한 adapter가 담고 푼다 — 이 부분만 application의 몫이다 |

논리 id가 그대로이므로 주소를 다시 알릴 필요가 없다.

**이동 중인 상태는 Location Store를 거치지 않는다.** 떠나는 node가 도착 node로 mesh 연결을 통해
직접 보낸다.

<iframe class="zlink-diagram" src="/common/diagrams/37-relocation-move.html" title="논리 id는 그대로, 실행 위치만 옮긴다" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/37-relocation-move.html" target="_blank">↗ 크게 보기</a></p>

## 2. application이 맡는 부분 — adapter

Adapter는 **application 상태만** 바이트로 담고 푼다. 위치 권한, queue, timer, 받아들인 기록,
session 경로는 Framework가 처리한다.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Actors/PlayActorRelocationAdapter.cs:doc-relocation-adapter"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Actors/player_actor_relocation_adapter.hpp:doc-relocation-adapter"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/actors/PlayActorRelocationAdapter.java:doc-relocation-adapter"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/actors/PlayActorRelocationAdapter.kt:doc-relocation-adapter"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Actors/play-actor-relocation-adapter.ts:doc-relocation-adapter"
    ```

Adapter는 같은 이동에서 다시 호출될 수 있다. **담기와 풀기는 두 번 실행해도 같은 결과를
내야 한다.** 넘겨받은 바이트를 callback 밖에서 들고 있으려면 복사해 둔다.

### 2.1 factory 등록이 고르는 이동 정책

정책은 factory를 등록할 때 하나로 정하고 실행 중에 바꾸지 않는다. Actor가 다른 node의 Spot으로
join할 때와 운영에서 node를 비울 때 모두 이 정책을 따른다.

| 정책 | 도착 쪽에서 무엇이 되나 |
| --- | --- |
| 이동하지 않음 | 다른 node로 옮기기를 시작하기 전에 거절한다. 이 대상이 남아 있으면 node 비우기를 끝낼 수 없다 |
| 새로 만듦 | 같은 논리 id로 새 instance를 만든다. 대기하던 message와 timer는 유지하고 application 상태는 복원하지 않는다 |
| adapter로 상태 보존 | adapter가 담은 바이트를 새 instance에 푼다. queue와 timer도 함께 유지한다 |

등록 호출의 이름은 언어를 따른다 — [Spot](21-spot.ko.md)의 등록이 그 자리다.

## 3. 상태를 담는 시점 — factory 등록이 정한다

Framework는 떠나는 node에서 새 turn 받기를 닫고, adapter로 상태를 담고, 도착 node에서 푼 뒤
권한을 넘긴다. **상태를 담는 그 시점**을 누가 정하는지는 factory 등록에서 고른다.

| 모드 | 시점을 정하는 쪽 | 사용하는 자리 |
| --- | --- | --- |
| Framework가 정함(기본) | Framework — 끝난 turn과 다음 turn 사이 | 대부분의 Spot |
| application이 신호함 | application — 신호한 turn의 끝 | 상태의 일관성 단위가 여러 turn에 걸치는 Spot |

**기본 모드가 성립하는 조건.** Framework는 실행 중인 turn을 중단하지 않는다. handler 하나와 tick
하나는 끝난 뒤에야 상태를 담으므로, 상태 변경이 한 turn 안에서 끝나면 turn 경계에서 담은 상태는
언제나 일관된다.

**기본 모드가 성립하지 않는 조건.** 일관성 단위가 여러 turn에 걸쳐 있으면 turn 경계에서 담은
상태가 불완전할 수 있다. FPS 라운드가 그런 예다 — 시작 tick, 여러 입력 packet, 정산 tick으로
이뤄지고 그 중간 상태는 복원해도 라운드를 이어서 진행할 수 없다. **Framework는 turn 경계는 알지만
application이 정의한 일관성 단위는 알지 못한다.**

application이 신호하는 모드를 등록하면 Framework는 상태를 스스로 담지 않고 신호한 시점까지
기다린다. 이 모드는 `SpotWide` User Spot에서만 사용할 수 있다 —
[실행 모델](32-execution-model.ko.md)이 그 경계를 다룬다.

## 4. execution mode가 정하는 이동 단위

`SpotWide` User Spot은 Spot과 소속 Actor를 **하나의 단위로 함께** 옮긴다. Entry Spot과
`PerActor` User Spot은 Actor를 각각 옮긴다.

**이 이동은 join·leave callback을 호출하지 않는다.** membership을 그대로 둔 채 실행 위치만 바꾸는
것이라 application이 보기에 들어오거나 나간 사건이 아니다 —
[Actor membership](35-actor-membership.ko.md)이 그 사건 쪽을 다룬다.

## 5. 절차와 되돌릴 수 있는 지점

1. 먼저 옮길 대상과 도착 후보의 적격성·여유를 확인한다. 받을 곳이 없으면 떠나는 쪽의 상태를 바꾸지 않고 실패로 끝난다.
2. host를 이동 중으로 알리고 각 실행 queue에 알림을 예약한다.
3. 알림이 turn 경계에 닿으면 실행 중인 turn만 끝내고 새 turn은 시작하지 않는다. 받기는 열어 둔 채 이후 message를 떠나는 쪽에 보류한다.
4. 차단 시점에 남은 message, 받아들인 기록, timer 등록과 대기 tick, 담은 상태를 도착 쪽으로 직접
   보낸다.
5. 권한과 membership을 바꾼다.
6. 도착 쪽이 받을 준비를 보고하면 보류한 message를 전달하고 전환을 지시한다. 도착 쪽이 순서대로
   밀린 것을 합치고 필수 callback을 끝낸 뒤 dispatch를 연다.
7. 모든 단위의 dispatch가 끝나면 host를 이동 완료로 바꾼다. 연결과 인프라는 종료 호출 전까지
   유지한다.

**첫 확정 전의 실패는 떠나는 쪽의 queue와 받기를 되돌릴 수 있다.** 첫 확정 뒤에는 되돌리지 않고
도착 쪽 복구를 계속하며, 기한을 넘기면 강제 종료로 끝낸다.

## 6. 관련 문서

- 옮겨 다니는 것 — [Spot](21-spot.ko.md) · [Actor](22-actor.ko.md)
- 이동 단위를 정하는 것 — [실행 모델](32-execution-model.ko.md)
- 들어오고 나가는 사건 — [Actor membership](35-actor-membership.ko.md)
- timer가 함께 옮겨 가는 이유 — [Timer와 worker](36-timer-worker.ko.md)
- 운영에서 호출하는 API — [운영과 lifecycle](12-operations.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
