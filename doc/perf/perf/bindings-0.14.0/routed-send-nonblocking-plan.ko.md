# send 계열 sync(+flags)/async 종결자 정리 계획 (최종)

> 작성: 2026-08-28
> 근거: 전 언어 terminal 조사, async-coroutine-policy.ko.md, README.ko.md:2132·2159,
>       사용자 확정(2026-08-28)

## 문제

send 계열에서 **nonblocking(DONTWAIT) 즉시 backpressure** 경로가 대부분 언어 공개 계약에서
사라졌다. native `zlink_send_async`는 항상 nonblocking이고 flags 필드가 없어 async terminal로는
`DONTWAIT`를 표현할 수 없는데, routed send가 대부분 async 단일 terminal이라 그 경로가 없다.
`README.ko.md:2159`는 SendOp에 `flags(...)`(DONTWAIT 포함)를 요구하지만 어느 언어도 안 지킨다.

## 원리 (왜 대상이 갈리는가)

send 계열이 대상인 이유는 **HWM 대기가 발생할 수 있기 때문**이다. 대기가 있으면
"기다리는 async"와 "즉시 실패하는 sync(DONTWAIT)" 두 경로가 모두 의미를 갖는다.

| 연산 | HWM 대기 | 전송 경로 | 필요한 종결자 |
|---|---|---|---|
| routed send (DEALER/ROUTER) | 있음 | data-plane | **sync(+flags) / async 짝** |
| `Received.send()` | 있음 | data-plane (routed send의 편의형) | **동일** |
| PAIR send | 있음 | data-plane | **동일** |
| `reply()` | **없음** (별도 control 소켓/completion lane) | control-plane | **동기 1개, flags 없음** — 손대지 않음 |
| publish (PUB/XPUB) | 없음 (lossy) | — | 별도 (동기) |
| recv | — (도착분 꺼냄, 즉시 반환 가능) | — | **동기 1개 + flags** — 이미 통일됨, 안 나눔 |

- `reply()`는 core reply 함수에 send flag 인자가 없고 HWM에 안 걸린다(control 소켓). 동기 하나로
  완결이며 flags가 없다. route 없으면 `NOT_CONNECTED`. 우리 규칙 대상이 아니다.
- recv는 고정 스레드에서 쓰고 도착분을 즉시 반환하므로 `recv(flags)` 동기 하나로 충분하다.
  send와 억지로 대칭시키지 않는다.

## 적용 범위 (확정)

**send 계열 3종 — routed send, `Received.send()`, PAIR send — 에 동일 규칙을 적용한다.**
- async 종결자: 기존 유지 (이름 그대로).
- **sync + flags 종결자 추가**: 기본 blocking, `DONTWAIT` flag로 즉시 backpressure.
  native는 `zlink_send_part(_rid)` 경로.

이로써 조사가 발견한 불일치가 함께 해소된다:
- .NET PAIR가 이미 sync `Submit()+Flags()`인 것 → send 계열은 sync가 정상이므로 정합.
- Python/Rust `Received.send()`가 async라 :2159와 어긋난 것 → sync+flags 추가로 정합.
- Python multipart의 숨은 동기 fallback → 공개 sync 종결자로 양성화.

## 언어별 종결자와 시그니처

`async` = 기존 async 종결자(유지), `sync` = 추가할 sync+flags 종결자.

### C++
- async: `async_result_t<void> async() &&` (유지)
- sync: `void submit() &&` (기존 blocking) + **`&& flags(int) &&` 단계 추가**
  → `.flags(DONTWAIT).submit()`. 난이도 낮음.

### Go
- sync: `Submit(ctx context.Context) error` (기존, Core 내부 blocking)
  + **builder에 flags 전달 경로 추가**. async 종결자는 없음(Go 관례상 정상). 난이도 낮음.

### Java
- async: `CompletionStage<Void> submit()` (유지 — Kotlin `await`/virtual thread `join()`용)
- sync: **`void submit(SendFlags flags)` 오버로드 추가**
  → 파라미터가 달라 overload 가능. 이름 유지. 실패 시 `ZlinkSubmitException`. 난이도 중간.

### .NET
- async: `Task Async(CancellationToken = default)` (유지)
- sync: **`void Submit(SendFlags flags)`** (관례: async=`Async`, sync=`Submit`). PAIR도 동일.
  난이도 중간.

### Rust
- async: `fn submit(self) -> impl Future<Output = Result<(), SubmitError>> + Send` (유지)
- sync: **`fn submit_blocking(self, flags: SendFlags) -> Result<(), SubmitError>`**
  → overload 없고 `async` 예약어라 이름 분리. `submit_blocking`. 난이도 중간.

### Node
- async: `submit(signal?): Promise<void>` (유지)
- sync: **`void submit(SendFlags flags)` 추가 (짝을 맞추되 사용 제약)**
  - `submit(DONTWAIT)` → 즉시 반환(즉시 backpressure). 이벤트 루프 안 막음 → 안전.
  - `submit(NONE)` = blocking → 이벤트 루프를 막으므로 **권장하지 않음(사용자 책임)**.
    recv의 `recv(DontWait)` 안전 / `recv(NONE)` 주의와 같은 성격.

