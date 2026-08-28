# 비동기 실행 모델과 완료 표면 용어

> 이 문서는 bindings 스펙 전반이 쓰는 **비동기/동기, 실행 환경, 완료 표면** 용어를
> 한곳에 정의하는 **참고 문서**다. 계약을 새로 만들지 않는다 — 다른 스펙 문서가
> 이 용어를 그대로 쓴다. 용어가 코드·다른 문서와 어긋나면 이 문서를 기준으로 맞춘다.

## 1. 두 개의 축을 구분한다

혼동을 피하려면 서로 다른 두 질문을 나눈다.

| 축 | 질문 | 값 |
|---|---|---|
| **완료 성격** | 호출이 완료를 **언제** 돌려주나 | **지금**(동기, synchronous) / **나중에**(비동기, asynchronous) |
| **실행 환경** | 대기를 **무엇이** 흡수하나 | OS 스레드 / 가상 스레드·goroutine / 코루틴 / 이벤트 루프 |

완료 성격은 **API terminal**의 성질이고, 실행 환경은 **사용자가 그 API를 부르는 곳**의
성질이다. (완료 성격 자체가 다시 `동기/비동기`와 `blocking/non-blocking` 두 축으로
갈린다 — §2.) 예: 비동기 API를 코루틴에서 `await`할 수도, 이벤트 루프에서 콜백으로 받을 수도
있다. 동기 API를 OS 스레드에서 blocking으로 부를 수도, 가상 스레드에서 부를 수도 있다.

## 2. 완료 성격: 동기/비동기와 blocking/non-blocking은 다른 축이다

두 쌍은 자주 섞어 쓰지만 **서로 다른 질문**에 답한다. 구분해야 정확하다.

| 축 | 질문 | 값 |
|---|---|---|
| **동기 / 비동기** | 완료를 **언제** 전달하나 | 동기: 호출 반환 시점에 완료가 결정됨(그 자리에서 받음) · 비동기: 시작만 하고 완료는 나중에 awaitable/callback으로 통지 |
| **blocking / non-blocking** | 대기 동안 **스레드가 멈추나** | blocking: 진행 불가 시 호출 스레드를 park하고 기다림 · non-blocking: 진행 불가 시 즉시 실패로 반환(대기 없음). `DONTWAIT`가 여기 |

두 축은 **조합된다.** zlink send 종결자를 격자에 놓으면 관계가 분명해진다.

| | **blocking** (진행 불가 시 park) | **non-blocking** (진행 불가 시 즉시 실패) |
|---|---|---|
| **동기** (완료를 그 자리에서) | `submit()`(`NONE`) — admit될 때까지 park 후 결과 | `submit(DONTWAIT)` — 즉시 `BACKPRESSURED`/`EAGAIN` |
| **비동기** (완료는 나중에) | (안 씀 — 막으면서 미루지 않는다) | `async()`/awaitable — 시작 즉시 반환, 완료는 나중에 통지 |

- 따라서 **sync terminal은 flag로 blocking/non-blocking을 고른다**(`NONE`=blocking, `DONTWAIT`=non-blocking).
  async terminal은 **항상 non-blocking**(시작 즉시 반환)이고 완료만 나중이다.
- native `zlink_send_async`는 항상 "비동기·non-blocking" 칸이라 `DONTWAIT`가 의미를 갖는
  "동기·non-blocking" 칸을 표현할 수 없다 — 그래서 sync(+flags) 종결자가 별도로 필요하다
  ([routed 전송 정책](async-coroutine-policy.ko.md) 참조).

- **"비동기(asynchronous)"가 상위어다.** 코루틴뿐 아니라, **스레드 풀·executor에 작업을
  제출하고 완료를 Future 등으로 별도 통지하는 방식**도 비동기다. "지금 최종 완료를 기다리지
  않고 반환, 완료는 나중에 통지"가 성립하면 무엇으로 구현하든 비동기다.
- 다만 **가상 스레드·goroutine에서 blocking API를 호출하는 것 자체는 비동기가 아니다** —
  그 호출은 여전히 동기(blocking)이고, 대기를 런타임이 경량으로 흡수할 뿐이다(§3). 실행
  주체가 경량이라는 사실만으로 API가 비동기가 되지는 않는다.
- "코루틴"을 상위 대표어로 쓰지 않는다 — 가상 스레드·goroutine·이벤트 루프는 코루틴이
  아니기 때문이다(§3).

## 3. 실행 환경

비동기를 **어떻게** 구현하느냐다. 아래 네 가지는 **서로 배타적인 분류가 아니라 조합 가능한
구현 요소**다 — 정확히는 `실행 단위(스레드 / 코루틴)`와 `완료 dispatch 방식(이벤트 루프 /
executor / 직접 continuation)` 두 하위 축의 조합이다. 실무에서 자주 쓰는 조합을 이름으로
정리하면 다음과 같다.

| 실행 환경 | 대기 흡수 방식 | 코드 형태 | 예 |
|---|---|---|---|
| **OS 스레드 (platform thread)** | 커널이 스레드를 park | blocking 호출 | C, C++ `std::thread`, Java platform thread |
| **경량 실행 단위 (가상 스레드 / goroutine)** | 런타임이 경량 실행 단위를 park | blocking 호출 (동기처럼 보임) | Java virtual thread, Go goroutine |
| **코루틴 (coroutine)** | 함수가 실행을 suspend/resume (대개 stackless 상태 머신) | `await`/`suspend` 명시 | Kotlin `suspend`, Python `async def`, C++20 coroutine, Rust `async`/`Future` |
| **이벤트 루프 (event loop)** | 콜백/Promise를 큐에 등록, 단일 스레드 순환 | `await` 또는 콜백 | Node.js, Python asyncio, 브라우저 JS |

