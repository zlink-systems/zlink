---
title: "실행 모델 · C++"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/32-execution-model.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 실행 모델

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 모니터링](26-monitoring.ko.md) | [다음: Backpressure — 처리보다 도착이 빠를 때](33-backpressure.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — **C++** · [C#/.NET](../../../dotnet/guide/server/32-execution-model.ko.md) · [Java](../../../java/guide/server/32-execution-model.ko.md) · [Kotlin](../../../kotlin/guide/server/32-execution-model.ko.md) · [Node/TypeScript](../../../node/guide/server/32-execution-model.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    한 Spot 안에서 무엇이 함께 실행되고 무엇이 줄을 서는지, 그 경계를 무엇이 정하는지 알 수
    있다. 이 장의 코드는 Bingo 샘플에서 가져왔다.

[Spot](21-spot.ko.md)과 [Actor](22-actor.ko.md)는 "자기 앞으로 온 일을 한 번에 하나씩
처리한다"로 요약했다. 이 장은 그 문장이 어디까지 참인지 다룬다 — 어떤 작업이 같은 줄에 서고,
어떤 작업이 나뉘며, 그 경계를 정하는 것이 무엇인가.

## 1. 작업이 대기하는 queue

Spot으로 들어오는 작업은 다음 queue로 나뉘어 대기한다. Spot 자신에게 온 packet과 timer는
**Spot queue**에, 그 Spot에 속한 Actor 앞으로 온 payload는 **Actor queue**에 들어간다.

**Actor 앞으로 온 업무 message는 Spot queue를 거치지 않는다.** Spot의 callback이 그 message를
받아 Actor에게 넘겨주는 구조가 아니라, 처음부터 Actor queue로 들어간다.

| queue | 들어간다 | 들어가지 않는다 |
| --- | --- | --- |
| Spot queue | Spot 앞 payload, 일치한 Logical Multicast payload, timer callback, Actor의 join·leave와 lifecycle callback | **Actor 업무 payload** |
| Instance Spot의 queue | Spot 앞 payload와 timer callback | Actor에 관한 전부. **등록 시점에 거부한다** |
| Actor queue | Actor 업무 payload | — |

Instance Spot에 Actor membership이나 Logical Multicast 구독을 등록하려 하면 실행 중이 아니라
**등록하거나 Spot을 준비하는 시점에** 거부된다.

## 2. 직렬화 범위를 정하는 것

서로 다른 queue의 작업을 동시에 실행할 수 있는지는 **Spot의 종류와 execution mode**가 정한다.

| | 직렬화 범위 | 상태 소유 |
| --- | --- | --- |
| Entry Spot | Spot queue와 Actor queue를 각각 직렬화한다. 서로 다른 queue는 동시에 실행할 수 있다 | Actor가 각자 소유한다. Actor 사이에 공유하는 상태는 외부 저장소에 둔다 |
| User Spot `SpotWide`(기본) | Spot handler, member Actor handler, timer, lifecycle callback 전체를 공통 gate 하나로 직렬화한다 | Spot instance가 소유한다. Actor와 공유하는 상태에도 별도 동기화가 필요하지 않다 |
| User Spot `PerActor` | Actor별, Spot lane별로 각각 직렬화한다. 서로 다른 lane은 동시에 실행할 수 있다 | Actor가 각자 소유한다. lane 사이에 공유하는 상태는 외부 저장소에 둔다 |
| Instance Spot | Spot queue의 handler와 timer를 직렬화한다. Actor queue가 없다 | Spot instance가 소유한다 |

<iframe class="zlink-diagram" src="/common/diagrams/06-spot.html" title="Spot 실행 모델 — SpotWide와 PerActor" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/06-spot.html" target="_blank">↗ 크게 보기</a></p>

Execution mode는 factory를 등록할 때 고정하며 실행 중에는 바꾸지 않는다.

## 3. SpotWide — gate 하나를 지나는 실행

기본값은 `SpotWide`다. 그 Spot으로 향하는 모든 callback이 — 다른 Actor의 message도, timer도,
lifecycle callback도 — 공통 gate 하나를 지나 한 lane에서 turn 하나씩 실행된다.

<iframe class="zlink-diagram" src="/common/diagrams/06-spotwide-lockfree.html" title="SpotWide — 락 없는 순차 실행" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/06-spotwide-lockfree.html" target="_blank">↗ 크게 보기</a></p>

같은 순간에 실행되는 turn이 없으므로 **handler는 Spot과 member Actor의 상태를 락 없이 평범한
코드로 직접 다룬다.** Relocation에서도 Spot과 소속 Actor가 한 단위로 함께 이동한다. 대신
오래 걸리는 callback 하나가 그 Spot의 다음 callback 전체를 지연시킨다.

기본값이라 적지 않아도 되지만, 적어 두면 그 Spot이 어느 mode로 실행되는지가 등록에 드러난다.

```cpp
--8<-- "framework/languages/cpp/samples/Bingo/Server/Play/play_server_host_factory.hpp:doc-execution-mode"
```

## 4. PerActor — lane마다 하나

`PerActor`는 Actor마다 독립적으로 실행해야 처리량을 얻는 경우에 고른다. Spot 자체는 상태를 두지 않고 실행 단위로만 사용한다.

서로 다른 lane이 동시에 실행되므로 **여러 Actor가 함께 바꾸는 상태와 Spot 단위의 예약은 Redis나
database 같은 외부 저장소에 둔다.** Factory의 이동 정책은 `RecreateOnRelocation`만 사용할 수 있다.

**Entry Spot도 같은 모델이다.** Spot queue와 Actor queue가 나뉘어 있으므로 같은 제약을 받는다.

## 5. 직렬 실행과 thread 점유

직렬 실행은 thread 하나를 계속 점유한다는 뜻이 아니다. Handler가 대기 지점에 도달하면 실행
thread는 다른 일을 처리하지만, **그 turn은 handler가 끝날 때까지 유지된다.** `SpotWide`에서는
그동안 같은 Spot의 다음 callback을 시작하지 않는다.

오래 걸리는 I/O를 기다리는 동안 다음 turn을 실행해야 하면
[Timer와 worker](36-timer-worker.ko.md)의 `Yield` 계약을 사용한다.

## 6. 관련 문서

- id로 호출하는 상태 객체 — [Spot](21-spot.ko.md) · [Actor](22-actor.ko.md)
- 종류별 생성 시점과 lifecycle callback — [활성화와 수명](34-activation-lifetime.ko.md)
- turn을 넘기는 계약 — [Timer와 worker](36-timer-worker.ko.md)
- 도착이 처리보다 빠를 때 — [Backpressure](33-backpressure.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
