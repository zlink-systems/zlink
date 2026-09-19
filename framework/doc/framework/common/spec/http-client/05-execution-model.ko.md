# 5. 실행 모델

> [공통 계약 목차](README.ko.md)
>
> Terminator와 Spot 실행 문맥 결합은
> [12 HTTP client §3](12-http-client.ko.md)과
> [04 비동기 실행 정책 §1.1](../server/01-execution/README.ko.md)이 소유한다. 아래
> §5.1은 호출 형태를 이해하기 위한 요약이다. 이 문서는 §5.2부터 HTTP 전송의
> non-blocking 근거, cancellation과 timeout 경계를 정의한다.

## 5.1 실행 방식과 callback

HTTP 종결자 표면은 [언어별 인터페이스 §1.4](language-interfaces.ko.md#14-종결자-terminator)를 따르고,
실행 방식별 완료 의미(response completion·`Yield`)는 [12 §3](12-http-client.ko.md#3-실행-terminator--response-completion--callback)이
소유한다. `Yield`의 적격 문맥과 gate 반납 의미는 framework
[Submit과 완료 §2](../server/01-execution/01-submit-and-completion.ko.md#2-terminator별-완료-의미와-언어별-이름)가
정하며, HTTP에서 사용하는 두 형태는 [§5.2](#52-외부-http-대기와-spot-실행-줄)가 설명한다.

**Callback은 별도 완료 경로다.** Awaitable을 사용하지 않는 호출자가 사용하며, 완료 callback은
Spot 실행 줄의 **새 turn**으로 들어간다
([framework 12 §3](12-http-client.ko.md)).

제출은 언어의 표준 비동기 값을 돌려주며, 네트워크 대기 중 호출자의 스레드/event loop를 점유하지
않는다.

| 언어 | 비동기 반환형 | non-blocking 근거 |
| --- | --- | --- |
| cpp | `task_t<T>` (`co_await`) | execute scheduler(기본 또는 `.coroutines(...)`로 주입)로 오프로드 |
| dotnet | `ValueTask<T>` | `SocketsHttpHandler` epoll/IOCP |
| java | `CompletionStage<T>` | `java.net.http` NIO selector |
| kotlin | `suspend` 함수 | java 런타임 + `CompletionStage.await()` 브리지 |
| node | `Promise<T>` | undici libuv |

## 5.2 외부 HTTP 대기와 Spot 실행 줄

외부 API를 기다리는 동안 같은 Spot의 다른 작업과 timer가 진행되어야 하면 gate를 반납해야 한다.
서버 HTTP request builder는 그 문맥에서 `Yield`를 직접 제공한다.

```csharp
var profile = await http.Get($"/players/{id}").Yield<Profile>(ct);
```

응답을 기다리는 동안 다른 작업도 함께 수행해야 하면 I/O Worker로 감싸고 Worker call의 `Yield`로
끝낸다. 아래 예제가 그 형태다.

```csharp
var profile = await Context
    .RunIoWorker(async workerCancellation =>
        await http.Get($"/players/{id}").Fetch<Profile>(workerCancellation))
    .Yield(ct);
```

- **Worker `Yield` 앞뒤로 Spot 상태가 그대로라고 가정하지 않는다.** 대기 중 다른 callback이 상태를
  바꿀 수 있다.
- **Response completion terminator는 turn을 유지한다.** Spot 상태를 비동기 대기 전후에
  이어서 다뤄야 하면 이 terminator를 사용한다.

## 5.3 turn seam — execution scheduler 주입

HTTP client의 Framework 계약 의존과 산출물 경계는 [01 범위와 아키텍처 §1.3](01-scope-and-architecture.ko.md#13-framework와의-관계--단방향-의존)가 소유한다.
Spot turn 연결과 completion scheduler 주입은
[HTTP client §3.2](12-http-client.ko.md#32-turn-seam--주입점-하나)를 따른다.

- HTTP client는 **execution scheduler 주입점**을 공개 계약으로 둔다. scheduler가 completion을
  어디서 재개할지 정한다.
- **Framework는 DI 등록 시 callback completion scheduler를 주입한다.** Callback은 Spot 실행 줄의 새
  turn으로 들어간다.

cpp의 `coroutines(resume_scheduler)` / `framework_resume_scheduler_t`가 이 seam의 선례다.

- `coroutines()` — 기본 스케줄러 사용.
- `coroutines(resume_scheduler)` — 재개 위치 주입(framework 실행 줄에서 continuation 재개).
- `coroutines(execute_scheduler, resume_scheduler)` — 실행/재개 모두 주입.

주의(현행 구현 특성, 계약 아님): cpp 기본 스케줄러는 단일 스레드를 execute/resume 공용으로 쓰므로
요청이 직렬화되며, 재개된 continuation에서 같은 스케줄러의 다른 task를 blocking 대기하면 데드락이
가능하다.

## 5.4 runtime 실행 문맥은 blocking terminator 호출을 거부한다

`Fetch` 계열은 decoded body를 직접 반환하지만 다섯 언어 모두 비동기로 완료된다. 이름이
`Fetch`라는 이름은 현재 thread를 점유하는 대기를 허용하지 않는다.

- 금지 대상: `.result()`, `.join()`, `.get()`처럼 비동기 결과를 현재 thread에서 기다리는 API.
- 테스트나 CLI에서 동기로 기다려야 하면 **호출자가** 언어 관용으로 감싼다
  (`GetAwaiter().GetResult()`, `runBlocking`, `.join()`).
- 합성은 `co_await` / `await` / `thenCompose` / suspend로 한다.

C++ blocking 종결자(이름은 [언어별 인터페이스 §1.4](language-interfaces.ko.md#14-종결자-terminator))는 CLI·client
시나리오 전용이다. runtime 실행 문맥에서는
[Submit과 완료 §2](../server/01-execution/01-submit-and-completion.ko.md#2-terminator별-완료-의미와-언어별-이름)에 따라
`InvalidOperation`으로 즉시 실패한다.

## 5.5 서버 표면과 client 수명

**서버(Spot handler·channel handler)에서 사용하는 client는 DI로 주입받는다.** handler 안에서 정적
팩토리로 client를 만들지 않는다 — 연결 pool과 turn seam을 잃는다.

| 표면 | 누가 쓰나 | terminator |
|------|-----------|------------|
| 정적 팩토리 | CLI · client 시나리오 | response completion / callback |
| **DI 주입 client** | **Spot handler · 서버 코드** | response completion / callback / `Yield` |

- client는 서비스당 하나를 만들어 재사용한다(pool/keep-alive 이득).
- builder verb 단축(one-shot)은 제출 시 client를 lazy build하고 완료 후 닫는 **편의 경로**다.
  요청마다 전송 스택 초기화 비용을 내므로 반복/고부하 호출에 사용하지 않는다. one-shot 요청 객체는
  재제출할 수 없다(재제출 시 `InvalidOperation`).

## 5.6 취소

- dotnet은 제출에 `CancellationToken`을 받는다.
- kotlin coroutine 취소의 하부 요청 전파는 현재 미구현이다(개정 후보
  [R5](10-revision-candidates.ko.md)).
- cpp/java/node는 요청 단위 취소 API를 노출하지 않는다(timeout으로 경계).