언어별 기본 실행 환경 (이 프로젝트 bindings 대상):
> **참고.** 0.14.0부터 send 계열(PAIR/routed/`Received.send()`)은 모든 바인딩에서 async·sync(+flags) 두 terminal을 가진다([정규 표](async-coroutine-policy.ko.md) 소유). 아래 '완료 표면 형태'는 각 언어의 기본/대표 표면만 보인다 — Node/Rust/Python도 sync 종결자가 있다.


| 언어 | 기본 실행 환경 | 완료 표면 형태 | 비고 |
|---|---|---|---|
| **C** | OS 스레드 | blocking | core C API |
| **C++** | OS 스레드 + 코루틴 | blocking `submit()` / `co_await async()` | C++20 coroutine 선택 |
| **.NET** | 코루틴(async/await) + 스레드 풀 | send: async `Async(ct)`→`Task` · sync `Submit(SendFlags)`→`void` | send는 async/sync 두 terminal(0.14.0). **request는 여전히 async 전용** |
| **Java** | 코루틴 없음(`CompletionStage`) · recv는 OS/가상 스레드 | send: async `submit()`→`CompletionStage` · sync `submit(SendFlags)`→`void` | send에 sync overload 추가(0.14.0). request는 async 전용; 가상 스레드는 **선택** |
| **Kotlin** | 코루틴 | `suspend` / `await()` | `kotlinx-coroutines` |
| **Node** | 이벤트 루프 | `Promise` / `await` | 단일 스레드 |
| **Python** | 코루틴 + 이벤트 루프(asyncio) · OS 스레드 | `await` coroutine object / blocking | GIL, `async def` |
| **Rust** | 코루틴(async) | `.await` `Future` | runtime 비종속 |
| **Go** | 가상 스레드(goroutine) | blocking `Submit(ctx)` | channel로 완료 관찰 |

미묘한 구분:

- **가상 스레드 vs 코루틴** — 둘 다 경량 동시성이지만, 가상 스레드는 **blocking 코드를 그대로**
  쓰고 런타임이 뒤에서 park/unpark한다(코드에 비동기가 안 드러남). 코루틴은 `suspend`/`await`를
  **명시**한다(코드에 드러남).
- **코루틴 vs 이벤트 루프** — 직교한다(실행 단위 vs dispatch 방식). 둘은 함께 쓰인다:
  Python asyncio는 **이벤트 루프 위에서 도는 coroutine**이고, Node는 **이벤트 루프에서
  `Promise` continuation을 처리**한다(JS 표준 용어는 `async function`/`Promise`이며, Node
  실행 전체를 "코루틴"이라 부르지 않는다). 두 경우 모두 이벤트 루프가 하부 dispatch 엔진이다.

## 4. 완료 표면(terminal)과 반환값

bindings는 core C API 위에 언어별 **완료 표면(completion surface)**을 제공한다. 이 문서는
**operation을 끝내고 결과 또는 완료 handle을 반환하는 메서드를 `terminal`**이라 부른다
(Java Stream·Rx의 `terminal operation`과는 다른 의미이며, 여기서는 이 문서의 정의를 쓴다).
terminal이 반환하는 값의 표준 이름은 다음과 같다.

| 반환값 | 성격 | 언어 |
|---|---|---|
| **awaitable** (비동기 완료 값) | 비동기 완료를 담는 값의 문서 상위어 | 아래 전부의 총칭 |
| `Task` / `ValueTask` | .NET awaitable | .NET |
| `CompletionStage` / `CompletableFuture` | Java 완료 handle (언어 `await`는 없고 chaining/`.get()`으로 관찰) | Java |
| `Promise` | JS awaitable | Node |
| coroutine object | `await` 가능한 Python awaitable | Python |
| `Future` | Rust awaitable | Rust |
| `async_result_t<T>` | C++ move-only awaitable | C++ (`async()`) |
| completion channel | Go 완료 handle (receive/`select`로 관찰) | Go (request) |

- 문서는 이들을 묶어 **awaitable**로 부르지만, 엄밀히는 `Task`·Python coroutine object·C++
  awaiter처럼 **언어 `await` protocol을 직접 구현하는 값**과, Java `CompletionStage`·Go
  channel처럼 **chaining/receive로 완료를 관찰하는 handle**을 아우르는 총칭이다.
- 동기 terminal의 반환은 awaitable이 아니다 — 언어 관용의 즉시 값(`void`/`bool`/`None`/
  `Result`/`error`)이나 예외다.

## 5. 문서에서 쓰는 대표 용어 (요약)

| 개념 | 문서 표준 용어 | 쓰지 않을 표현 |
|---|---|---|
| 완료를 나중에 받음(상위어) | **비동기(asynchronous)** | (전체를) 코루틴 |
| 완료를 호출 중에 받음 | **동기(synchronous)** | — |
| 대기 동안 스레드 park | **blocking** | — |
| 진행 불가 시 즉시 실패 반환 | **non-blocking** | — |
| 비동기 구현 방식의 총칭 | **실행 환경(execution model)** | — |
| 그 종류(배타 아님, 조합) | **OS 스레드 / 가상 스레드·goroutine / 코루틴 / 이벤트 루프** | — |
| 비동기 완료를 담는 값 | **awaitable** | — |
| API의 완료 표면 | **terminal(완료 표면)** | — |

- **"코루틴"은 실제 코루틴인 언어(Kotlin `suspend`, Python `async def`, C++20, Rust
  `async`)를 특정할 때만** 쓴다. 전체를 아우를 때는 "비동기" 또는 "실행 환경"을 쓴다.
- 여러 실행 환경을 예로 들 때는 "스레드(가상 스레드 포함)·코루틴·이벤트 루프"처럼 나열한다.
