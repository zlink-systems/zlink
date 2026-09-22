---
title: "Location · C#/.NET"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/25-location.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Location

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Session과 Actor 연결](24-actor-session.ko.md) | [다음: 모니터링](26-monitoring.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/25-location.ko.md) · **C#/.NET** · [Java](../../../java/guide/server/25-location.ko.md) · [Kotlin](../../../kotlin/guide/server/25-location.ko.md) · [Node/TypeScript](../../../node/guide/server/25-location.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

이 장은 [tutorial의 `Client` 디렉터리와 「실행」 절](https://github.com/zlink-systems/zlink-dotnet-examples/blob/main/tutorial/README.ko.md#실행)에서 코드를 인용하며, 그 tree를 bootstrap하고 build하면 아래 실행 결과를 재현할 수 있다.

!!! info "이 장을 읽고 나면"

    id만 가지고 그 Spot·Actor가 지금 어느 node에 있는지 물어볼 수 있다.
    이 장의 코드는 `framework/languages/dotnet/tutorial`에서 그대로 실행된다.

[Spot](21-spot.ko.md)과 [Actor](22-actor.ko.md)는 id로만 호출했다. 그것이 지금 어느 node에
있는지는 Framework가 찾는다. 그 기록을 보관하는 곳이 **Location Store**다. 이 장은
application이 그 기록을 직접 읽는 방법을 다루며, Store가 무엇을 적어 두고 process들이 그것으로
어떻게 서로를 찾는지는
[Channel 동작 원리](30-channel-patterns.ko.md#61-location-store--누가-어디-있는지-적어-두는-곳)가
다룬다.

## 1. 조회와 호출의 차이 — 조회는 대상의 상태를 바꾸지 않는다

호출은 대상에게 동작을 실행하게 한다. 조회는 **대상의 상태를 바꾸지 않고 위치만 읽는다.** 운영 화면에
"이 방이 지금 어느 서버에 있는가"를 표시할 때, 또는 대상을 옮긴 뒤 기록이 실제로 바뀌었는지
확인할 때 사용한다.

<iframe class="zlink-diagram" src="/common/diagrams/25-location-find.html" title="find는 Store에 묻고 끝난다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/25-location-find.html" target="_blank">↗ 크게 보기</a></p>

돌아오는 값은 그 시점의 기록이다. 대상이 실제로 응답하는지까지 확인하지 않으므로, **살아
있는지를 알아야 하면 조회가 아니라 요청을 보낸다.**

## 2. 조회하는 node의 Store 등록 요건

조회는 Store를 읽는 일이므로, 조회하는 process가 그 Store를 **같은 prefix로** 등록하고 있어야
한다. 등록 코드는 [Spot](21-spot.ko.md#2-location-store--spot-등록의-선행-조건)에 있는 것과 같다.

Spot도 Actor도 실행하지 않고 호출만 하는 node라면 Location Store 하나로 충분하다. Relocation
Store는 factory를 등록한 node에만 요구된다.

## 3. id로 위치 조회

Spot은 spot manager에, Actor는 actor manager에 조회한다. 둘 다 id 하나만 받으며, 어느 node에
물을지는 정하지 않는다.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:location-find"
```

돌려주는 것은 id와 node 이름을 담은 참조 하나다. 찾지 못하면 빈 값을 돌려준다. 조회는 **지금
메시지를 받을 수 있는 대상만** 답하므로, 아직 만들어지는 중이거나 옮겨 가는 중인 대상도
빈 값으로 온다. 그래서 빈 값 하나를 "그런 id는 없다"로 단정하지 않는다.

## 4. 실행 결과

tutorial README의 「실행」 절을 따라 띄운 상태에서 Server와 Client를 실행한 상태에서 Client의 HTTP 표면에 아래 `curl` 요청을 보내면, 각 HTTP 응답은 `curl` stdout에 나오고 생성·조회 handler 기록은 Server process의 stdout 또는 `server.log`에 나온다.

```bash
curl -X POST http://127.0.0.1:5080/rooms \
  -H 'Content-Type: application/json' -d '{"title":"lobby"}'
# "5ce8339b-ec20-42d8-a117-a74677ad9af0"

curl http://127.0.0.1:5080/locations/rooms/5ce8339b-ec20-42d8-a117-a74677ad9af0
# {"spotId":"5ce8339b-ec20-42d8-a117-a74677ad9af0","node":"game-server-1"}

curl -X POST http://127.0.0.1:5080/players/p7 \
  -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "created"

curl http://127.0.0.1:5080/locations/players/p7
# {"actorId":"p7","node":"game-server-1"}

curl http://127.0.0.1:5080/locations/players/ghost
# 404
```

방과 플레이어 둘 다 `game-server-1`에 있다고 답했다. 마지막 호출은 만든 적 없는 id를 조회했다. 빈 값은 404가 된다.

## 5. 조회 결과의 사용 범위 — node 이름은 호출 주소가 아니다

조회가 돌려준 node 이름은 **그 순간의 위치**이지 호출 주소가 아니다. 호출은 [Spot](21-spot.ko.md)과
[Actor](22-actor.ko.md)에서 한 그대로 id만 준다. Framework가 호출할 때마다 같은 기록을 다시 읽기
때문에, 조회한 뒤 대상이 다른 node로 옮겨 가도 호출은 옮겨 간 node로 전달된다. node 이름을 받아
[node를 직접 호출하기](20-channel-messaging.ko.md#36-node를-직접-호출하기)로 보내면 그 따라가기를
잃는다.

참조를 그대로 넘기는 자리는 따로 있다. 닫거나 지우는 호출은 **그 참조가 가리키는 세대**만
대상으로 삼으며, 같은 id로 다시 만들어진 대상은 그대로 둔다.

## 6. 관련 문서

- id로 호출하는 상태 객체 — [Spot](21-spot.ko.md) · [Actor](22-actor.ko.md)
- Store가 무엇을 적어 두는가 — [Channel 동작 원리](30-channel-patterns.ko.md#61-location-store--누가-어디-있는지-적어-두는-곳)
- 운영 조회 — [운영과 lifecycle](12-operations.ko.md#5-location-readiness와-운영-조회)
- 이 장 코드의 실행본 — `framework/languages/dotnet/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
