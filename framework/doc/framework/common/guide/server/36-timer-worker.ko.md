# Timer와 worker

!!! info "이 장을 읽고 나면"

    Spot 안에서 주기 작업을 돌리고, 오래 걸리는 작업을 Spot queue 밖으로 내보낼 수 있다.
    이 장의 코드는 TicTacToe 샘플에서 가져왔다.

[실행 모델](32-execution-model.ko.md#1-작업이-대기하는-queue)는 Spot queue가
한 Spot의 작업을 순서대로 실행한다는 것을 다룬다. **timer는 주기마다 Spot queue에 작업을 넣고,
worker는 오래 걸리는 작업을 Spot queue 밖의 실행 문맥에서 실행한다.**

## 1. Timer — 주기 실행

Timer는 이름·주기·handler를 Spot context에 등록한다. tick은 그 Spot의 실행 queue에 들어가므로
**handler 안에서 Spot 상태를 그대로 다룬다.** 등록은 timer handle을 돌려주며, 그 handle로 나중에 취소한다.

이름은 같은 Spot 안에서 유일하다. 주기가 `0` 이하이면 등록 시점에 설정 오류다.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/TicTacToeGameTimerHandler.cs:doc-timer-handler"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe_game_spot.hpp:doc-timer-handler"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/spots/tictactoegamespot/handlers/TicTacToeGameTimerHandler.java:doc-timer-handler"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/tictactoegamespot/handlers/TicTacToeGameTimerHandler.kt:doc-timer-handler"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/tictactoe-game-timer-handler.ts:doc-timer-handler"
    ```

### 1.1 예정 시각을 지난 tick

Spot queue에 작업이 쌓이거나 handler 실행이 길어지면 tick이 예정 시각보다 늦게 실행된다. 지나간
tick을 어떻게 처리할지는 overrun 정책이 정한다.

| 값 | 예정 시각을 지났을 때 | 고르는 기준 |
| --- | --- | --- |
| `SkipLateTicks`(기본) | 지나간 tick을 버리고 **현재 시각에 해당하는 tick 하나만** 전달한다 | 최신 상태만 의미 있을 때 — 상태 broadcast, 만료 검사 |
| `CatchUpBounded` | 지나간 tick을 **한도까지** 전달하고 초과분은 버린다 | tick 횟수 자체가 의미 있을 때 — 회복량 누적, 시뮬레이션 step |
| `DelayNextTick` | 고정 주기를 유지하지 않고 **직전 tick이 끝난 시각 + 주기**로 다음 예정을 다시 계산한다 | 실행 사이의 최소 간격을 보장해야 할 때 — 외부 API polling |

한도 값은 `CatchUpBounded`에서만 쓰인다. 기본값은 `1`이고, `0` 이하이면 등록 시점에 설정
오류다.
앞의 두 정책은 timer가 시작한 시각을 기준으로 고정 rate를 유지하므로, 한 tick이 늦게 실행되어도
**다음 tick의 예정 시각은 바뀌지 않는다.**

### 1.2 tick이 전달하는 값

timer handler가 받는 tick은 예정 대비 지연과 건너뛴 tick 수를 필드에 담아 전달한다.

| 필드 | 뜻 |
| --- | --- |
| 이름 | 등록할 때 준 이름 |
| 예정 순번 · 전달 순번 | 몇 번째 예정 tick인지와 실제 전달 순번. 두 값의 차이가 지금까지 버려진 tick 수다 |
| 예정 시각 · 시작 시각 | 예정된 시각과 실제로 실행을 시작한 시각 |
| 지연 | 시작 시각에서 예정 시각을 뺀 값 — 이 tick의 예정 대비 지연 |
| 건너뛴 tick 수 | 이번 tick 직전에 버려진 tick 수 |
| 주기 | 등록한 주기 |

지연이 커지는 것은 그 Spot queue가 밀린다는 신호다. handler가 그 값을 읽어 부하를 보고하면
운영에서 원인을 찾을 자리가 생긴다.

<iframe class="zlink-diagram" src="/common/diagrams/36-timer-worker.html" title="timer는 Spot queue에 들어가고, worker는 별도 실행 문맥에서 실행된다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/36-timer-worker.html" target="_blank">↗ 크게 보기</a></p>

## 2. Worker — Spot queue 밖에서 실행하기

Spot queue는 한 번에 하나만 실행한다. 무거운 계산이나 외부 I/O를 handler 안에서 그대로 기다리면
**그동안 그 Spot의 다른 작업이 전부 멈춘다.** 그런 작업은 worker 호출로 넘긴다. worker 작업은 Spot queue를
거치지 않고 별도 실행 문맥에서 실행되어 Spot의 turn을 점유하지 않는다.

넘길 작업이 **thread를 점유하는 동기 코드**인지 **완료를 기다리는 비동기 코드**인지에 따라 호출이 달라진다.

CPU worker에는 직렬화·압축·경로 탐색·이미지 처리처럼 CPU를 계속 사용하는 동기 계산 함수를 넘긴다.
I/O worker에는 DB·파일·HTTP처럼 응답을 기다리는 비동기 호출 함수를 넘긴다.

worker thread 풀 자체 — 최소·최대 thread, 유휴 시간, 대기열 길이 — 는 루트 옵션에서 정한다.

## 3. 실행권을 반납하는 종결자

worker 호출을 어떤 종결자로 닫느냐가 **기다리는 동안 그 Spot의 실행권을 쥐고 있는지**를 정한다.

| 종결자 | Spot 실행권 | 사용하는 자리 |
| --- | --- | --- |
| `Yield` | 기다리는 동안 **반납한다** | 기본 선택. 그 사이 같은 Spot의 다른 작업이 실행된다 |
| `Async` | 기다리는 동안 **쥐고 있는다** | 작업이 짧고, 기다리는 동안 Spot 상태가 바뀌면 안 될 때 |
| `Submit` | 즉시 돌아온다 | 결과를 기다리지 않고 제출만 할 때 |

`Yield`와 `Async`의 turn 차이, `Yield` 뒤 상태 재확인과 호출에 넘길 값의 사전 복사, 사용할 수
있는 Spot은 [실행 모델 §5](32-execution-model.ko.md#5-직렬-실행과-thread-점유)가 다룬다.

## 4. 관련 문서

- 무엇이 Spot queue에서 순서대로 실행되는가 — [실행 모델](32-execution-model.ko.md)
- 도착이 처리보다 빠를 때 — [Backpressure](33-backpressure.ko.md)
- timer가 이동을 만났을 때 — [Relocation](37-relocation.ko.md)
- 옵션의 정확한 이름과 기본값 — 언어별 `16. Options` 장

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
