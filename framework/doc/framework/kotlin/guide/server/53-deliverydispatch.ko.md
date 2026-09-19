---
title: "DeliveryDispatch 따라 읽기 · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/53-deliverydispatch.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# DeliveryDispatch 따라 읽기

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: SupportChat 따라 읽기](52-supportchat.ko.md) | [다음: ShoppingMall 따라 읽기](54-shoppingmall.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/53-deliverydispatch.ko.md) · [C#/.NET](../../../dotnet/guide/server/53-deliverydispatch.ko.md) · [Java](../../../java/guide/server/53-deliverydispatch.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/53-deliverydispatch.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    DeliveryDispatch 샘플을 편집기에 열고, HTTP로 접수된 배송 요청이 배송원에게 제안되고 무응답이면
    재배정되어 고객에게 상태가 push되기까지 코드를 따라갈 수 있다. 이 장의 코드는
    `framework/languages/java/samples/kotlin/DeliveryDispatch`에서 그대로 실행된다.

[샘플 고르기](14-samples.ko.md#5-deliverydispatch--배차-시스템-구축)가 이 샘플이 무엇을 보여 주는지
소개했다. 이 장은 그 소개 다음에 읽는 자리다 — 역할과 코드 위치, 주요 시나리오의 메시지 흐름,
각 흐름에 등장하는 framework 기능과 그것을 설명하는 장을 소스가 놓인 순서대로 따라간다.
이 장에는 계약을 소유하는 스펙 문서가 없다. 요구사항, 메시지 계약과 검증 기준은
[DeliveryDispatch 시나리오](../../../common/sample/deliverydispatch/README.ko.md)가 소유하며, 이
장은 그것을 다시 적지 않는다.

## 1. 이 샘플이 보여 주는 것

고객은 HTTP로 배송을 만들고 STREAM으로 상태를 받는다. 배송원은 STREAM으로 제안을 받고 결정을
보낸다. 그 사이의 서버는 session map이나 socket registry를 두지 않는다 — 배송원과 고객은 각각
id를 가진 Actor이고, 제안과 상태는 그 Actor에 묶인 session으로 push된다. 배송원의 응답을 기다리는
동안 어떤 handler도 실행 줄을 점유하지 않으며, 제안의 deadline은 Dispatch가 기록으로 관리한다.

<iframe class="zlink-diagram" src="/common/diagrams/14-delivery.html" title="DeliveryDispatch 샘플 토폴로지" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-delivery.html" target="_blank">↗ 크게 보기</a></p>

이 장이 따라가는 흐름은 배송원 bind → HTTP 접수와 첫 제안 → 제안 push와 결정 → timeout 재배정 →
상태 기록과 고객 push 순서다.

## 2. 역할과 코드 위치

| 역할 | process 수 | 소유하는 것 | 코드 위치 |
| --- | ---: | --- | --- |
| Dispatch | 1 | HTTP 접수, 후보 선택, offer 기록과 deadline, 재배정 | `Server/Dispatch` |
| CourierSession | 1 | 배송원 STREAM, courier Actor bind와 relay | `Server/CourierSession` |
| CourierActorNode | 2 | courier Entry Spot과 courier Actor | `Server/CourierActorNode` |
| Tracking | 1 | 상태 event 기록, customer Actor로 전달 | `Server/Tracking` |
| CustomerGateway | 1 | 고객 STREAM, customer Actor bind와 push | `Server/CustomerGateway` |
| Client | 1 | 고객·배송원 시나리오와 self-check | `Client` |

후보 순서, deadline과 Attempt 검증은 Dispatch의 Application이, 상태 전이는 Tracking의 Domain이
소유하며 framework type을 참조하지 않는다. 메시지는 `shared`의 JSON 계약이 정한다.

## 3. 서버 구성

Dispatch는 courier mesh의 object client이고, dispatch channel의 server이며, tracking channel의
client다. HTTP handler가 dispatch channel로 요청을 보내는 구현에서는 같은 process가 그 channel의
client도 겸한다.

`Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchServerApplication.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchServerApplication.kt:doc-dd-dispatch-register"
```

CourierActorNode는 courier Entry Spot과 courier Actor factory를 object server로 등록한다. 두 node가
같은 type을 제공하므로 Actor가 어느 node에 만들어질지는 framework가 고른다.

`Server/CourierSpotNode/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/courierspotnode/CourierSpotNodeApplication.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/CourierSpotNode/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/courierspotnode/CourierSpotNodeApplication.kt:doc-dd-node-register"
```

channel의 client와 server 역할은 [Channel 메시징](20-channel-messaging.ko.md#4-clientserver-channel)이, object server와
client의 차이는 [Spot](21-spot.ko.md#3-받는-쪽--spot을-호스팅하는-node)이 다룬다.

## 4. 배송원 bind

배송원 client가 STREAM으로 접속해 bind를 요청하면 CourierSession은 courier id로 Actor를 만들거나
찾아 현재 session에 묶는다.

`Server/CourierSession/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/couriersession/sessions/CourierSession.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/CourierSession/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/couriersession/sessions/CourierSession.kt:doc-dd-session-bind"
```

Actor를 얻는 호출은 어느 CourierActorNode에 만들지 정하지 않는다. 묶기는
[Session과 Actor 연결](24-actor-session.ko.md)이 다룬다.

## 5. HTTP 접수와 첫 제안

고객의 HTTP 요청은 Dispatch의 endpoint에 도착한다. handler는 요청을 worker에 넘기고 바로 응답한다 —
일부 구현은 dispatch channel의 one-way send로, 일부는 같은 process의 queue로 넘긴다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-delivery-success.html" title="정상 흐름 — 배차·수락·상태 push" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-delivery-success.html" target="_blank">↗ 크게 보기</a></p>

`Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchHttpServer.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchHttpServer.kt:doc-dd-http-create"
```

worker는 첫 후보를 고르고 Assigned 상태를 Tracking에 알린 뒤, offer를 attempt와 deadline과 함께
기록하고 배송원 Actor에 제안을 보낸다. 제안을 보낸 turn은 거기서 끝난다.

`Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchWorker.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchWorker.kt:doc-dd-offer-start"
```

제안은 courier id를 가리키는 Actor direct send다. 요청이 아니므로 응답을 기다리지 않는다.

`Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchWorker.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchWorker.kt:doc-dd-offer-send"
```

id로 Actor를 호출하는 경로는 [Actor](22-actor.ko.md#3-호출하는-쪽--actor를-호출하는-node)가,
send와 request의 차이는 [Channel 메시징](20-channel-messaging.ko.md#1-요청과-응답)이 다룬다.

## 6. 제안 push와 결정

제안은 courier Actor의 Entry Spot handler에 도착한다. Actor는 attempt를 기억하고 bound session으로
제안 notify를 push한다.

`Server/CourierSpotNode/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/courierspotnode/CourierActor.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/CourierSpotNode/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/courierspotnode/CourierActor.kt:doc-dd-offer-push"
```

배송원의 결정은 같은 연결로 들어와 Actor로 relay된다. handler는 기억한 attempt를 결정에 붙여
dispatch channel로 one-way 전송한다. node는 결정을 판단하지도 시간을 재지도 않는다.

`Server/CourierSpotNode/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/courierspotnode/spots/handlers/CourierDecisionActorHandler.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/CourierSpotNode/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/courierspotnode/spots/handlers/CourierDecisionActorHandler.kt:doc-dd-decision-send"
```

Dispatch의 handler는 결정의 attempt가 기록의 현재 attempt와 같을 때만 반영한다. 재배정 뒤에
도착한 이전 배송원의 결정은 stale로 무시된다.

`Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/handlers/OfferDeliveryResultHandler.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/handlers/OfferDeliveryResultHandler.kt:doc-dd-decision-settle"
```

handler 종류는 [Handler와 메시지 처리](31-handler-dispatch.ko.md#4-spot과-actor의-handler-종류)가
다룬다.

## 7. timeout 재배정

deadline은 Dispatch의 sweeper가 확인한다. 주기마다 만료된 offer를 꺼내 재배정을 호출할 뿐, 아무
handler도 배송원을 기다리지 않는다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-delivery-reassign.html" title="Timeout 재배정 — Attempt=2 승격" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-delivery-reassign.html" target="_blank">↗ 크게 보기</a></p>

`Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/OfferDeadlineSweeper.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/OfferDeadlineSweeper.kt:doc-dd-sweeper"
```

재배정은 다음 후보에게 새 attempt로 같은 절차를 반복한다. 후보가 없으면 Failed 상태로 끝난다.

`Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchWorker.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/Dispatch/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/dispatch/DispatchWorker.kt:doc-dd-reassign"
```

sweeper는 framework의 timer가 아니라 host의 주기 작업이다 — deadline이 Spot의 상태가 아니기
때문이다. Spot 안의 주기 실행이 필요할 때의 형태는
[Timer와 worker](36-timer-worker.ko.md#1-timer--주기-실행)가 다룬다.

## 8. 상태 기록과 고객 push

상태 변경은 tracking channel의 요청으로 Tracking에 도착한다. Tracking은 evidence를 기록하고
customer id를 가리키는 Actor direct send로 CustomerGateway에 전달한다.

`Server/Tracking/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/tracking/handlers/DeliveryStatusChangedHandler.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/Tracking/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/tracking/handlers/DeliveryStatusChangedHandler.kt:doc-dd-tracking-forward"
```

customer Actor의 Entry Spot handler가 그것을 받아 bound session으로 상태 notify를 push한다.
고객이 재접속해도 같은 Actor에 새 session이 묶이므로 push는 새 연결로 간다.

`Server/CustomerGateway/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/customergateway/spots/handlers/DeliveryStatusUpdatedHandler.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/DeliveryDispatch/Server/CustomerGateway/src/main/kotlin/systems/zlink/samples/kotlin/deliverydispatch/server/customergateway/spots/handlers/DeliveryStatusUpdatedHandler.kt:doc-dd-customer-push"
```

bound session으로 보내는 경로와 재접속 뒤의 갱신은
[Session 묶음의 동작 원리](39-session-binding.ko.md)가 다룬다.

## 9. 실행과 검증

runner 하나가 Redis 컨테이너, 서버 process와 client 시나리오를 함께 띄운다.

```bash
framework/languages/java/samples/kotlin/DeliveryDispatch/run_sample.sh
```

client는 정상 배차와 timeout 재배차 두 흐름에서 상태 notify의 순서와 `DeliveryId`를 assertion으로
확인한다. 확인 항목과 로그 문자열은
[DeliveryDispatch 시나리오](../../../common/sample/deliverydispatch/README.ko.md#9-client-self-check)가
정한다.

## 10. 관련 문서

- 다른 샘플과의 비교와 선택 기준: [샘플 고르기](14-samples.ko.md)
- 요구사항, 메시지 계약과 완료 기준:
  [DeliveryDispatch 시나리오](../../../common/sample/deliverydispatch/README.ko.md)
- session 하나에 Actor 여럿을 묶는 구성: [SupportChat 따라 읽기](52-supportchat.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