### Python
- async: `submit() -> Awaitable[None]` (유지)
- sync: **`submit_blocking(*, flags=0) -> None` 추가**
  → GIL이 native 호출 중 풀리고 async가 기본이 아니라 thread+blocking이 자연스럽다. 난이도 중간.

## 현행 언어 spec과의 충돌 (구현 전 반드시 인지)

정규 표(0.14.0 목표)는 **현행 .NET/Java/Rust/Node README가 명시적으로 금지한 종결자를
신설**한다. 이는 gap-fill이 아니라 **의도된 설계 되돌림**이므로, 구현자는 현행 README를
권위로 오해하지 말고 아래를 근거로 갱신한다.

| 언어 | 현행 README 금지 서술 | 되돌림 내용 |
|---|---|---|
| .NET | `dotnet/README:342-349` — routed builder는 `Task Async()`뿐, **"blocking submit·`Flags(...)`·`Submit(...)`을 추가하지 않는다"**. base 역할에 동기 `SendOperation`이 없어 변환도 차단 | routed 빌더에 **자체 sync `Submit(SendFlags)` 종결자를 신설**(또는 공통 역할에 sync send terminal 추가). base 변환 차단 문장 삭제/수정 |
| Java | `java/README:674` — 비동기 builder에 **"blocking `await()`·`flags(...)`를 제공하지 않는다"** | 별도 `void submit(SendFlags)` **overload 신설**(async `submit()`과 시그니처 다름). "blocking terminal 없음" 문장을 "sync overload 있음"으로 갱신 |
| Rust | `rust/README:414` — builder에 **"blocking wait terminal을 두지 않는다"** | `submit_blocking(SendFlags)` 신설로 문장 갱신 |
| Node | `node/README:510,548` — Promise 종단만 | 제약부 sync `submit(SendFlags)` 신설(`NONE` blocking은 이벤트 루프 정지 → 권장 안 함) |

Kotlin framework는 되돌림과 무관하게 async `submit().await()`를 계속 쓴다(blocking 종결자를
코루틴에서 쓰지 않는다). C++/Go/Python은 blocking이 이미 있어 되돌림이 아니라 flags/노출 추가다.

## spec 작업 (구현과 함께)

- `async-coroutine-policy.ko.md`: send 계열(routed/PAIR/Received.send)에 "sync 종결자 + flags
  (기본 blocking, DONTWAIT nonblocking)"를 async 종결자와 **별개로 명문화**. 현재는 async 단일만 규정.
  reply는 동기·flags 없음(control 소켓, HWM 무관)을 명확히. recv는 동기+flags 단일 유지.
- `README.ko.md:2159`(SendOp flags)와 정합 확인.
- Kotlin 매핑: Java에 sync overload를 추가해도 **Kotlin framework는 기존 async `submit()`을
  `await`로 계속 쓴다**(blocking terminal을 코루틴에서 쓰지 않는다)를 명시.
- 언어별 spec(`bindings/doc/spec/<lang>/`)의 send terminal 서술 갱신.

## 작업 순서 제안

1. spec 확정(위 문서들) — public interface 계약이므로 먼저.
   - `async-coroutine-policy.ko.md` (한글 완료), `.en.md` (영문 반영 필요).
   - `README.ko/en.md:2159`(SendOp flags) 중복 정리 — 계약은 async-policy 소유, README는 링크.
   - 언어별 spec(`bindings/doc/spec/<lang>/README`)의 send terminal 서술 갱신.
2. 언어별 구현(병렬 가능, 언어 디렉터리 분리). C++/Go(flags만) → Java/.NET(overload) →
   Rust/Python(신규 sync) → Node(제약 있는 sync).
3. 각 언어 contract test로 sync+flags·async 두 경로 검증.
4. **bindings guide 문서 최신화 (구현 이후)** — spec 확정 계약에 맞춰
   `bindings/doc/guide/<lang>/index.{ko,en}.md`를 갱신한다.
   - 각 언어의 send terminal 사용법을 새 계약(async / sync+flags)에 맞게 갱신.
   - C API 대응표에 sync terminal 매핑 추가.
   - **blocking send 사용 주의(계약 아님, 실행 모델 권고)를 guide에 넣는다.**
     sync terminal의 flag 없는 기본 blocking은 HWM 대기 시 실행 단위를 멈춘다.
     plain thread·virtual thread(Java)에서는 그 스레드/vthread만 대기하므로 안전하나,
     이벤트 루프 모델 — Node(항상), Python(asyncio 루프 안) — 에서는 blocking send가
     루프 전체를 멈춰 런타임이 정지한다. 이 모델에서는 `DONTWAIT` 또는 async terminal을
     쓰도록 각 언어 index의 "스레딩 유의사항"에 안내한다.
   - 이 blocking 주의는 spec(계약 문서)에서 의도적으로 제외했다(계약이 아니라 사용 권고).
5. perf runner를 새 계약에 맞춰 정렬(multi backpressure를 async suspend 또는 sync DONTWAIT로).
6. 재측정.
