---
title: "Actor · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/22-actor.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Actor

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Spot](21-spot.ko.md) | [다음: STREAM](23-stream.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/22-actor.ko.md) · [C#/.NET](../../../dotnet/guide/server/22-actor.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/22-actor.ko.md) · [Node/TypeScript](../../../node/guide/server/22-actor.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    개체 하나를 id로 만들고, 그것에 메시지를 보내고 답을 받을 수 있다.
    이 장의 코드는 `framework/languages/java/tutorial/java`에서 그대로 실행된다.

[Spot](21-spot.ko.md)이 방·queue처럼 **여럿이 함께 사용하는 자리**를 다뤘다면, Actor는 플레이어
하나, 세션 하나처럼 **개체 단위 상태**를 맡는다. 둘 다 id로 호출하고 한 번에 하나씩 처리하는
것은 같고, 다른 것은 Actor가 **언제나 어떤 Spot 안에 있다**는 점이다. 이 장은 Actor 하나를
만들어 호출하기까지 다루며, 방 사이 이동과 membership은
[Actor membership](35-actor-membership.ko.md)이 다룬다.

## 1. Actor의 자리 — 언제나 어떤 Spot 안에 있다

Actor는 반드시 어떤 Spot에 속한다. 만들어진 직후에는 **Entry Spot**에 속하고, 방에 들어가면 그
User Spot으로 옮겨 간다. Entry Spot은 Object Server가 시작할 때 Framework가 만드는 Spot이고,
아직 어느 방에도 속하지 않은 Actor의 기본 자리다.

<iframe class="zlink-diagram" src="/common/diagrams/22-actor-membership.html" title="Actor는 Spot 안에서 산다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/22-actor-membership.html" target="_blank">↗ 크게 보기</a></p>

**그래서 handler가 Spot과 Actor를 함께 받는다.** actor id로 보낸 메시지는 그 Actor가 지금
속한 Spot 안에서 실행되고, 그 Spot의 상태와 Actor의 상태를 한 자리에서 다룰 수 있다.

Actor도 Spot과 같은 Location Store를 사용한다. 등록 코드는 [Spot](21-spot.ko.md#2-location-store--spot-등록의-선행-조건)에
있는 것과 같다.

## 2. 받는 쪽 — Actor를 호스팅하는 node

!!! info "받는 쪽 process"

    이 절의 코드는 Actor를 **실행하는** process에 들어간다.

### 2.1 Actor 작성

Actor는 상태를 필드로 들고, 자기 id를 context에서 읽는다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/Player.java:actor-class"
```

**Actor는 생성자로 만들어지지 않는다.** Framework가 factory를 통해 만들므로, 의존성이
필요하면 그 factory에서 받는다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/PlayerFactory.java:actor-factory"
```

### 2.2 Entry Spot 작성

Actor가 처음 들어갈 자리다. Entry Spot은 application이 만들지 않는다. Object Server마다 하나를 등록한다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/spots/LobbySpot.java:entry-spot"
```

!!! warning "C++만 입장 승인 callback이 필수다"

    C++의 entry spot은 `on_actor_join`을 반드시 구현하며, **Actor 생성 처리가 이 callback에서
    시작한다.** 거절하도록 두면 Actor 생성 자체가 실패한다. 나머지 네 언어의 entry spot에는 그 callback이 없고, 생성
    승인은 `onCreateActor`(기본값은 수락)가 맡는다.

### 2.3 handler 작성

handler는 **Spot과 Actor를 함께** 첫 두 인자로 받는다. 값을 돌려주지 않는 handler는 `send`로
온 메시지를 받는다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/ChangeNicknameHandler.java:actor-send-handler"
```

값을 돌려주는 handler는 그 값이 응답이 된다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/actors/GetPlayerHandler.java:actor-request-handler"
```

### 2.4 등록

Entry Spot과 Actor factory를 같은 Object Server에 등록한다. actor type을 등록한 node가 생성
후보가 된다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:actor-register"
```

## 3. 호출하는 쪽 — Actor를 호출하는 node

!!! info "호출하는 쪽 process"

    이 절의 코드는 Actor를 만들고 **호출하는** 다른 process에 들어간다. 앞 절과 짝을 이룬다.

등록은 Spot을 부를 때와 같다 —
[Spot을 호출하는 node](21-spot.ko.md#4-호출하는-쪽--spot을-호출하는-node)의 코드가 그대로 쓰인다.

### 3.1 만들기

Spot과 달리 **id를 호출하는 쪽이 정한다.** 플레이어 id처럼 이미 있는 값을 그대로 쓰기
때문이다. 같은 id로 다시 호출하면 만들지 않고 있던 것을 돌려준다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:actor-create-call"
```

만들었는지 이미 있었는지는 결과가 알려 준다. 처음 만들었으면 `created`, 있던 것을 찾았으면
`existing`이다.

### 3.2 호출하기

actor id만 준다. 그 Actor가 지금 어느 Spot 안에 있는지는 Framework가 찾는다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:actor-send-call"
```

답이 필요하면 요청을 보낸다.

```java
--8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:actor-request-call"
```

!!! note "만들기가 돌려주는 참조"

    만들기 호출은 actor id와 함께 **그때의 세대와 조회 당시 owner 경로**를 담은 참조를 돌려준다.
    이 참조를 사용하는 자리는 [Session과 Actor 연결](24-actor-session.ko.md)과
    [활성화와 수명](34-activation-lifetime.ko.md#32-actor를-없애는-자리)이 다룬다.
    평소의 Actor 호출에는 actor id만 있으면 된다.

## 4. 실행 결과

아래 명령은 tutorial(`framework/languages/java/tutorial/java`)의 Server와 Client를 그 README의 「실행」 절대로 띄운 상태에서 실행한다. 주소는 Client의 HTTP 표면이다.

```bash
curl -X POST http://127.0.0.1:5080/players/p7 \
  -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "created"

curl -X POST http://127.0.0.1:5080/players/p7 \
  -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "existing"

curl -X POST http://127.0.0.1:5080/players/p7/nickname \
  -H 'Content-Type: application/json' -d '{"nickname":"veteran"}'
# 202

curl http://127.0.0.1:5080/players/p7
# {"playerId":"p7","nickname":"veteran"}
```

두 번째 호출은 `existing`을 돌려준다. 같은 id를 다시 호출하면 만들지 않는다. 이름은
세 번째 호출이 바꾸었고, 네 번째 호출이 바뀐 값을 읽었다.

## 5. Spot과의 차이

둘 다 id로 호출하고 한 번에 하나씩 처리한다. 고를 때는 **id를 누가 정하느냐**와 **무엇을
담느냐**를 본다. Spot의 id는 만들 때 Framework가 발급하고 방·queue처럼 여럿이 함께 사용하는 자리를
담는다. Actor의 id는 호출하는 쪽이 정하고 플레이어·세션처럼 개체 하나의 상태를 담는다.

방 사이 이동은 [Actor membership](35-actor-membership.ko.md)이, 종류별 membership callback은
[활성화와 수명](34-activation-lifetime.ko.md)이, 이동 중 상태 보존은
[Relocation](37-relocation.ko.md)이 다룬다.

## 6. 관련 문서

- 여럿이 함께 사용하는 자리 — [Spot](21-spot.ko.md)
- 이름으로 호출하는 경로 — [Channel 메시징](20-channel-messaging.ko.md)
- 이동과 membership — [Actor membership](35-actor-membership.ko.md)
- 이 장 코드의 실행본 — `framework/languages/java/tutorial/java`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
