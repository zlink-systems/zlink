---
title: "GameQuest 따라 읽기 · Node/TypeScript"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/55-gamequest.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# GameQuest 따라 읽기

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: ShoppingMall 따라 읽기](54-shoppingmall.ko.md) | [다음: ZoneWorld 따라 읽기](56-zoneworld.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/55-gamequest.ko.md) · [C#/.NET](../../../dotnet/guide/server/55-gamequest.ko.md) · [Java](../../../java/guide/server/55-gamequest.ko.md) · [Kotlin](../../../kotlin/guide/server/55-gamequest.ko.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    GameQuest 샘플을 편집기에 열고, client의 플레이 action이 player별 owner Spot에서 판정되어 진행과
    완료가 push되기까지, 그리고 유실된 진행을 보정하기까지 코드를 따라갈 수 있다. 이 장의 코드는
    `framework/languages/node/samples/GameQuest.Ts`에서 그대로 실행된다.

[샘플 고르기](14-samples.ko.md#7-gamequest--퀘스트-진행-시스템-구축)가 이 샘플이 무엇을 보여 주는지
소개했다. 이 장은 그 소개 다음에 읽는 자리다 — 역할과 코드 위치, 주요 시나리오의 메시지 흐름,
각 흐름에 등장하는 framework 기능과 그것을 설명하는 장을 소스가 놓인 순서대로 따라간다.
이 장에는 계약을 소유하는 스펙 문서가 없다. 요구사항, 메시지 계약과 검증 기준은
[GameQuest 시나리오](../../../common/sample/event/gamequest.ko.md)가 소유하며, 이 장은 그것을
다시 적지 않는다.

## 1. 이 샘플이 보여 주는 것

client의 action은 GameApi가 검증해 gameplay event로 만들고, 같은 `PlayerId`의 event는 QuestMission의
owner Spot 하나가 순서대로 판정한다. 판정 결과는 event stream에 기록되고, 진행과 완료는 player의
session Actor를 거쳐 연결로 push된다. ShoppingMall과 같은 owner Spot·event sourcing 구성이지만,
진행 tier의 메시지는 best-effort다 — 유실되면 authoritative fact를 읽어 보정한다.

<iframe class="zlink-diagram" src="/common/diagrams/14-gamequest.html" title="GameQuest 샘플 토폴로지" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-gamequest.html" target="_blank">↗ 크게 보기</a></p>

이 장이 따라가는 흐름은 join과 session Actor bind → action 접수와 owner로의 send → owner Spot의
판정과 기록 → 진행 push → 보정과 종료 순서다.

## 2. 역할과 코드 위치

| 역할 | process 수 | 소유하는 것 | 코드 위치 |
| --- | ---: | --- | --- |
| GameApi | 2 | STREAM session, session Actor, action 검증과 gameplay event, owner 호출 | `Server/GameApi` |
| QuestMission | 2 | player quest Instance Spot, replay·판정·append·projection, notify | `Server/QuestMission` |
| Client | 1 | join, action, reconnect, reconcile assertion | `Client` |

quest 조건, aggregate fold와 완료 규칙은 `Server/QuestMission/Domain`에 있고 framework type을
참조하지 않는다. action 검증과 event 생성은 `Server/GameApi/Application`이 맡는다. 메시지는
`shared`의 JSON 계약이 정한다.

## 3. 서버 구성

GameApi는 Entry Spot과 session Actor factory를 object server로 등록하고 STREAM 연결을 받는다.
QuestMission은 같은 mesh에 player quest Instance Spot factory를 등록한다.

`Server/GameApi/game-api-module.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/GameApi/game-api-module.ts:doc-gq-api-register"
```

QuestMission 쪽은 mesh의 object server로 Instance Spot factory만 등록한다.

`Server/QuestMission/gamequest-quest-module.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/QuestMission/gamequest-quest-module.ts:doc-gq-mission-register"
```

두 역할이 같은 mesh를 공유하고 mission별 channel 이름을 두지 않는다. Instance Spot factory는
relocation 때 target에서 다시 만드는 정책을 고른다 — 상태는 event stream에서 복원하면 된다.
종류별 생성 정책은 [활성화와 수명](34-activation-lifetime.ko.md#1-spot의-종류)이 다룬다.

## 4. join과 session Actor

client가 join하면 GameApi의 session handler가 `PlayerId`로 session Actor를 만들거나 찾아 현재
session에 묶는다. 이 Actor는 quest 상태를 갖지 않는다 — owner Spot이 보낸 notify를 연결로 옮기는
자리다.

`Server/GameApi/game-api-session.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/GameApi/game-api-session.ts:doc-gq-join-bind"
```

다른 GameApi에 재접속해도 같은 id의 Actor에 새 session이 묶인다. 묶기는
[Session과 Actor 연결](24-actor-session.ko.md)이 다룬다.

## 5. action 접수와 owner로의 send

action packet은 묶인 Actor로 relay되어 Entry Spot의 actor request handler에 도착한다. handler는
application service에 위임하고 발급된 `eventId`를 응답한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-gamequest-progress-flow.html" title="정상 progress와 completion" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-gamequest-progress-flow.html" target="_blank">↗ 크게 보기</a></p>

`Server/GameApi/Infrastructure/ZLink/gamequest-player-handlers.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/GameApi/Infrastructure/ZLink/gamequest-player-handlers.ts:doc-gq-action-handler"
```

service는 idempotency key로 event를 저장한 뒤 owner에 보낸다. 같은 key의 재요청은 같은 `eventId`가
된다. owner가 Unavailable이면 그대로 실패한다 — framework가 다른 node에 다시 보내지 않는다.

`Server/GameApi/Application/gameplay-action-service.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/GameApi/Application/gameplay-action-service.ts:doc-gq-store-dispatch"
```

owner로 가는 메시지는 `PlayerId`를 Spot id로 하는 one-way send다. 그 id의 Spot이 없으면 이 첫
메시지가 eligible node 중 한 곳에 Spot을 만든다.

`Server/GameApi/Infrastructure/ZLink/gameplay-event-publisher.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/GameApi/Infrastructure/ZLink/gameplay-event-publisher.ts:doc-gq-owner-send"
```

응답은 GameApi가 접수했다는 뜻이지 owner가 판정했다는 뜻이 아니다. 판정 결과는 뒤에 오는 push로
확인한다. Instance Spot 호출은 [Spot](21-spot.ko.md#6-spot의-다른-종류)이, send와 request의 차이는
[Channel 메시징](20-channel-messaging.ko.md#1-요청과-응답)이 다룬다.

## 6. owner Spot의 판정과 기록

Spot이 만들어지면 initialize callback이 `PlayerId`와 generation을 기록한다.

`Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot.ts:doc-gq-spot-init"
```

gameplay 메시지는 Spot의 packet handler에 도착한다.

`Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot-handlers.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot-handlers.ts:doc-gq-apply-handler"
```

processor는 (`PlayerId`, `QuestId`) stream을 읽어 aggregate를 복원하고, 조건을 판정해 event를
append한 뒤 projection을 갱신한다. 이미 저장된 source event는 다시 append되지 않는다.

`Server/QuestMission/Application/quest-event-processor.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/QuestMission/Application/quest-event-processor.ts:doc-gq-process"
```

같은 player의 메시지는 한 Spot의 turn 안에서 한 번에 하나씩 처리되므로 fold와 append 사이에
경쟁하는 writer가 없다. 직렬 실행의 범위는 [실행 모델](32-execution-model.ko.md)이 다룬다.

## 7. 진행 push — owner에서 session Actor로

append가 끝나면 QuestMission은 진행과 완료 메시지를 `PlayerId`를 가리키는 Actor direct send로
보낸다. 어느 GameApi에 그 Actor가 있는지는 framework가 찾는다.

`Server/QuestMission/Infrastructure/ZLink/player-quest-notifier.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/QuestMission/Infrastructure/ZLink/player-quest-notifier.ts:doc-gq-notify-actor"
```

GameApi의 Entry Spot handler가 그것을 받아 Actor의 bound session으로 notify를 push한다.

`Server/GameApi/Infrastructure/ZLink/quest-notification-handlers.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/GameApi/Infrastructure/ZLink/quest-notification-handlers.ts:doc-gq-progress-push"
```

session이 묶여 있지 않은 동안의 notify는 성공 조건이 아니다. 상태는 event store에 있고, 재접속한
client는 조회로 복원한다. id로 Actor를 호출하는 경로는
[Actor](22-actor.ko.md#3-호출하는-쪽--actor를-호출하는-node)가 다룬다.

## 8. 보정과 종료

진행 메시지가 유실되어 fact와 fold가 어긋나면 client나 운영 trigger가 sync 요청을 보낸다. owner
Spot은 authoritative snapshot을 읽어 같은 판정 경로로 보정 event를 append한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-gamequest-reconcile-flow.html" title="reset/reconcile와 failure boundary" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-gamequest-reconcile-flow.html" target="_blank">↗ 크게 보기</a></p>

`Server/QuestMission/Application/quest-event-processor.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/QuestMission/Application/quest-event-processor.ts:doc-gq-sync"
```

명시적 close 뒤에 같은 `PlayerId`로 온 메시지는 새 generation의 Spot을 만들고, 그 Spot은 stream을
replay해 이어간다.

`Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot-handlers.ts`

```typescript
--8<-- "framework/languages/node/samples/GameQuest.Ts/Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot-handlers.ts:doc-gq-close-handler"
```

Ready owner의 process가 사라지면 진행 중인 요청은 Unavailable로 끝나고 framework는 다른 node에
같은 Spot을 자동으로 만들지 않는다. Close와 generation은
[활성화와 수명](34-activation-lifetime.ko.md)이 다룬다.

## 9. 실행과 검증

runner 하나가 Redis 컨테이너, 서버 process와 client 시나리오를 함께 띄운다.

```bash
framework/languages/node/samples/GameQuest.Ts/run_sample.sh
```

client는 action 응답, 진행과 완료 notify, 재접속 뒤의 조회, 보정 결과를 assertion으로 확인한다.
확인 항목과 로그 문자열은
[GameQuest 시나리오](../../../common/sample/event/gamequest.ko.md#9-client-self-check)가 정한다.

## 10. 관련 문서

- 다른 샘플과의 비교와 선택 기준: [샘플 고르기](14-samples.ko.md)
- 요구사항, 메시지 계약과 완료 기준:
  [GameQuest 시나리오](../../../common/sample/event/gamequest.ko.md)
- 같은 owner Spot·event sourcing을 무손실 도메인에 적용한 구성:
  [ShoppingMall 따라 읽기](54-shoppingmall.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
