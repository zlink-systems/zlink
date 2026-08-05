# HTTP Client — 공통 스펙

[스펙 목차](../README.ko.md) | [이전: Channel 메시징](../08-channel-messaging.ko.md) | [다음: SPOT 메시징](../12-spot-messaging.ko.md)

> 이 문서는 HTTP client를 Framework에서 등록하고 호출하는 경계를 정의한다. 정체성,
> fluent builder 형식, 실행 terminator, Spot 실행 문맥과의 결합, codec과 공통 오류
> model을 소유한다.
>
> 상세 계약(builder, 응답, redirect·retry·cookie, 인증·TLS·proxy, 압축, 오류 매핑, 회귀)은
> 같은 폴더의 [01~11](README.ko.md)이 각각 소유한다.
>
> 언어별 정확한 타입과 signature는 [`languages/<lang>/`](README.ko.md)이 소유한다.
> [language-interfaces](language-interfaces.ko.md)는 다섯 언어를 나란히 놓고 보는
> **비규범 대조표**다 — 계약을 고정하지 않는다.

## 1. 정체성 — framework 동반 client

**HTTP client는 STREAM connector와 같은 자리에 있다.** 별도 패키지로 배포하지만 **framework
전용 동반 client**이며, 계약은 framework가 소유한다.

