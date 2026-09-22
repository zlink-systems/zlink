---
title: "Bingo 따라 읽기 · Node/TypeScript"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/50-bingo.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Bingo 따라 읽기

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 14. 샘플 고르기 — 내 문제에 가까운 예제부터](14-samples.ko.md) | [다음: TicTacToe 따라 읽기](51-tictactoe.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/50-bingo.ko.md) · [C#/.NET](../../../dotnet/guide/server/50-bingo.ko.md) · [Java](../../../java/guide/server/50-bingo.ko.md) · [Kotlin](../../../kotlin/guide/server/50-bingo.ko.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    Bingo 샘플을 편집기에 열고, client의 인증부터 게임 종료까지 메시지가 어느 서버의 어느
    코드를 지나는지 따라갈 수 있다. 이 장의 코드는 `framework/languages/node/samples/Bingo.Ts`에서
    그대로 실행된다.

[샘플 고르기](14-samples.ko.md#4-bingo--온라인-게임-서버-구축)가 이 샘플이 무엇을 보여 주는지
소개했다. 이 장은 그 소개 다음에 읽는 자리다 — 역할과 코드 위치, 주요 시나리오의 메시지 흐름,
각 흐름에 등장하는 framework 기능과 그것을 설명하는 장을 소스가 놓인 순서대로 따라간다.
이 장에는 계약을 소유하는 스펙 문서가 없다. 요구사항, 메시지 계약과 검증 기준은
[Bingo 시나리오](../../../common/sample/bingo/README.ko.md)가 소유하며, 이 장은 그것을 다시
적지 않는다.

## 1. 이 샘플이 보여 주는 것

client는 Session 서버의 STREAM 연결 하나만 유지한다. 인증은 API가, 대기방 예약은 Matchmaking이,
게임 진행은 room을 소유한 Play가 처리하고, 서버 사이의 연결과 object의 위치는 Location Store가
해석한다. 한 판은 인증 → 매칭 → room join → card 제출 → timer 추첨과 push → 승자 확정과 reward
publish → cleanup 순서로 진행되며, 이 장의 절도 같은 순서다.

<iframe class="zlink-diagram" src="/common/diagrams/14-bingo.html" title="Bingo 샘플 토폴로지" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-bingo.html" target="_blank">↗ 크게 보기</a></p>

이 구성이 framework 기능을 가장 많이 사용하는 이유는 흐름마다 다른 기능이 필요하기 때문이다.
연결 하나에 player를 묶는 데는 session binding이, 대기방 예약에는 요청이 올 때 만들어지는
Instance Spot이, room에는 참가자·timer·push를 한 줄로 처리하는 User Spot이, 다른 Play 서버의
관전자에게 알리는 데는 Logical Multicast가 쓰인다.

## 2. 역할과 코드 위치

역할마다 process가 따로 있고, 코드도 `Server/<역할>` 아래에 같은 이름으로 나뉜다. 언어별
package 경로는 다르지만 디렉터리 이름은 같다.

| 역할 | process 수 | 소유하는 것 | 코드 위치 |
| --- | ---: | --- | --- |
| Session | 2 | STREAM 연결, 인증 전 packet 처리, Actor bind와 relay | `Server/Session` |
| API | 2 | 인증, player record, 매칭 조율 | `Server/Api` |
| Matchmaking | 1 | level bucket별 대기방 예약 — Redis가 결정을 보관한다 | `Server/Matchmaking` |
| Play | 2 | player Actor, room Spot, timer, push, reward publish | `Server/Play/Infrastructure` |
| Client | 1 | 인증부터 관찰 종료까지의 시나리오와 self-check | `Client` |

Bingo 규칙 — card 검증, 번호 추첨, mark와 승자 판정 — 은 `Server/Play/Domain`에 있고 framework
type을 참조하지 않는다. 이 장은 그 규칙을 다루지 않는다. 메시지 이름과 field는 `shared`의
Protobuf schema가 정하며, 이 샘플만 payload가 Protobuf다.

## 3. 서버 구성

각 역할의 host 구성이 그 역할이 무엇을 받고 무엇을 호출하는지 보여 준다. Session은 STREAM
연결을 받고, Play mesh의 object를 호출하며, API channel을 호출한다.

`Server/Session/bingo-session-module.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Session/bingo-session-module.ts:doc-bingo-session-register"
```

stream node에 session type을 등록하면 인증된 session에 묶인 Actor로 packet이 전달된다 — managed
언어는 actor dispatch를 함께 켠다. mesh와 channel 등록에 상대 endpoint가 없다 — 어느 node가 어느 이름을 맡는지는
Location Store가 해석한다([Location](25-location.ko.md)).

Play는 Entry Spot, player Actor factory, room Spot factory를 같은 mesh에 등록한다.

`Server/Play/bingo-play-module.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/bingo-play-module.ts:doc-bingo-play-register"
```

Entry Spot은 node마다 하나이고 Actor가 처음 만들어지는 자리다. room은 id마다 하나인 User
Spot이며 `SpotWide`로 실행된다. handler는 managed 언어에서는 scan으로, C++에서는 builder로
등록한다([Handler와 메시지 처리](31-handler-dispatch.ko.md)). 종류별 차이는
[활성화와 수명](34-activation-lifetime.ko.md)이 다룬다.

## 4. 인증과 session·Actor bind

client가 보낸 첫 packet은 인증 요청이다. Session은 API에 token을 검증시키고, 전역 id로 player
Actor를 만들거나 찾고, 그 Actor를 현재 session에 묶는다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-auth-binding.html" title="인증과 binding" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-auth-binding.html" target="_blank">↗ 크게 보기</a></p>

session handler는 API channel에 요청을 보내고, 결과가 유효하면 Play mesh에서 Actor를 얻는다.
Actor의 생성 payload가 이때 함께 전달된다.

`Server/Session/Sessions/Handlers/authenticate-session-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Session/Sessions/Handlers/authenticate-session-handler.ts:doc-bingo-session-auth"
```

Actor를 얻는 호출은 어느 Play node에 만들지 정하지 않는다. 이미 있으면 그 Actor를, 없으면 새로
만든 Actor를 돌려주며, 어느 쪽이든 Session은 반환된 참조를 그대로 묶는다.

`Server/Session/Sessions/Handlers/authenticate-session-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Session/Sessions/Handlers/authenticate-session-handler.ts:doc-bingo-session-bind"
```

묶인 뒤에 도착하는 packet은 session handler가 아니라 Actor의 handler가 받는다. session의
dispatch callback이 그 분기를 만든다 — 등록된 session handler가 처리하지 못한 packet은 묶인
Actor로 relay된다.

`Server/Session/Sessions/bingo-session.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Session/Sessions/bingo-session.ts:doc-bingo-session-relay"
```

Session은 Play endpoint를 저장하지 않는다. Actor가 relocation으로 옮겨져도 framework가 relay
경로를 갱신한다. 묶는 기능은 [Session과 Actor 연결](24-actor-session.ko.md)이, 묶음의 개수와
경로 갱신은 [Session 묶음의 동작 원리](39-session-binding.ko.md)가 다룬다.

## 5. 매칭과 room 준비

매칭 요청은 묶인 Actor로 relay되어 Entry Spot의 actor request handler에 도착한다. handler는
API에 매칭을 요청하고, 받은 `RoomId`로 room join을 예약한 뒤 응답한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-matching-start.html" title="Matching과 game start" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-matching-start.html" target="_blank">↗ 크게 보기</a></p>

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/match-bingo-actor-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/match-bingo-actor-handler.ts:doc-bingo-match-actor"
```

join은 handler 안에서 실행되지 않고 handler가 끝난 뒤에 실행된다. 그래서 매칭 응답의 state는
join 전의 `WaitingForPlayers`이고, 게임 시작은 뒤에 오는 push로 확인한다. 예약의 규칙은
[Actor membership](35-actor-membership.ko.md#2-예약-등록--handler가-끝난-뒤에-실행된다)이 다룬다.

API는 level bucket을 id로 하는 Matchmaker Instance Spot에 예약을 요청하고, 돌려받은 `RoomId`로
room Spot을 만들거나 찾는다.

`Server/Api/Handlers/match-bingo-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Api/Handlers/match-bingo-handler.ts:doc-bingo-api-match"
```

Instance Spot은 첫 요청이 도착할 때 만들어진다. 같은 id로 동시에 온 요청은 하나의 생성을
기다리며, room의 `getOrCreate`도 같은 규칙을 따른다 — API는 어느 Play node에 room을 만들지
정하지 않는다. Matchmaker는 예약을 Redis에서 원자적으로 결정하므로 process memory에 복구
근거를 두지 않고, 활동이 없으면 timer로 자신을 닫는다.

`Server/Matchmaking/bingo-matchmaker.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Matchmaking/bingo-matchmaker.ts:doc-bingo-matchmaker-idle"
```

닫힌 뒤의 다음 요청은 새 generation의 Instance Spot을 만든다. 종류별 생성 시점은
[활성화와 수명](34-activation-lifetime.ko.md#1-spot의-종류)이, Instance Spot 호출은
[Spot](21-spot.ko.md#6-spot의-다른-종류)이 다룬다.

## 6. room join과 Yield

예약된 join이 실행되면 room Spot의 joined callback이 호출된다. player 분기는 API에서 player
record를 읽어야 하는데, 그 왕복 동안 room의 실행권을 반납하지 않으면 다른 player의 join과 timer가
기다린다. 그래서 요청을 `Yield`로 보낸다.

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts:doc-bingo-room-join"
```

이 요청에서 `Yield`가 turn을 반납하고 재개 뒤 pending join을 다시 확인하는 이유는
[실행 모델 §5](32-execution-model.ko.md#5-직렬-실행과-thread-점유)가 다룬다.

## 7. card, 추첨 timer와 push

두 player가 card를 제출하면 room의 timer가 추첨을 시작한다. tick마다 번호 하나를 뽑아 두 card를
mark하고, 갱신된 state를 두 player에게 push한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-card-draw.html" title="Card·draw와 winner 결정" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-card-draw.html" target="_blank">↗ 크게 보기</a></p>

timer handler는 room Spot의 turn 안에서 실행되므로 card 제출 handler와 겹치지 않는다.

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo-room-timer-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo-room-timer-handler.ts:doc-bingo-draw-timer"
```

push는 각 player Actor에 묶인 session으로 보낸다. Actor가 어느 Play node에 있든, session이 어느
Session node에 있든, 보내는 쪽은 Actor의 bound session만 가리킨다.

`Server/Play/Infrastructure/ZLink/Actors/player-actor.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Actors/player-actor.ts:doc-bingo-bound-push"
```

timer 등록과 tick의 실행 위치는 [Timer와 worker](36-timer-worker.ko.md#1-timer--주기-실행)가,
bound session으로 보내는 경로는
[Session 묶음의 동작 원리](39-session-binding.ko.md)가 다룬다.

## 8. reward의 Logical Multicast

승자가 확정되면 room은 종료 push를 보낸 뒤 reward event를 publish한다. 관전자는 다른 Play
node에 있을 수 있으므로 특정 Spot을 가리켜 보내지 않고, topic으로 범위를 정해 발행한다 —
managed 언어는 channel 이름을 함께 적는다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-reward-observe.html" title="Reward 관찰" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-reward-observe.html" target="_blank">↗ 크게 보기</a></p>

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts:doc-bingo-reward-publish"
```

각 Play node의 관찰용 room이 같은 topic을 구독한다. 구독 handler는 event의 `RoomId`가 자신이
관찰하는 room과 일치할 때만 관전자 Actor의 bound session으로 notify를 보낸다.

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo-reward-acquired-event-handler.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo-reward-acquired-event-handler.ts:doc-bingo-reward-subscribe"
```

publish가 정상으로 끝났다는 것은 발행이 시작되었다는 뜻이지 구독자가 처리했다는 뜻이 아니다.
client는 publish 결과가 아니라 관전자에게 도착한 notify로 전달을 확인한다. pub/sub의 형태와 대상
선택은 [Channel 동작 원리](30-channel-patterns.ko.md#5-pubsub의-형태)가 다룬다.

## 9. 종료 cleanup과 disconnect

게임이 끝나면 room은 player Actor를 내보낸다. Actor는 Entry Spot으로 돌아가고, Entry Spot이
destroy 표시를 확인해 Actor를 정리한다. room이 직접 destroy하지 않는 이유는 leave callback이
API에 결과를 기록하는 동안 `Yield`로 실행권을 반납하기 때문이다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-bingo-end-cleanup.html" title="종료 cleanup" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-bingo-end-cleanup.html" target="_blank">↗ 크게 보기</a></p>

`Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts:doc-bingo-room-cleanup"
```

Entry Spot의 joined callback이 표시를 확인하고 destroy를 호출한다. destroy는 Actor 객체, registry와
session 묶음을 정리하며 leave callback을 다시 호출하지 않는다.

`Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo-entry-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo-entry-spot.ts:doc-bingo-entry-destroy"
```

client 연결이 끊기는 것은 별개의 경로다. 연결이 끊기면 framework가 그 시점에 묶여 있던 Actor
전체에 끊김을 알린다. session의 disconnect callback은 그 사실을 로그로 남길 뿐 Actor를
destroy하거나 room에서 내보내지 않는다 — 일부 언어는 통지를 직접 호출하기도 하지만, 자동 통지와
겹쳐도 callback은 한 번만 실행된다.

`Server/Session/Sessions/bingo-session.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Session/Sessions/bingo-session.ts:doc-bingo-session-disconnect"
```

Actor가 Entry Spot으로 돌아가는 순서는 [활성화와 수명](34-activation-lifetime.ko.md)이,
끊김 통지가 도착하는 곳은
[Session 묶음의 동작 원리](39-session-binding.ko.md#3-연결이-끊길-때의-통지)가 다룬다.

## 10. relocation 대비

room Spot factory는 `SpotWide` 실행과 application이 알리는 relocation 시점을 선택하고, 상태를
보존할 adapter를 지정한다. Node 구현은 room Spot의 relocation을 끄고 player Actor의 adapter만
둔다.

`Server/Play/bingo-play-module.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/bingo-play-module.ts:doc-execution-mode"
```

adapter는 domain 상태만 직렬화한다. queue, timer, accepted journal과 owner fence는 framework가
옮기므로 adapter payload에 넣지 않는다.

`Server/Play/Infrastructure/ZLink/Actors/player-actor-relocation-adapter.ts`

```typescript
--8<-- "framework/languages/node/samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Actors/player-actor-relocation-adapter.ts:doc-bingo-relocation-adapter"
```

relocation을 켠 구현의 room은 한 round의 push와 publish가 끝난 turn에서 relocation-ready를
예약한다 — 앞 절의 timer handler 끝에 그 호출이 있다. 이동 단위와 되돌릴 수 있는 지점은
[Relocation](37-relocation.ko.md)이 다룬다.

## 11. 실행과 검증

runner 하나가 Redis 컨테이너, 서버 process와 client 시나리오를 함께 띄운다.

```bash
framework/languages/node/samples/Bingo.Ts/run_sample.sh
```

client는 인증 결과, 매칭 state, push 순서와 reward notify를 assertion으로 확인하고 마지막에
`bingo=completed`를 출력한다. runner는 서버 로그의 lifecycle 행을 세어 `bingo-placement=completed`를
출력한다. 확인 항목과 로그 문자열은
[Bingo 시나리오](../../../common/sample/bingo/README.ko.md#9-client-self-check)가 정한다.

## 12. 관련 문서

- 다른 샘플과의 비교와 선택 기준: [샘플 고르기](14-samples.ko.md)
- 요구사항, 메시지 계약과 완료 기준: [Bingo 시나리오](../../../common/sample/bingo/README.ko.md)
- 같은 게임을 수동 연결·수동 등록으로 만든 구성: [TicTacToe 따라 읽기](51-tictactoe.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
