---
title: "Session 묶음의 동작 원리 · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/39-session-binding.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Session 묶음의 동작 원리

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: STREAM의 동작 원리](38-stream-boundary.ko.md) | [다음: 12. 운영 — 런타임 메트릭 · graceful drain · readiness](12-operations.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/39-session-binding.ko.md) · [C#/.NET](../../../dotnet/guide/server/39-session-binding.ko.md) · [Java](../../../java/guide/server/39-session-binding.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/39-session-binding.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    한 연결에 몇 개를 묶을 수 있는지, 묶은 대상이 옮겨 가면 무엇이 갱신되는지, 연결이
    끊길 때 누구에게 알리는지 알 수 있다.

[Session과 Actor 연결](24-actor-session.ko.md)은 연결 하나를 Actor 하나에 묶고 알림을 받기까지
다뤘다. 이 장은 그 묶음이 지키는 규칙을 다룬다.

## 1. 묶을 수 있는 개수 — session 하나에 Actor 여럿, Actor 하나에 session 하나

<iframe class="zlink-diagram" src="/common/diagrams/39-binding-shape.html" title="session 하나는 Actor 여럿을 묶고, Actor 하나는 session 하나에만 묶인다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/39-binding-shape.html" target="_blank">↗ 크게 보기</a></p>

**session 하나는 여러 Actor를 동시에 묶을 수 있다.** 한 연결이 플레이어 Actor와 파티 Actor를
함께 사용해도 된다.

**반대로 Actor 하나는 동시에 session 하나에만 묶인다.** 새 묶음이 확정되면 이전 묶음은 무효가
되고, 그쪽으로 온 늦은 message는 거부된다.

여러 Actor를 묶은 session에서 넘길 대상을 고르는 것은 application의 몫이다. application이 정한
규약으로 actor id를 골라 찾기 호출에 넘긴다 — **Framework는 임의의 Actor를 고르지 않는다.**

묶는 호출은 중복 묶기를 오류로 처리한다. 인증 재전송처럼 이미 묶여 있을 수 있는 흐름에서는
찾거나 묶는 호출을 사용한다.

## 2. 묶음과 Spot membership — 서로 독립이다

묶음은 Actor의 Spot membership과 별개다. Actor가 다른 Spot이나 node로 옮겨 가도 actor id와
[generation](22-actor.ko.md#33-참조의-generation)은 유지되고, Framework가 묶음의 경로를 갱신한다 —
[Actor membership](35-actor-membership.ko.md)과 [Relocation](37-relocation.ko.md)이 그 이동을
다룬다.

**relay는 기록을 다시 조회하지 않는다.** 묶을 때 확인한 경로를 session이 Actor마다 보관하고
그것으로 보낸다. Actor가 옮겨 가면 이동이 확정된 뒤 Framework가 그 보관한 경로를 갱신한다 —
application이 다시 묶지 않는다.

묶은 뒤 Actor가 보낼 수 있는 것은 먼저 보내기와 끊기뿐이다. 요청에 대한 답은 요청 handler의
반환값으로 처리한다.

## 3. 연결이 끊길 때의 통지

물리적으로 연결이 끊기면 Framework가 그 시점의 묶음 전체에 자동으로 알린다. 연결이 유지된 채
논리적으로 끊겼다고 알릴 때만 application이 직접 호출한다.

끊김은 Actor를 지우지도 Entry Spot으로 옮기지도 않는다. 다시 접속한 session은 같은 참조를 다시
조회해 [다시 묶을 수 있다](24-actor-session.ko.md#31-묶기). TicTacToe의 게임 Spot은 끊긴 Actor를
표시할 뿐 room과 경기 상태에서는 빼지 않는다.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/tictactoegamespot/TicTacToeGame.kt:doc-disconnect-actor"
```

이 호출은 물리 연결이 유지된 채 application 규약상 끊김을 알릴 때만 사용한다.

**실행 결과.** 2026-09-22에 .NET TicTacToe의 `run_sample.sh`를 실행하고 host client가 연결을
닫았을 때 `play-a.log`에는 묶인 Actor 하나가 있던 연결의 실제 기록이 남았다.

```text
20:12:39.106 info: TicTacToe.Server.Play.Infrastructure.ZLink.Sessions.PlaySession[0] client -> play stream: disconnected. sessionId=00000002, actors=1
```

**한 Actor의 통지가 실패해도 나머지는 계속한다.** Framework는 연결이 끊긴 시점의 묶음 목록을
고정하고 각 Actor에 알리는데, 그중 하나가 실패하거나 callback이 기한을 넘겨도 남은 Actor 통지와
session 정리를 멈추지 않는다.

**자동 통지와 직접 호출이 겹쳐도 callback은 한 번만 실행된다.** 같은 묶음에 대한 두 통지를
Framework가 합치므로, 직접 호출한 직후에 연결이 끊겨도 Spot의 끊김 callback이 두 번 돌지 않는다.
연결은 유지하지만 application 규약상 끊겼다고 처리할 때만 Actor에 직접 알린다.

```kotlin
--8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.kt:session-disconnect-notify"
```

## 4. 묶기가 실패하거나 무효가 되는 경우

| 상황 | 결과 |
| --- | --- |
| Actor가 없거나 받을 수 있는 상태가 아니다 | 묶기가 typed 오류로 끝난다 |
| 참조의 [generation](22-actor.ko.md#33-참조의-generation)이 다르다 | 낡은 참조를 다른 generation에 묶지 않는다 |
| Actor가 옮겨 가는 중이다 | 옮기는 중이라는 오류로 끝나며 몰래 재시도하지 않는다 |
| 묶은 뒤 Actor가 옮겨 갔다 | Framework가 경로를 갱신하며 session을 다시 묶지 않는다 |
| session이 끊겼다 | Actor와 Spot membership은 유지한다 |
| session이 닫힌 뒤 도착한 응답 | 버린다. 새 session이나 새 묶음의 응답으로 사용하지 않는다 |
| relay 뒤 timeout이나 경로 실패 | 다른 Actor·새 owner·다른 node로 자동 재전송하지 않는다 |

참조가 담은 mesh 이름과 node 식별자는 **처음 조회했을 때의 값**이다. application은 낡은 경로를
새로 조합하지 말고 manager의 조회 호출로 지금 참조를 다시 얻는다 —
[Location](25-location.ko.md#5-조회-결과의-사용-범위--node-이름은-호출-주소가-아니다)이 그 경계를
다룬다.

## 5. 관련 문서

- 묶기까지의 절차 — [Session과 Actor 연결](24-actor-session.ko.md)
- 연결을 받는 자리 — [STREAM](23-stream.ko.md) · [STREAM의 동작 원리](38-stream-boundary.ko.md)
- 묶이는 대상 — [Actor](22-actor.ko.md)
- 옮겨 가는 동안의 처리 — [Relocation](37-relocation.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
