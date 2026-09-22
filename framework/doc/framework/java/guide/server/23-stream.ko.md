---
title: "STREAM · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/23-stream.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# STREAM

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Actor](22-actor.ko.md) | [다음: Session과 Actor 연결](24-actor-session.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/23-stream.ko.md) · [C#/.NET](../../../dotnet/guide/server/23-stream.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/23-stream.ko.md) · [Node/TypeScript](../../../node/guide/server/23-stream.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

이 장은 [tutorial의 `Server`·`StreamClient` 디렉터리와 「실행」 절](https://github.com/zlink-systems/zlink-java-examples/blob/main/tutorial/README.ko.md#실행)에서 코드를 인용하며, 그 tree를 bootstrap하고 build하면 아래 실행 결과를 재현할 수 있다.

STREAM node에 연결하는 client는 언어별 stream connector를 사용한다. Unity·Unreal·browser·Node·.NET·Java·C++ client의 설치 범위는 [Client stream connector](../../../install.ko.md#client-stream-connector)에 있고, 사용하는 언어의 [stream connector 가이드](../stream-connector/README.ko.md)가 연결과 packet 처리 방법을 다룬다.

!!! info "이 장을 읽고 나면"

    mesh 밖의 프로그램이 연결 하나로 접속해 요청을 보내고 답을 받게 할 수 있다.
    이 장의 코드는 `framework/languages/java/tutorial/java`에서 그대로 실행된다.

지금까지의 호출은 모두 mesh 안의 node끼리였다. 게임 client나 application은 mesh 밖에 있고, Framework를
참조하지도 않는다. **STREAM은 그런 프로그램이 접속하는 자리**이고, 연결 하나가 열려 있는 동안
양쪽이 서로에게 보낸다. 이 장은 연결을 받아 packet 하나에 답하기까지 다루며, 그 연결을 Actor에
묶는 것은 [Session과 Actor 연결](24-actor-session.ko.md)이 다룬다.

## 1. STREAM이 푸는 문제

channel도 Spot도 Actor도 **mesh 안에서 호출하는 경로**다. 호출하는 쪽이 mesh의 구성원이어야 하고,
Framework를 참조해야 한다. 플레이어의 기기에서 실행되는 client는 그 조건을 충족하지 못한다.

STREAM은 그 경계를 하나의 연결로 좁힌다. client는 주소 하나와 packet 이름만 알면 되고, mesh가
몇 개의 node로 되어 있는지는 모른다.

<iframe class="zlink-diagram" src="/common/diagrams/23-stream-boundary.html" title="STREAM은 mesh의 바깥 경계다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/23-stream-boundary.html" target="_blank">↗ 크게 보기</a></p>

**연결 하나에 session 객체 하나가 대응한다.** 그 연결로 들어온 packet은 그 session에서 한 번에
하나씩 처리되므로, session의 필드는 그 연결의 상태를 담는 자리로 사용할 수 있다.

## 2. 주고받을 packet을 정한다

mesh 호출의 계약과 다르지 않다. 다만 이 packet은 mesh 밖의 client와 주고받으므로, client가
사용하는 이름과 같아야 한다.

```java
--8<-- "framework/languages/java/tutorial/java/Shared/src/main/java/systems/zlink/tutorial/shared/Contracts.java:stream-contracts"
```

## 3. 받는 쪽 — 연결을 받는 node

!!! note "받는 쪽 process"

    이 절의 코드는 연결을 **받는** process에 들어간다.

### 3.1 session 작성

session은 연결 하나를 대표한다. 연결·해제·오류와 packet 도착이 모두 callback으로 오고, 같은
연결의 callback은 순서대로 실행된다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/sessions/GameSession.java:session-class"
```

**packet이 도착하는 callback이 모든 처리의 시작점이다.** handler를 등록해 두더라도 이 callback이 그쪽으로
넘기지 않으면 아무것도 처리되지 않는다.

!!! warning "handler를 등록하는 자리가 언어마다 다르다"

    .NET과 Node는 session 쪽에서 등록하고, Java와 Kotlin은 stream node를 등록할 때 함께
    등록한다. **C++에는 등록 표면이 아예 없다** — 모든 packet이 하나의 callback으로 오고,
    이름으로 처리를 나누는 것은 application의 코드다.

### 3.2 packet handler 작성

값을 돌려주는 대신 **답을 직접 사용한다.** 요청으로 온 packet에는 reply로 답하고, 기다리는 요청이
없는 client에 먼저 보낼 때는 send를 사용한다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/sessions/PingHandler.java:session-handler"
```

### 3.3 등록

stream node는 포트를 열고 session type 하나를 받는다. mesh node와는 별개의 등록이다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:stream-register"
```

## 4. 접속하는 쪽 — mesh 밖의 client

!!! info "접속하는 쪽 process"

    이 절의 코드는 mesh 밖에서 **붙는** process에 들어간다. 앞 절과 짝을 이룬다.

이 process는 Framework를 참조하지 않는다. 별도로 배포되는 **connector** 하나만 참조한다.

```java
--8<-- "framework/languages/java/tutorial/java/StreamClient/src/main/java/systems/zlink/tutorial/streamclient/StreamClientProgram.java:stream-client"
```

!!! warning "Node의 connector는 WebSocket으로 붙는다"

    네 언어의 connector는 TCP를 사용하고 `tcp://`를 적는다. Node의 connector는 브라우저용으로
    배포되어 WebSocket을 쓰므로, **stream node와 client가 둘 다 `ws://`를 적는다.**

## 5. 실행 결과

tutorial README의 「실행」 절을 따라 띄운 상태에서 Server를 실행한 상태에서 아래 명령으로 StreamClient를 시작하면, StreamClient가 Server의 stream endpoint에 보낸 request와 받은 응답은 StreamClient process의 stdout에 나오고 연결·handler 기록은 Server process의 stdout 또는 `server.log`에 나온다.

```bash
dotnet run --project StreamClient/StreamClient.csproj
# connected: True
# round trip: 56ms
```

`connected`가 참이면 연결이 맺어졌다는 뜻이다. 왕복 시간은 client가 보낸 시각을 server가
그대로 돌려주어 잰 값이다. 이 출력으로 mesh 밖과 안이 이어진 것을 확인한다.

## 6. mesh 호출과의 차이 — 연결 자체가 대상이다

mesh 호출은 호출하는 쪽이 mesh의 구성원이다. 대상은 이름이나 id다. STREAM은 **연결 자체가
대상**이다. 그래서 client를 고르는 일이 없다. 반대로 server가 먼저 보낼 수 있다.

연결이 끊기면 그 session은 끝난다. 연결 너머의 상태를 이어 가려면 그 연결을 Actor에 묶는다 —
[Session과 Actor 연결](24-actor-session.ko.md)이 다룬다.

## 7. 관련 문서

- 연결을 개체에 묶기 — [Session과 Actor 연결](24-actor-session.ko.md)
- id로 호출하는 상태 객체 — [Spot](21-spot.ko.md) · [Actor](22-actor.ko.md)
- session lifecycle과 옵션 전체 — [STREAM](23-stream.ko.md)
- 이 장 코드의 실행본 — `framework/languages/java/tutorial/java`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
