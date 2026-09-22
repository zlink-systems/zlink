---
title: "SupportChat 따라 읽기 · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/52-supportchat.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# SupportChat 따라 읽기

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: TicTacToe 따라 읽기](51-tictactoe.ko.md) | [다음: DeliveryDispatch 따라 읽기](53-deliverydispatch.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/52-supportchat.ko.md) · [C#/.NET](../../../dotnet/guide/server/52-supportchat.ko.md) · [Java](../../../java/guide/server/52-supportchat.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/52-supportchat.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    SupportChat 샘플을 편집기에 열고, 고객의 상담 요청이 상담원에게 배정되고 대화가 닫히기까지
    메시지가 어느 서버의 어느 코드를 지나는지 따라갈 수 있다. 이 장의 코드는 [언어별 예제 저장소의 SupportChat 샘플](https://github.com/zlink-systems/zlink-java-examples/tree/main/samples/SupportChat)에서 가져온다.

[샘플 고르기](14-samples.ko.md#4-supportchat--라이브-채팅-상담-시스템-구축)가 이 샘플이 무엇을
보여 주는지 소개했다. 이 장은 그 소개 다음에 읽는 자리다 — 역할과 코드 위치, 주요 시나리오의
메시지 흐름, 각 흐름에 등장하는 framework 기능과 그것을 설명하는 장을 소스가 놓인 순서대로
따라간다. 이 장은 SupportChat 샘플의 역할과 코드 위치, 주요 메시지 흐름, 실행 검증을 소스 순서대로
설명한다. 요구사항, 메시지 계약과 검증 기준은 [SupportChat 시나리오](../../../common/sample/supportchat/README.ko.md)에서 참고한다.

## 1. 이 샘플이 보여 주는 것

고객과 상담원은 각각 Session 서버의 STREAM 연결 하나만 유지한다. 인증은 API가, 대화 상태는
Support의 conversation Spot이 소유한다. 상담원 한 명이 여러 대화를 동시에 처리하므로, 상담원의
연결 하나에 roster Actor와 대화별 conversation Actor가 함께 묶이고, 들어오는 packet은 stream
metadata의 `ConversationId`로 어느 Actor에 갈지 정해진다.

<iframe class="zlink-diagram" src="/common/diagrams/14-supportchat.html" title="SupportChat 샘플 토폴로지" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-supportchat.html" target="_blank">↗ 크게 보기</a></p>

이 장이 따라가는 흐름은 인증과 identity Actor bind → 상담 열기와 배정 → 상담원의 대화 join →
metadata로 relay되는 채팅 → idle timer와 종료 순서다.

## 2. 역할과 코드 위치

| 역할 | process 수 | 소유하는 것 | 코드 위치 |
| --- | ---: | --- | --- |
| Session | 1 | STREAM 연결, 인증 packet, Actor bind와 metadata relay | `Server/Session` |
| API | 1 | token 검증, conversation Spot 생성 요청 | `Server/Api` |
| Support | 1 | Entry Spot, conversation Spot, identity·roster·conversation Actor, 배정과 push | `Server/Support/Infrastructure` |
| Client | 1 | 고객 시나리오와 상담원 시나리오, self-check | `Client` |

참가자, `MessageSeq`, typing과 idle·close 전이는 `Server/Support/Domain`에 있고 framework type을
참조하지 않는다. 상담원의 capacity 판단은 `Server/Support/Application`이 맡는다. 메시지는
`shared`의 JSON 계약이 정한다.

## 3. 서버 구성

Session은 Support mesh의 object를 호출하고 API channel을 호출하며 STREAM 연결을 받는다.

`Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/SessionApplication.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/SessionApplication.kt:doc-sc-session-register"
```

Support는 Entry Spot, Actor factory와 conversation Spot factory를 등록하고, session과 Actor 사이에
오갈 수 있는 metadata key를 허용한다.

`Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/SupportApplication.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/SupportApplication.kt:doc-sc-support-register"
```

metadata를 허용하지 않으면 session이 붙인 `ConversationId`는 오류 없이 값만 전달되지 않는다.
허용 key는 [Options](16-options.ko.md#10-반드시-지정하는-값)에 반드시 지정하는 값으로 있다.

## 4. 인증과 identity Actor

client가 보낸 첫 packet은 인증 요청이다. session은 packet 이름으로 인증, 대화 join, 그 밖의
packet을 나눈다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-supportchat-auth-join.html" title="인증, 상담 생성과 agent join" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-supportchat-auth-join.html" target="_blank">↗ 크게 보기</a></p>

인증 뒤에는 identity Actor를 session에 묶어 이후 packet을 identity 또는 conversation Actor로 relay한다.

`Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt:doc-sc-session-dispatch"
```

인증이 끝나면 session은 identity Actor를 만들거나 찾아 현재 session에 묶는다. 고객이면 그 Actor가
곧 대화의 참가자이고, 상담원이면 roster Actor다.

`Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt:doc-sc-session-auth"
```

같은 id로 다시 접속하면 같은 Actor에 새 session이 묶인다. 묶기는
[Session과 Actor 연결](24-actor-session.ko.md)이 다룬다.

## 5. 상담 열기와 상담원 배정

고객의 open 요청은 identity Actor로 relay되어 Entry Spot의 actor request handler에 도착한다.
handler는 API에 conversation 생성을 요청하고, 받은 id로 conversation Spot join을 예약한다.

`Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/handlers/OpenConversationActorHandler.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/handlers/OpenConversationActorHandler.kt:doc-sc-open-actor"
```

API는 Spot manager로 conversation Spot을 만든다. framework가 id를 발급하고 owner를 고른다.

`Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/FrameworkConversationStarter.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/FrameworkConversationStarter.kt:doc-sc-api-open"
```

고객 Actor의 join이 끝나면 conversation Spot이 capacity가 남은 상담원을 고르고, 그 roster Actor의
bound session으로 배정 notify를 보낸다. 남은 상담원이 없으면 오류가 아니라 대기 상태다.

`Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/ConversationSpot.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/ConversationSpot.kt:doc-sc-assign"
```

roster Actor는 Entry Spot에 있고 conversation Spot의 member가 아니다. 그래도 bound session으로
보내는 데는 Actor의 위치가 필요 없다.

`Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/notifications/ConversationNotificationPublisher.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/notifications/ConversationNotificationPublisher.kt:doc-sc-roster-push"
```

User Spot 생성은 [Spot](21-spot.ko.md#4-호출하는-쪽--spot을-호출하는-node)이, join 예약은
[Actor membership](35-actor-membership.ko.md#2-예약-등록--handler가-끝난-뒤에-실행된다)이 다룬다.

## 6. 상담원의 대화 join — 연결 하나, Actor 여럿

한 Actor는 동시에 한 Spot에만 속한다. roster Actor는 Entry Spot에 남아야 하므로, 상담원이 대화에
들어갈 때 session은 대화별 conversation Actor를 새로 만들어 같은 session에 추가로 묶는다.

`Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt:doc-sc-agent-join"
```

이후 그 대화의 push는 conversation Actor의 bound session, 곧 상담원의 같은 연결로 도착한다. 묶을
수 있는 개수와 규칙은
[Session 묶음의 동작 원리](39-session-binding.ko.md#1-묶을-수-있는-개수--session은-여럿-actor는-하나)가
다룬다.

## 7. 채팅 — metadata로 고르는 relay

채팅 packet은 payload를 해석하지 않고 stream metadata의 `ConversationId`로 대상 Actor를 고른다.
상담원의 map에 있으면 그 conversation Actor로, 없으면 identity Actor로 relay한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-supportchat-chat-typing.html" title="채팅과 typing" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-supportchat-chat-typing.html" target="_blank">↗ 크게 보기</a></p>

`ConversationId` metadata가 상담원의 여러 conversation Actor 중 하나를 고르므로 payload를 해석하지 않아도 relay 대상이 정해진다.

`Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Session/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/session/sessions/SupportChatSession.kt:doc-sc-metadata-relay"
```

conversation Spot은 메시지에 `MessageSeq`를 부여하고, 보낸 사람을 제외한 참가자의 bound session으로
notify를 push한다.

`Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/notifications/ConversationNotificationPublisher.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/notifications/ConversationNotificationPublisher.kt:doc-sc-message-push"
```

응답 token의 수명은 [STREAM의 동작 원리](38-stream-boundary.ko.md#3-응답-token의-수명)가, Spot 안에서
한 번에 하나씩 실행되는 범위는 [실행 모델](32-execution-model.ko.md)이 다룬다.

## 8. idle timer와 종료

마지막 메시지 뒤 일정 시간이 지나면 conversation Spot의 timer가 대화를 idle로 옮기고, grace 시간
안에 메시지가 없으면 닫는다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-supportchat-idle-close.html" title="idle, close와 reconnect" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-supportchat-idle-close.html" target="_blank">↗ 크게 보기</a></p>

idle timer는 대화 state를 바꾸고, 연결 끊김은 availability만 바꾸므로 두 경로는 서로 다른 상태를 다룬다.

`Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/ConversationSpot.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/conversationspot/ConversationSpot.kt:doc-sc-idle-timer"
```

대화가 닫히면 배정된 상담원의 capacity가 돌아온다. 상담원의 연결이 끊기면 Entry Spot의 disconnect
callback이 availability를 내리고, 대화 상태는 그대로 남아 재접속 뒤 이어진다.

`Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/SupportEntrySpot.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/SupportEntrySpot.kt:doc-sc-agent-disconnect"
```

availability는 상담원이 요청으로 켜고 끈다.

`Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/handlers/SetAgentAvailableHandler.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/SupportChat/Server/Support/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/server/support/infrastructure/zlink/spots/entryspot/handlers/SetAgentAvailableHandler.kt:doc-sc-set-available"
```

timer는 [Timer와 worker](36-timer-worker.ko.md#1-timer--주기-실행)가, 끊김 통지가 도착하는 곳은
[Session 묶음의 동작 원리](39-session-binding.ko.md#3-연결이-끊길-때의-통지)가 다룬다.

## 9. 실행과 검증

runner 하나가 Redis 컨테이너, 서버 process와 client 시나리오를 함께 띄운다.

```bash
framework/languages/java/samples/kotlin/SupportChat/run_sample.sh
```

client는 배정 notify, `MessageSeq` 순서, typing notify, idle과 close 전이, 재접속 뒤의 state를
assertion으로 확인한다. 확인 항목과 로그 문자열은
[SupportChat 시나리오](../../../common/sample/supportchat/README.ko.md#9-client-self-check)가
정한다.

## 10. 관련 문서

- 다른 샘플과의 비교와 선택 기준: [샘플 고르기](14-samples.ko.md)
- 요구사항, 메시지 계약과 완료 기준: [SupportChat 시나리오](../../../common/sample/supportchat/README.ko.md)
- session 하나에 Actor 하나만 묶는 구성: [Bingo 따라 읽기](50-bingo.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
