---
title: "ShoppingMall 따라 읽기 · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/54-shoppingmall.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# ShoppingMall 따라 읽기

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: DeliveryDispatch 따라 읽기](53-deliverydispatch.ko.md) | [다음: GameQuest 따라 읽기](55-gamequest.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/54-shoppingmall.ko.md) · [C#/.NET](../../../dotnet/guide/server/54-shoppingmall.ko.md) · [Java](../../../java/guide/server/54-shoppingmall.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/54-shoppingmall.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    ShoppingMall 샘플을 편집기에 열고, HTTP로 접수된 주문이 owner Spot에서 재고 예약 → 결제 승인 →
    확정으로 진행되고 실패하면 보상되기까지 코드를 따라갈 수 있다. 이 장의 코드는 [언어별 예제 저장소의 ShoppingMall 샘플](https://github.com/zlink-systems/zlink-java-examples/tree/main/samples/ShoppingMall)에서 가져온다.

[샘플 고르기](14-samples.ko.md#7-shoppingmall--주문-처리-시스템-구축)가 이 샘플이 무엇을 보여 주는지
소개했다. 이 장은 그 소개 다음에 읽는 자리다 — 역할과 코드 위치, 주요 시나리오의 메시지 흐름,
각 흐름에 등장하는 framework 기능과 그것을 설명하는 장을 소스가 놓인 순서대로 따라간다. 이 장은
ShoppingMall 샘플의 역할과 코드 위치, 주요 메시지 흐름, 실행 검증을 소스 순서대로 설명한다.
요구사항, 메시지 계약과 검증 기준은 [ShoppingMall 시나리오](../../../common/sample/event/shoppingmall.ko.md)에서 참고한다.

## 1. 이 샘플이 보여 주는 것

주문 하나는 `OrderId`를 id로 하는 Instance Spot이 소유한다. API는 HTTP를 종단하고 주문 상태를
직접 바꾸지 않으며, 같은 id로 요청을 보내기만 한다. Spot 안의 코드는 event stream을 다시 읽어
현재 단계를 알아내고 다음 단계 하나를 실행한 뒤 event를 기록한다 — 조율 상태, 스케줄러, outbox
같은 별도 계층이 없다.

<iframe class="zlink-diagram" src="/common/diagrams/14-shoppingmall.html" title="ShoppingMall 샘플 토폴로지" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/14-shoppingmall.html" target="_blank">↗ 크게 보기</a></p>

event sourcing은 framework 기능이 아니라 이 샘플이 Spot 위에 둔 application 설계다. framework가
맡는 것은 `OrderId`로 현재 owner를 찾는 routing, 첫 요청에서의 생성, 그리고 한 주문의 handler를
한 번에 하나씩 실행하는 것이다.

## 2. 역할과 코드 위치

| 역할 | process 수 | 소유하는 것 | 코드 위치 |
| --- | ---: | --- | --- |
| CommerceApi | 2 | HTTP 입력 검증, idempotency mapping, workflow 요청, read model 조회 | `Server/CommerceApi` |
| OrderWorkflow | 2 | order workflow Instance Spot, replay·다음 단계·event 기록, 외부 module adapter | `Server/OrderWorkflow` |
| Client | 1 | 주문 시작, polling, 중복·실패·재개·rebuild assertion | `Client` |

상태 전이, event 생성과 보상 규칙은 `Server/OrderWorkflow/Domain`에 있고 framework type을 참조하지
않는다. replay, fold, 다음 단계 판정과 expected version 기록은 `Server/OrderWorkflow/Application`이
맡는다. 메시지는 `shared`의 JSON 계약이 정한다.

## 3. 서버 구성

OrderWorkflow는 mesh의 object server로 order workflow Instance Spot factory를 등록한다. factory는
relocation 때 상태를 옮기지 않고 target에서 다시 만드는 정책을 고른다 — 상태는 event stream에서
복원하면 되기 때문이다.

`Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowApplication.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowApplication.kt:doc-sm-workflow-register"
```

CommerceApi는 같은 mesh의 object client다. 두 API와 두 Workflow가 있지만 어느 쪽도 상대의
endpoint를 알지 않는다.

`Server/CommerceApi/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/commerceapi/CommerceApiApplication.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/CommerceApi/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/commerceapi/CommerceApiApplication.kt:doc-sm-api-register"
```

Instance Spot의 생성 정책은 [활성화와 수명](34-activation-lifetime.ko.md#1-spot의-종류)이,
relocation 때 다시 만드는 정책은
[Relocation](37-relocation.ko.md#3-상태를-담는-시점--factory-등록이-정한다)이 다룬다.

## 4. 주문 시작 — 첫 요청이 owner를 만든다

client가 HTTP로 주문을 시작하면 API는 idempotency key를 확인한다. 이미 시작된 주문이면 read
model을 돌려주고, 아니면 mapping을 예약한 뒤 workflow에 Start를 요청한다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-shoppingmall-start-success.html" title="주문 시작과 성공 처리 흐름" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-shoppingmall-start-success.html" target="_blank">↗ 크게 보기</a></p>

idempotency key가 같은 시작 요청을 같은 주문으로 연결하므로 중복 요청이 새 workflow를 만들지 않는다.

`Server/CommerceApi/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/commerceapi/StartOrderUseCase.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/CommerceApi/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/commerceapi/StartOrderUseCase.kt:doc-sm-api-start"
```

workflow로 가는 요청은 모두 같은 형태다 — `OrderId`를 Spot id로, Instance Spot type과 mesh를
함께 적는다. 그 id의 Spot이 없으면 첫 요청이 eligible node 중 한 곳에 만들고, 있으면 현재
owner가 받는다.

`Server/CommerceApi/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/commerceapi/OrderWorkflowRouter.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/CommerceApi/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/commerceapi/OrderWorkflowRouter.kt:doc-sm-api-request"
```

Spot 쪽에서는 request handler가 요청을 Spot의 start 메서드로 넘긴다.

`Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/handlers/StartOrderWorkflowHandler.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/handlers/StartOrderWorkflowHandler.kt:doc-sm-start-handler"
```

start는 event stream에 시작 event를 기록하고 `Created` 상태를 응답한다. 예약·승인·확정은 응답
뒤에 이어진다.

`Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/handlers/StartOrderWorkflowHandler.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/handlers/StartOrderWorkflowHandler.kt:doc-sm-spot-start"
```

Instance Spot 호출은 [Spot](21-spot.ko.md#6-spot의-다른-종류)이, 같은 id로 동시에 온 요청이 하나의
생성을 기다리는 규칙은 [활성화와 수명](34-activation-lifetime.ko.md)이 다룬다.

## 5. 단계 진행 — replay, 다음 단계, 기록

HTTP 응답은 `Created`에서 끝나고 나머지 단계는 background continuation이 진행한다. client는 polling으로
`Confirmed` 또는 `Failed`를 확인한다.

`Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/WorkflowSagaWorker.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/WorkflowSagaWorker.kt:doc-sm-background-continue"
```

continuation의 한 회전은 event stream을 읽어 aggregate를 복원하는 데서 시작한다. 진행 지점을 따로
저장하지 않는다 — 마지막 event가 곧 현재 단계다.

`Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowService.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowService.kt:doc-sm-replay"
```

현재 단계에 따라 다음 단계 하나를 실행한다. 재고 예약이 거절되면 결제를 호출하지 않고, 결제가
거절되면 이미 기록된 예약을 해제한 뒤 실패로 끝낸다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-shoppingmall-failure-compensation.html" title="재고 실패와 결제 실패 보상" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-shoppingmall-failure-compensation.html" target="_blank">↗ 크게 보기</a></p>

실패한 단계는 다음 단계를 시작하지 않고 이미 기록한 상태에 맞는 보상만 실행한다.

`Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowService.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowService.kt:doc-sm-next-step"
```

event는 expected version으로 기록한다. 이전 owner가 아직 남아 같은 stream에 기록하려 하면 version이
어긋나 실패하므로, 두 owner가 같은 단계를 두 번 실행하지 않는다. 기록 뒤에는 read model을 갱신한다.

`Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowService.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowService.kt:doc-sm-append"
```

이 모든 단계는 같은 Spot의 turn 안에서 실행되므로 한 주문에 경쟁하는 writer가 없다. 직렬 실행의
범위는 [실행 모델](32-execution-model.ko.md)이 다룬다.

## 6. 중복, 재개와 projection 재생성

같은 key로 두 번 시작하면 mapping 예약에서 먼저 성공한 요청의 `OrderId`를 양쪽이 사용하고, stream에
이미 있는 command는 다시 기록되지 않는다. 중단된 주문은 재개 명령으로 같은 continuation을 다시
돌린다 — replay가 마지막 단계를 알려 주므로 완료한 단계는 건너뛴다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-shoppingmall-duplicate-resume.html" title="중복 시작과 중단 후 재개" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-shoppingmall-duplicate-resume.html" target="_blank">↗ 크게 보기</a></p>

조회는 read model만 읽고 주문을 진행시키지 않는다.

`Server/CommerceApi/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/commerceapi/handlers/GetOrderStateHandler.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/CommerceApi/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/commerceapi/handlers/GetOrderStateHandler.kt:doc-sm-get-state"
```

read model이 지워지거나 어긋나면 event stream만으로 다시 만든다.

`Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowService.kt`

```kotlin
--8<-- "framework/languages/java/samples/kotlin/ShoppingMall/Server/OrderWorkflow/src/main/kotlin/systems/zlink/samples/kotlin/shoppingmall/server/orderworkflow/OrderWorkflowService.kt:doc-sm-rebuild"
```

## 7. 종료와 lifecycle

주문이 `Confirmed`나 `Failed`에 도달하면 Spot은 자신을 닫을 수 있다 — .NET과 Node 구현이 그렇게
한다. 닫힌 뒤 같은 `OrderId`로 온 요청은 새 generation의 Spot을 만들고, 그 Spot은 stream을 replay해
terminal 상태를 그대로 돌려준다.

<iframe class="zlink-diagram" src="/common/diagrams/sample-shoppingmall-lifecycle.html" title="Lifecycle과 실패 경계" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/sample-shoppingmall-lifecycle.html" target="_blank">↗ 크게 보기</a></p>

terminal 상태의 Spot을 닫아도 같은 `OrderId`의 다음 요청은 event stream을 replay해 현재 상태를 복원한다.

```kotlin
// 이 구현의 order workflow Spot은 terminal에서 Close하지 않는다.
```

Ready owner의 process가 사라지면 진행 중인 요청은 Unavailable로 끝나고, framework는 다른 node에
같은 주문을 자동으로 만들지 않는다. 계획된 relocation은 별개의 절차이며, 그 뒤의 재개도 위와 같은
replay로 이어진다. Close와 generation은 [활성화와 수명](34-activation-lifetime.ko.md)이, 계획된
이동은 [Relocation](37-relocation.ko.md)이 다룬다.

## 8. 실행과 검증

runner 하나가 Redis 컨테이너, 서버 process와 client 시나리오를 함께 띄운다.

```bash
framework/languages/java/samples/kotlin/ShoppingMall/run_sample.sh
```

client는 정상·실패 결과, 중복 시작, 재개와 projection 재생성을 assertion으로 확인한다. 확인 항목과
로그 문자열은
[ShoppingMall 시나리오](../../../common/sample/event/shoppingmall.ko.md#9-client-self-check)가
정한다.

## 9. 관련 문서

- 다른 샘플과의 비교와 선택 기준: [샘플 고르기](14-samples.ko.md)
- 요구사항, 메시지 계약과 완료 기준:
  [ShoppingMall 시나리오](../../../common/sample/event/shoppingmall.ko.md)
- 같은 owner Spot·event sourcing을 유실을 허용하는 도메인에 적용한 구성:
  [GameQuest 따라 읽기](55-gamequest.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
