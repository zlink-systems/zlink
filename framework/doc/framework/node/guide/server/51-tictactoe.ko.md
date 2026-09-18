---
title: "TicTacToe 따라 읽기 · Node/TypeScript"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/51-tictactoe.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# TicTacToe 따라 읽기

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Bingo 따라 읽기](50-bingo.ko.md) | [다음: SupportChat 따라 읽기](52-supportchat.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/51-tictactoe.ko.md) · [C#/.NET](../../../dotnet/guide/server/51-tictactoe.ko.md) · [Java](../../../java/guide/server/51-tictactoe.ko.md) · [Kotlin](../../../kotlin/guide/server/51-tictactoe.ko.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    TicTacToe 샘플을 편집기에 열고, HTTP로 room을 만드는 곳부터 두 player가 방을 나가고 Actor가
    정리되는 곳까지 코드를 따라갈 수 있다. 이 장의 코드는
    `framework/languages/<언어>/samples/TicTacToe`에서 그대로 실행된다.

[샘플 고르기](14-samples.ko.md#2-tictactoe--실시간-대전-게임-서버-구축)가 이 샘플이 무엇을 보여
주는지 소개했다. 이 장은 그 소개 다음에 읽는 자리다 — 역할과 코드 위치, 주요 시나리오의 메시지
흐름, 각 흐름에 등장하는 framework 기능과 그것을 설명하는 장을 소스가 놓인 순서대로 따라간다.
이 장에는 계약을 소유하는 스펙 문서가 없다. 요구사항, 메시지 계약과 검증 기준은
[TicTacToe 시나리오](../../../common/sample/tictactoe/README.ko.md)가 소유하며, 이 장은 그것을
다시 적지 않는다.

## 1. 이 샘플이 보여 주는 것

별도 Session 서버가 없다. `Play`가 STREAM 연결, player Actor, Entry Spot과 room Spot을 함께
소유하고, `Api`는 HTTP room 생성과 인증만 맡는다. 서버 사이의 연결은 Location Store로 자동
해석하지 않고 runner가 준 endpoint로 코드에서 직접 연결한다. handler도 scan에 맡기지 않고 등록
코드에 하나씩 적는다.

<iframe class="zlink-diagram" src="/common/diagrams/14-tictactoe.html" title="TicTacToe 샘플 토폴로지" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-tictactoe.html" target="_blank">↗ 크게 보기</a></p>

수동인 것은 **node 사이의 연결**이다. room과 Actor가 지금 어느 node에 있는지는 여기서도 Location
Store가 해석한다. host는 Play A에, guest와 observer는 Play B에 접속하므로, room을 소유한 node와
Actor가 만들어진 node가 다른 경우가 한 판 안에서 반드시 생긴다 — 그 경우를 framework가 어떻게
처리하는지가 이 샘플의 두 번째 주제다.

## 2. 역할과 코드 위치

| 역할 | process 수 | 소유하는 것 | 코드 위치 |
| --- | ---: | --- | --- |
| Api | 2 | HTTP room 생성, token 검증 | `Server/Api` |
| Play | 2 | STREAM session, Entry Spot, player Actor, room Spot, milestone publish | `Server/Play/Infrastructure` |
| Client | 3 | host·guest·observer 시나리오와 self-check | `Client` |

board, turn, 승·무 판정은 `Server/Play/Domain`에 있고 framework type을 참조하지 않는다. 메시지는
`shared`의 JSON 계약이 정한다.

## 3. 서버 구성 — 수동 연결과 수동 등록

Play는 stream node, API channel client와 route mesh를 등록하고, mesh의 object server에 Entry
Spot, player Actor factory와 room Spot factory를 등록한다. API channel과 mesh의 상대 endpoint는
설정에서 읽어 직접 연결한다.

`Server/Play/tictactoe-play-module.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/tictactoe-play-module.ts:doc-ttt-play-register"
```

player Actor factory는 상태를 보존하는 adapter를 지정하고, room Spot factory는 relocation을 끈다.
Api 쪽도 같은 방식으로 Play의 mesh endpoint에 연결한다.

`Server/Api/tictactoe-api-module.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/tictactoe-api-module.ts:doc-manual-peer-connect"
```

수동 연결에서도 routing id를 함께 적는다 — framework는 endpoint를 Location Store의 descriptor와
대조해 상대가 기대한 node인지 확인한다. 연결과 discovery의 차이는
[Channel 동작 원리](30-channel-patterns.ko.md#6-연결과-discovery)가 다룬다.

handler는 scan에 맡기지 않고 Spot과 session의 handler registry에 packet 이름과 함께 직접
등록한다 — 위 host 구성과 각 Spot의 configure callback에 그 등록이 있다. managed 언어에서 이렇게
등록하는 샘플은 TicTacToe뿐이다. 등록 변형은
[Handler와 메시지 처리](31-handler-dispatch.ko.md#1-handler-등록의-변형)가 다룬다.

## 4. room 생성 — HTTP 요청에서 Spot Create

client는 HTTP로 room을 만든다. Api의 HTTP handler는 Spot manager에 room Spot 생성을 요청하고,
framework가 발급한 `RoomId`와 Play endpoint 목록을 응답한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-tictactoe-create-auth.html" title="Room 생성과 인증·입장" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-tictactoe-create-auth.html" target="_blank">↗ 크게 보기</a></p>

`Server/Api/Handlers/create-game-http-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/Handlers/create-game-http-handler.ts:doc-create"
```

응답의 Play endpoint는 client가 접속할 곳이지 room의 owner가 아니다. Api는 어느 Play에 room을
만들지 정하지 않으며, 만들어진 위치는 Location Store에 기록된다. User Spot 생성은
[Spot](21-spot.ko.md#4-호출하는-쪽--spot을-호출하는-node)이, 생성 시점과 callback은
[활성화와 수명](34-activation-lifetime.ko.md#4-user-spot--application이-만드는-자리)이 다룬다.

## 5. 인증과 bind — Play가 session도 소유한다

client는 응답에서 고른 Play endpoint에 STREAM으로 접속해 인증한다. session handler는 Api에 token을
검증시킨 뒤 player Actor를 만들거나 찾고, 현재 session에 묶는다.

`Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate-play-session-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate-play-session-handler.ts:doc-ttt-session-bind"
```

Bingo와 달리 접속 서버와 Actor를 소유한 서버가 같은 process다. 그래도 Actor를 얻는 호출은 어느
node에 만들지 정하지 않는다. 묶기는 [Session과 Actor 연결](24-actor-session.ko.md)이, 다시 접속한
session이 같은 Actor에 다시 묶이는 규칙은
[Session 묶음의 동작 원리](39-session-binding.ko.md)가 다룬다.

## 6. room join — 예약과 다른 node로의 이동

join 요청은 Entry Spot의 actor send handler에 도착한다. handler는 room join을 예약만 하고 끝난다.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler.ts:doc-join-defer"
```

예약된 join이 실행될 때 Actor가 있는 node와 room을 소유한 node가 다르면, framework는 join
operation 안에서 Actor를 room의 owner로 옮긴다. 옮기는 동안 application 상태는 player Actor
factory에 지정한 adapter가 직렬화한다.

`Server/Play/Infrastructure/ZLink/Actors/play-actor-relocation-adapter.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Actors/play-actor-relocation-adapter.ts:doc-ttt-actor-capture"
```

같은 node에 있으면 adapter는 호출되지 않는다. join이 끝나면 room Spot의 joined callback이
참가자를 등록하고 다른 참가자에게 알린다.

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts:doc-ttt-game-join"
```

예약의 규칙은 [Actor membership](35-actor-membership.ko.md#2-예약-등록--handler가-끝난-뒤에-실행된다)이,
Actor가 옮겨질 때 남는 것과 adapter의 역할은 [Relocation](37-relocation.ko.md)이 다룬다.

## 7. 수 두기와 push

수를 두는 요청은 묶인 Actor로 relay되어 room Spot의 actor request handler에 도착한다. handler는
domain에 수를 반영하고 갱신된 state를 응답한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-tictactoe-place-mark.html" title="수 두기와 최종 state" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-tictactoe-place-mark.html" target="_blank">↗ 크게 보기</a></p>

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-place-mark-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-place-mark-handler.ts:doc-actor-packet-handler"
```

요청한 쪽은 응답으로, 상대는 bound session push로 같은 state를 받는다.

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts:doc-ttt-broadcast"
```

turn 제한 시간은 room Spot의 timer가 확인한다. timer는 initialize callback에서 등록하며, tick은
room의 turn 안에서 실행된다.

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts:doc-ttt-timer-register"
```

잘못된 turn이나 이미 쓴 cell은 application의 거부이므로 typed 오류 응답으로 끝난다. handler
종류는 [Handler와 메시지 처리](31-handler-dispatch.ko.md#4-spot과-actor의-handler-종류)가, timer는
[Timer와 worker](36-timer-worker.ko.md#1-timer--주기-실행)가 다룬다.

## 8. milestone의 Logical Multicast

host의 누적 승수가 100이 되면 room Spot이 milestone event를 publish한다. observer는 host와 다른
Play에 접속해 있으므로 room이 observer의 위치를 알 필요가 없도록 channel과 topic으로 발행한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-tictactoe-milestone.html" title="Wins 100 milestone" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-tictactoe-milestone.html" target="_blank">↗ 크게 보기</a></p>

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts:doc-multicast-publish"
```

각 Play의 Entry Spot이 같은 topic을 구독한다. 구독 handler도 packet handler와 같은 registry에
등록한다.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts:doc-multicast-subscribe"
```

event를 받은 Entry Spot은 관찰을 신청한 Actor마다 notify를 보내고, 각 Actor의 bound session이
그것을 client에 전달한다. observer는
room의 member가 아니며 별도 Spot type도 없다.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/entry-spot-registries.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/entry-spot-registries.ts:doc-ttt-milestone-notify"
```

publish의 정상 완료는 구독자의 처리 완료가 아니다. client는 observer에게 도착한 notify로
전달을 확인한다. pub/sub의 형태는 [Channel 동작 원리](30-channel-patterns.ko.md#5-pubsub의-형태)가
다룬다.

## 9. disconnect, 재접속과 destroy

연결이 끊기면 framework가 묶인 Actor에 알린다. Actor는 지워지지 않고 room membership도 바뀌지
않는다. 다시 접속해 인증하면 같은 id의 Actor에 새 session이 묶이고, client는 같은 `RoomId`로
join을 다시 보내 현재 state를 받는다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-tictactoe-disconnect.html" title="Disconnect와 destroy" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-tictactoe-disconnect.html" target="_blank">↗ 크게 보기</a></p>

게임이 끝난 뒤 각 player가 leave를 보내면 room Spot은 그 Actor에 destroy 표시를 남기고 room에서
내보낸다.

`Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-leave-game-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-leave-game-handler.ts:doc-ttt-leave-game"
```

Actor는 Entry Spot으로 돌아가고, Entry Spot의 joined callback이 표시를 확인해 destroy를 호출한다.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts:doc-ttt-entry-destroy"
```

끊김 통지는 [Session 묶음의 동작 원리](39-session-binding.ko.md#3-연결이-끊길-때의-통지)가,
Entry Spot으로 돌아가는 순서와 destroy는
[활성화와 수명](34-activation-lifetime.ko.md#3-entry-spot--framework가-만드는-자리)이 다룬다.

## 10. 실행과 검증

runner 하나가 Redis 컨테이너, 서버 process와 client 시나리오를 함께 띄운다.

```bash
framework/languages/node/samples/TicTacToe.Ts/run_sample.sh
```

client는 room 생성 응답, 인증, join notify, 수 두기 결과, milestone notify와 재접속 뒤의 state를
assertion으로 확인한다. runner는 서버 로그에서 각 Actor의 room leave와 Entry Spot destroy를
확인한다. 확인 항목과 로그 문자열은
[TicTacToe 시나리오](../../../common/sample/tictactoe/README.ko.md#9-client-self-check)가 정한다.

## 11. 관련 문서

- 다른 샘플과의 비교와 선택 기준: [샘플 고르기](14-samples.ko.md)
- 요구사항, 메시지 계약과 완료 기준: [TicTacToe 시나리오](../../../common/sample/tictactoe/README.ko.md)
- 같은 게임을 자동 연결·자동 등록과 별도 Session 서버로 만든 구성: [Bingo 따라 읽기](50-bingo.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