| | [STREAM connector](../01-glossary.ko.md#stream-connector) | HTTP client |
|---|---|---|
| 패키지 | 별도 | 별도 |
| 계약 소유 | framework 공통 스펙([32](../stream-connector/32-stream-connector.ko.md)) | framework 공통 스펙(이 문서) |
| 언어별 인터페이스 | `stream-connector/languages/<lang>/` | `http-client/languages/<lang>/` |

**존재 이유는 하나다** — framework application이 **외부 API와 레거시 API를 zlink 스타일로**
호출할 수 있어야 한다. 범용 HTTP 라이브러리를 대체하려는 것이 아니다.

**의존은 한 방향이다: HTTP client → framework 계약.** HTTP client는 framework의 **오류 kind**와
**codec extension**을 소비하고(§5, §6), 자체 예외 계층을 만들지 않는다.
**framework는 HTTP client에 의존하지 않는다** — HTTP client 없이도 동작한다.

**소비하는 것은 framework의 계약이지 런타임이 아니다.** 그래야 CLI와 client 시나리오가 framework
host·runtime을 끌고 오지 않는다. [Spot](../01-glossary.ko.md#spot) 실행 문맥과의 결합은 §3.2의 **주입점 하나**로만 이뤄진다 —
scheduler가 주입되지 않으면 HTTP client는 turn을 모르는 평범한 client다.

산출물 경계와 언어별 패키지 분할은
[01 범위와 아키텍처 §1.3](01-scope-and-architecture.ko.md)이 소유한다.

## 2. Fluent builder

**framework messaging과 같은 형식이다** — "operation 선택 → 설정 → 실행 방식 terminator".

```
client.post("/games")               // operation
      .header("x-request-id", ...)  // 설정
      .query("region", "kr")
      .body(createGameReq)
      .timeout(3s)
      .submit<CreateGameRes>()      // C++·Java의 response completion terminator
                                    // Node는 async<CreateGameRes>() 사용
```

- verb 7종: `get` / `post` / `put` / `delete` / `patch` / `head` / `options`.
- 설정 축: `header`, `query`, `timeout`, body 소스(typed / raw / streaming / form / multipart).
- **body 소스는 상호 배타다.** 섞으면 `ProtocolError`.

builder의 세부 계약(path 형식, percent-encoding, body 소스별 retry 가능 여부 등)은
[03 Request builder](03-request-builder.ko.md)가 소유한다.

## 3. 실행 terminator — one-way와 response completion (+ callback)

HTTP client의 완료 표면은 one-way submission, response completion과 callback이다. 정확한 이름은
.NET `Async`, Kotlin wrapper `await`, Java·C++ `submit`이다. Node는 raw response에 `submitRaw`,
typed response와 callback에 `async`, one-way에 `submit`을 사용한다. TypeScript 상속 signature
제약은 언어별 exact interface가 소유한다. Shared Spot gate를 반납하는
`Yield`는 서버 request와 Worker call에만 제공하며 HTTP request builder에는 포함하지 않는다
([04 §1.1](../05-async-execution-policy.ko.md)).

| 실행 방식 | 무엇을 기다리나 | Spot 실행 줄 |
|---|---|---|
| **one-way submission** | HTTP 요청이 전송 경계에 제출될 때까지 기다린다 | 현재 turn을 유지한다. 정상 완료 값은 없다 |
| **response completion** | HTTP response가 도착할 때까지 기다린다 | 현재 turn을 유지한다 |

**Callback은 awaitable을 쓰지 않는 호출자**(CLI,
이벤트 루프 기반 client)를 위한 **별도 완료 경로**다. HTTP client는 그 경로도 함께 제공한다.

Spot 실행 문맥에서 callback을 쓰면 호출은 기다리지 않고 그대로 진행하며, 완료 callback은 그 Spot
실행 줄의 **새 turn**으로 큐에 들어간다. 완료 값으로 같은 turn의 판단을 이어가야 하면 callback 대신
언어별 response completion terminator를 사용한다.

### 3.1 외부 HTTP를 기다리면서 Spot gate를 반납하는 방법

HTTP client call 자체는 shared Spot gate를 반납하지 않는다. Actor 입·퇴장 중 외부 API를 기다리면서
다른 Spot 작업을 진행해야 하면 I/O Worker에서 HTTP client의 response completion terminator를
실행하고 Worker call의 `Yield`로 기다린다.

```csharp
var profile = await Context
    .RunIoWorker(async workerCancellation =>
        await http.Get($"/players/{id}").Fetch<Profile>(workerCancellation))
    .Yield(ct);
```

HTTP request builder에는 `Yield` terminal을 제공하지 않는다. Gate 반납과 재획득은 서버 runtime의
Worker call이 소유하므로 HTTP package가 Spot execution context를 판정하지 않는다.

### 3.2 turn seam — 주입점 하나

**HTTP client는 framework의 오류 kind와 codec은 알지만, Spot의 turn은 모른다.** turn을 아는 것은
**주입된 execution scheduler** 하나뿐이다.

- HTTP client는 **execution scheduler 주입점**을 공개 계약으로 둔다. scheduler는 completion을
  어디서 재개할지 정한다.
- **Framework는 DI 등록 시 callback completion scheduler를 주입한다.** Callback은 원래 Spot 실행 줄의
  새 turn으로 들어간다.
- DI와 단독 사용 모두 HTTP request builder에 `Yield`를 노출하지 않는다. 언어별 response
  completion terminator와 callback만 사용한다.

C++ HTTP client는 같은 scheduler seam을 `coroutines(resume_scheduler)`와
`framework_resume_scheduler_t`로 표현한다.

### 3.3 blocking terminator를 두지 않는다

**완료 값을 동기로 언래핑하는 public terminator를 만들지 않는다**([04 §2](../05-async-execution-policy.ko.md)).
같은 의미의 blocking 대안 terminator는 계약 위반이다. 테스트나 CLI에서 동기로 기다려야 하면
호출자가 언어 관용(`GetAwaiter().GetResult()`, `runBlocking`, `.join()`)으로 직접 감싼다.

## 4. 서버 표면과 등록

**서버(Spot handler·channel handler)에서 쓰는 HTTP client는 DI로 주입받는다.** 정적 팩토리로
handler 안에서 client를 만들지 않는다 — 연결 pool과 turn seam을 잃는다.

- **application이 명명 등록한다.** baseUrl, 인증, timeout, retry 정책은 서비스마다 다르므로
  framework가 기본 client 하나를 자동 등록하지 않는다.
- 등록 표면의 형태는 channel 등록과 같다: 구성 단계에서 이름과 정책을 함께 등록하고, handler는
  그 이름으로 주입받는다.
- **정적 팩토리 진입점은 client-side 전용으로 남긴다.** CLI와 client 시나리오가 쓴다.

| 표면 | 누가 쓰나 | terminator |
|------|-----------|------------|
| 정적 팩토리 | CLI · client 시나리오 | response completion / callback |
| **DI 주입 client** | **Spot handler · 서버 코드** | one-way / response completion / callback |

## 5. Codec

**HTTP client는 framework와 codec extension을 공유하지만 registry 인스턴스는 따로 가진다**
([Stream Session §5](../19-stream-session.ko.md#5-codec-계층-분리)). 같은 codec extension 객체를 양쪽에 각각 등록할 수 있으나,
**등록은 host마다 따로 해야 한다.**

typed body의 encode/decode는 그 registry가 담당한다. raw body API는 registry를 거치지 않는다.

## 6. 오류 모델

**HTTP client는 자체 예외 계층을 만들지 않는다.** framework 공용 오류 모델
([Framework 오류 모델](../32-framework-error-model.ko.md))의 error kind를 그대로 쓴다. **HTTP client
전용 error kind를 새로 만들지 않는다.**

| 상황 | kind |
|------|------|
| 구성·사용 오류, typed decode, 압축 해제 또는 redirect 형식 오류 | `ProtocolError` |
| Network, DNS, proxy와 target 연결 실패 | `Unavailable` |
| 설정한 response body byte 제한 초과 | `CapacityExceeded` |
| 시도당 timeout 초과 | `DeadlineExceeded` |
| HTTP status가 400 이상이거나 분류할 수 없는 실행 실패 | `InternalFailure` |

세부 매핑은 [09 오류 모델](09-error-model.ko.md)이 소유한다.

## 7. 회귀 테스트

| 항목 | 검증 |
|---|---|
| terminator 축 | one-way와 response completion 및 callback 완료 경로가 있고, blocking 언래핑 terminator와 HTTP `Yield`가 **없다** |
| turn 유지 | Spot handler가 response completion을 기다리는 동안 같은 Spot의 다른 callback이 시작하지 않는다 |
| turn 반납 | HTTP response completion을 `RunIoWorker` 안에서 실행하고 Worker `Yield`로 기다릴 때만 shared Spot gate를 반납한다 |
| 표면 제한 | DI와 단독 사용 모두 HTTP request builder에 `Yield`를 노출하지 않는다 |
| 등록 | 서버 표면이 DI 주입으로만 얻어지고, handler 안에서 정적 팩토리로 client를 만들지 않는다 |
| 오류 kind | HTTP client 전용 kind가 없고 framework 공용 kind만 쓴다 |
| builder | body 소스를 섞으면 `ProtocolError`로 실패한다 |
