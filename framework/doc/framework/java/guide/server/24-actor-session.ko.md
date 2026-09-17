---
title: "Session과 Actor 연결 · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/24-actor-session.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Session과 Actor 연결

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: STREAM](23-stream.ko.md) | [다음: Location](25-location.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C#/.NET](../../../dotnet/guide/server/24-actor-session.ko.md) · [C++](../../../cpp/guide/server/24-actor-session.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/24-actor-session.ko.md) · [Node/TypeScript](../../../node/guide/server/24-actor-session.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    외부 client의 연결 하나를 Actor 하나에 묶고, 그 Actor가 같은 연결로 알림을 보낼 수 있다.
    이 장의 코드는 `framework/languages/<언어>/tutorial`에서 그대로 실행된다.

[STREAM](23-stream.ko.md)의 session은 연결이 끊기면 함께 끝난다. 플레이어의 상태는 그보다
오래 남아야 하고, 그 상태를 맡는 것이 [Actor](22-actor.ko.md)다. **이 장은 그 둘을 묶는다.** 묶은 뒤에는 session이 다루지 않은 packet이 그 Actor로 간다.
Actor는 같은 연결로 보낼 수 있다.

## 1. 묶는 것이 푸는 문제

연결과 Actor는 수명이 다르다. 같은 플레이어가 끊었다 다시 접속하면 연결은 새로 만들어진다.
Actor는 이전 것이 그대로 남아 있다. 그래서 **연결이 자기 Actor를 가리키게 해 두고**, 그 뒤의
packet은 그 Actor가 받는다.

<iframe class="zlink-diagram" src="/common/diagrams/24-actor-session-binding.html" title="연결 하나를 개체 하나에 묶는다" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/24-actor-session-binding.html" target="_blank">↗ 크게 보기</a></p>

묶은 뒤에도 session은 사라지지 않는다. session이 아는 packet은 session이 먼저 처리하고,
**모르는 것만** Actor로 간다.

## 2. 주고받을 packet을 정한다

인증 요청과 그 답, 그리고 Actor가 보내는 알림이다. 마지막 것은 답할 요청이 없다.

```java
--8<-- "framework/languages/java/tutorial/java/Shared/src/main/java/systems/zlink/tutorial/shared/Contracts.java:session-actor-contracts"
```

## 3. 받는 쪽 — 연결을 Actor에 묶는 node

!!! note "받는 쪽 process"

    이 절의 코드는 [STREAM](23-stream.ko.md)에서 stream node를 등록한 process에 들어간다.
    **등록할 때 actor dispatch를 켜 두어야** 이 장의 relay가 동작한다.

### 3.1 묶기

client가 자기 id를 보내면, 그 id의 Actor를 찾거나 만들어 이 연결에 묶는다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/sessions/AuthenticateHandler.java:session-actor-bind"
```

**같은 id로 다시 접속하면 있던 Actor에 묶인다.** 그래서 만들기와 찾기를 한 호출로 처리한다.

### 3.2 남은 packet을 넘기기

session이 처리하지 못한 packet을 묶인 Actor로 보낸다. 묶기 전에는 보낼 곳이 없으므로 **인증이
먼저**라는 것이 여기서 강제된다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/sessions/GameSession.java:session-actor-relay"
```

넘어간 packet은 그 Actor의 handler가 받는다 — [Actor](22-actor.ko.md)에서 사용한 것과 같은
handler다. 같은 handler가 mesh 호출로도 STREAM relay로도 호출된다.

### 3.3 Actor가 그 연결로 보내기

Actor는 자기에게 묶인 연결을 알고 있다. 응답이 아니라 **스스로 보내는 알림**이다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/ChangeNicknameHandler.java:actor-push"
```

!!! warning "묶인 연결이 없을 때의 결과가 언어마다 다르다"

    같은 handler는 mesh 호출로도 호출된다. 그 호출에는 묶인 연결이 없다. .NET·C++·Node에서 그
    호출은 **아무 일도 하지 않고 끝난다.** Java와 Kotlin에서는 **예외를 던진다.** 이 예외는 동기
    예외이므로 반환된 결과를 확인하는 방식으로는 감지되지 않는다. 두 언어의 코드가 그 실패를
    직접 처리하는 이유다.

## 4. 접속하는 쪽 — mesh 밖의 client

!!! info "접속하는 쪽 process"

    이 절의 코드는 [STREAM](23-stream.ko.md)에서 연결을 연 process에 들어간다. 앞 절과 짝을
    이룬다.

인증하고, 알림을 받는다. 두 번째 호출은 답을 기다리지 않는 단방향인데도 뒤에 값이 온다 —
그것이 Actor가 보낸 알림이다.

```java
--8<-- "framework/languages/java/tutorial/java/StreamClient/src/main/java/systems/zlink/tutorial/streamclient/StreamClientProgram.java:session-actor-client"
```

## 5. 실행 결과

```bash
dotnet run --project StreamClient/StreamClient.csproj
# connected: True
# round trip: 56ms
# bound player: p1
# pushed: speedy
```

`pushed` 줄은 client가 요구하지 않은 응답이다. client는 이름을 바꾸라고 보냈을 뿐 답을
요구하지 않았다. 돌아온 것은 **player가 스스로 보낸 알림**이다. 그 사이에 packet은 session을
지나 Actor로 갔고, Actor가 같은 연결로 되돌려 보냈다.

## 6. 연결이 끊긴 뒤 — session은 끝나고 Actor는 남는다

session은 끝나고 Actor는 남는다. 같은 id로 다시 접속하면 그 Actor에 다시 묶인다.

끊김을 Actor에게 알리는 callback, 묶음이 다른 연결로 옮겨 갈 때의 규칙, 그리고 Actor가 다른
node로 이동하는 중의 처리는 [Session 묶음의 동작 원리](39-session-binding.ko.md)가 다룬다.

## 7. 관련 문서

- 연결을 받는 자리 — [STREAM](23-stream.ko.md)
- 묶이는 대상 — [Actor](22-actor.ko.md)
- 묶음의 규칙 — [Session 묶음의 동작 원리](39-session-binding.ko.md)
- 이 장 코드의 실행본 — `framework/languages/<언어>/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
