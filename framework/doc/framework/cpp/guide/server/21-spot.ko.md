---
title: "Spot · C++"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/21-spot.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Spot

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Channel 메시징](20-channel-messaging.ko.md) | [다음: Actor](22-actor.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — **C++** · [C#/.NET](../../../dotnet/guide/server/21-spot.ko.md) · [Java](../../../java/guide/server/21-spot.ko.md) · [Kotlin](../../../kotlin/guide/server/21-spot.ko.md) · [Node/TypeScript](../../../node/guide/server/21-spot.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    id로 호출하는 상태 객체를 만들고, 그것에 메시지를 보내고 답을 받을 수 있다.
    이 장의 코드는 `framework/languages/cpp/tutorial`에서 그대로 실행된다.

[Channel 메시징](20-channel-messaging.ko.md)의 호출은 이름을 맡은 node 중 하나가 받았다.
받을 대상이 정해져 있으면 그 경로를 사용할 수 없다. **Spot은 id로 찾는 상태 객체**이고, 자기
앞으로 온 일을 한 줄로 세워 처리한다. 이 장은 Spot 하나를 만들어 호출하기까지 다루며, 종류별
차이와 lifecycle 전체는 [Spot](21-spot.ko.md)이 다룬다.

## 1. Spot이 푸는 문제

채팅방 하나, 매칭 queue 하나처럼 **무언가를 기억하면서 자기 앞으로 온 일을 순서대로 처리해야
하는 단위**가 있다. channel로는 그것을 표현할 수 없다 — channel 호출은 그 이름을 맡은 node
중 하나로 가므로, 같은 방에 보낸 두 메시지가 서로 다른 node에 도착할 수 있다.

Spot은 id를 가진다. 같은 id로 보낸 메시지는 언제나 같은 Spot에 도착하고, 그 Spot 안에서
**한 번에 하나씩** 실행된다. 그래서 Spot의 필드는 lock 없이 다뤄도 된다.

<iframe class="zlink-diagram" src="/common/diagrams/21-spot-placement.html" title="Spot은 등록한 node 중 한 곳에 만들어진다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/21-spot-placement.html" target="_blank">↗ 크게 보기</a></p>

만드는 쪽도 호출하는 쪽도 node를 고르지 않는다. 그 점이 channel과 같고, **대상이 하나로
정해진다**는 점이 channel과 다르다.

## 2. Location Store — Spot 등록의 선행 조건

Spot은 host가 아니라 id로 호출된다. 그래서 **지금 어느 node에 있는지를 적어 둘 곳**이
필요하다. 그것이 Location Store이고, Spot을 하나라도 등록하면 반드시 있어야 한다.

```cpp
--8<-- "framework/languages/cpp/tutorial/Server/main.cpp:location-store"
```

**Relocation Store도 함께 등록한다.** Spot factory를 등록하는 것 자체가 조건이라, 이 장처럼
이동을 끄더라도 요구된다.

```cpp
--8<-- "framework/languages/cpp/tutorial/Server/main.cpp:relocation-store"
```

호출만 하는 node에는 Location Store만 있으면 된다 —
[호출하는 쪽 — Spot을 호출하는 node](#4-호출하는-쪽--spot을-호출하는-node).

## 3. 받는 쪽 — Spot을 호스팅하는 node

!!! note "받는 쪽 process"

    이 절의 코드는 Spot을 **실행하는** process에 들어간다.

### 3.1 Spot 작성

Spot은 상태를 필드로 들고, 만들어질 때 받은 payload로 자신을 초기화한다.

```cpp
--8<-- "framework/languages/cpp/tutorial/Server/spots/game_room.hpp:spot-class"
```

만들 때 거절하려면 생성 callback에서 거절한다. 그러면 생성 호출이 실패하고 Spot은 남지
않는다. 그 callback을 두지 않으면 모든 생성 요청을 받아들인다.

### 3.2 handler 작성

handler는 대상 Spot을 첫 인자로 받는다. channel handler와 달리 **그 Spot의 상태를 직접
다룬다.**

```cpp
--8<-- "framework/languages/cpp/tutorial/Server/spots/game_room.hpp:spot-handlers"
```

반환값이 없는 handler는 `send`로 온 메시지를 받고, 값을 돌려주는 handler는 그 값이 응답이
된다. 두 handler는 같은 Spot에서 **한 번에 하나씩** 실행된다.

### 3.3 등록

mesh node가 Object Server 역할을 한 번 고르고, 그 위에 Spot factory를 등록한다.

```cpp
--8<-- "framework/languages/cpp/tutorial/Server/main.cpp:object-server"

--8<-- "framework/languages/cpp/tutorial/Server/main.cpp:spot-register"
```

등록할 때 주는 이름이 **stable type**이다. 호출하는 쪽은 Spot을 만들 때 그 이름을 지정한다.
같은 이름을 등록한 node가 후보가 된다. Spot factory를 등록할 때 이동 정책을 하나 지정하며,
이 장의 코드는 이동을 끈 정책을 지정한다.

## 4. 호출하는 쪽 — Spot을 호출하는 node

!!! info "호출하는 쪽 process"

    이 절의 코드는 방을 만들고 **호출하는** 다른 process에 들어간다. 앞 절과 짝을 이룬다.

호출하는 node는 Spot을 실행하지 않으므로 factory를 등록하지 않는다. 대신 그 mesh에서
client 역할을 고르고, 위치를 읽을 Location Store를 같은 prefix로 등록한다.

```cpp
--8<-- "framework/languages/cpp/tutorial/Client/main.cpp:location-store-client"

--8<-- "framework/languages/cpp/tutorial/Client/main.cpp:spot-client-register"
```

### 4.1 만들기

stable type을 지정해 만든다. 응답으로 오는 id가 그 뒤 모든 호출의 주소다.

```cpp
--8<-- "framework/languages/cpp/tutorial/Client/main.cpp:spot-create-call"
```

### 4.2 호출하기

만들 때 받은 id만 준다. 지금 어느 node에 있는지는 Framework가 찾는다.

```cpp
--8<-- "framework/languages/cpp/tutorial/Client/main.cpp:spot-send-call"
```

답이 필요하면 요청을 보낸다. 대상이 옮겨 가는 중일 수 있으므로 timeout을 함께 준다.

```cpp
--8<-- "framework/languages/cpp/tutorial/Client/main.cpp:spot-request-call"
```

## 5. 실행 결과

아래 명령은 tutorial(`framework/languages/cpp/tutorial`)의 Server와 Client를 그 README의 「실행」 절대로 띄운 상태에서 실행한다. 주소는 Client의 HTTP 표면이다.

```bash
curl -X POST http://127.0.0.1:5080/rooms \
  -H 'Content-Type: application/json' -d '{"title":"lobby"}'
# "8213420c-03ff-47a6-8410-ddfb3df26660"

curl -X POST http://127.0.0.1:5080/rooms/8213420c-03ff-47a6-8410-ddfb3df26660/chat \
  -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'
# 202

curl http://127.0.0.1:5080/rooms/8213420c-03ff-47a6-8410-ddfb3df26660
# {"title":"lobby","chat":["p1: hello"]}
```

id는 Framework가 만든다. 두 번째 호출은 응답을 기다리지 않고, 세 번째는 방이 만든 답을
받는다. 방은 두 호출 사이에 두 호출 사이에도 상태를 유지한다.

## 6. Spot의 다른 종류

이 장이 만든 것은 **application이 명시적으로 만드는 Spot**이고, 대부분의 방·스테이지·존이
여기 해당한다. 그 밖에 Object Server가 시작할 때 Framework가 만드는 것과, 첫 메시지가
도착할 때 만들어지는 것이 있다. 생성 시점과 받는 lifecycle callback이 서로 다르다.

종류별 차이와 lifecycle 전체는 [Spot](21-spot.ko.md)이 다룬다.

## 7. 관련 문서

- id로 호출하는 또 다른 단위 — [Actor](22-actor.ko.md)
- 이름으로 호출하는 경로 — [Channel 메시징](20-channel-messaging.ko.md)
- Location Store가 무엇을 적어 두는가 — [Channel 동작 원리](30-channel-patterns.ko.md#6-연결과-discovery)
- 이 장 코드의 실행본 — `framework/languages/cpp/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
