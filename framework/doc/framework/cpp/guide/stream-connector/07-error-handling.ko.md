---
title: "오류 처리 · C++"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/stream-connector/07-error-handling.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 오류 처리

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: 연결 생명주기](06-lifecycle.ko.md) | [다음: E2E 클라이언트](08-e2e-client.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — **C++** · [C#/.NET](../../../dotnet/guide/stream-connector/07-error-handling.ko.md) · [Java](../../../java/guide/stream-connector/07-error-handling.ko.md) · [Kotlin](../../../kotlin/guide/stream-connector/07-error-handling.ko.md) · [Node/TypeScript](../../../node/guide/stream-connector/07-error-handling.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    실패한 호출에서 오류 코드를 읽고, 그 코드가 연결에 어떤 영향을 주는지 판단해
    재시도·복구·중단을 고를 수 있다.

connector의 오류 코드는 **닫힌 집합**이다. 구현이 코드를 더 만들거나 빼지 않으므로, 코드마다
처리를 정해 두면 새 코드가 나타나 분기를 빠뜨리는 일이 없다. 이 장은 그 코드를 받는 방법과
코드별 의미를 다룬다.

## 1. 오류를 받는 방법

전달 방식은 표면에 따라 다르되 의미는 같다. 완료를 기다리는 표면은 실패를 그 자리에서 전달하고,
callback을 받는 표면은 결과 객체로 전달하며, 어느 request에도 속하지 않는 오류는 오류 이벤트로
전달한다. **어느 방식이든 받는 쪽이 코드를 읽을 수 있다.**

```cpp
// 예외가 꺼진 빌드가 core를 그대로 사용하므로 실패는 값으로 돌아온다.
auto reply = connector.request (login_request_t{"player-1", "tok-abc123"})
               .submit<login_reply_t> ();

if (!reply) {
    if (reply.error_code () == sc::error_code_t::request_timeout) {
        retry_login ();
    }
}
```

**언어의 표준 예외를 그대로 던지지 않는다.** 인자 오류나 상태 오류를 나타내는 표준 타입에는
코드를 담을 자리가 없어, 호출자가 구성 오류인지 검증 실패인지 판정하지 못하기 때문이다. 오류를
예외로 전달하는 언어는 코드를 담는 전용 예외 타입을 사용하고, 예외를 끈 빌드는 결과 값으로
같은 코드를 전달한다.

## 2. request에 속하지 않는 오류

frame을 해석하지 못했거나 서버가 request와 무관하게 보낸 오류는 기다리는 호출이 없으므로 오류
이벤트로 전달한다. 이 handler도 해제할 수 있는 값을 돌려준다.

```cpp
auto errors = connector.on_error ([] (const sc::error_t &error) {
    log_error (error.code, error.message);
});
```

## 3. 오류 코드

| 코드 | 의미 |
|---|---|
| `disconnected` | 연결이 없거나 끊겼다 |
| `configuration_error` | 구성이 잘못됐다. endpoint scheme과 transport 충돌, 환경이 지원하지 않는 transport 등 |
| `ValidationFailed` | 전송 전 검증, option 값 범위 검증, 대기 표면의 관측 조건이 어긋났다 |
| `request_timeout` | 응답을 기다리다 시간이 초과됐다 |
| `ConnectTimeout` | 연결을 기다리다 시간이 초과됐다 |
| `FrameDecodeFailed` | frame이나 header를 해석하지 못했다 |
| `FrameTooLarge` | 받은 payload가 수신 한도를 넘었다 |
| `send_failed` | 전송에 실패했다 |
| `CompressionFailed` | 압축에 실패했다 |
| `DecompressionFailed` | 압축 해제에 실패했다 |
| `TlsValidationFailed` | TLS 검증에 실패했다 |
| `UserCallbackFailed` | 사용자 callback이 실패했다 |
| `RemoteError` | 서버가 오류 응답을 보냈다 |

**기다리다 시간이 초과된 두 코드는 뜻이 다르다.** 응답을 기다린 request의 초과는 요청이 답을
받지 못한 것이고, 대기 표면의 초과는 관측이 어긋난 것이다. 후자를 검증 실패로 전달하는 이유가
여기 있다 — 호출자가 두 상황을 다르게 처리해야 한다.

서버가 도메인 오류를 정상 응답으로 돌려주려면 오류 응답이 아니라 성공 응답과 자기 payload를
사용한다. 오류 응답의 payload는 codec 설정과 무관하게 항상 코드와 message를 담은 JSON이다.

## 4. 오류가 연결에 미치는 영향

같은 실패라도 그 호출만 실패하는 것과 연결이 끝나는 것은 대응이 다르다.

| 코드 | 현재 호출 | 연결 | 자동 재연결 |
|---|---|---|---|
| `configuration_error` · `ValidationFailed` | 실패 | 유지 | 하지 않는다 |
| `request_timeout` | 그 request만 실패 | 유지 | 하지 않는다 |
| `ConnectTimeout` · `TlsValidationFailed` | 연결 실패 | 끊김 | 시도 정책을 적용한다 |
| `disconnected` · `send_failed` | 진행 중인 호출 실패 | transport가 끊겼으면 끊김 | 켜져 있으면 적용한다 |
| `FrameDecodeFailed`(frame·header) · `FrameTooLarge` | 그 frame을 전달하지 않고 대기 중인 request를 실패시킴 | 종료 | 켜져 있으면 적용한다 |
| `CompressionFailed` | 그 송신만 실패 | 유지 | 하지 않는다 |
| `DecompressionFailed` | 그 수신 packet 또는 대기 중인 request만 실패 | 유지 | 하지 않는다 |
| `UserCallbackFailed` · `RemoteError` | 오류 이벤트나 관련 호출로 전달 | 유지 | 하지 않는다 |

연결이 끝나는 쪽은 종료 사유가 transport 오류로 남는다. 종료 사유를 읽는 방법은
[연결 생명주기](06-lifecycle.ko.md)가 다룬다.

## 5. 자주 만나는 처리

**연결 없음.** 송신이 연결 없음으로 실패하면 재연결이 진행 중이거나 이미 포기한 상태다. 연결
상태 handler를 등록해 두고 다시 연결된 뒤에 보낸다. 값이 오래되어 의미가 없어지는 packet은
다시 보내지 않는다.

**응답 시간 초과.** 그 request만 실패하고 연결은 유지되므로, 같은 request를 다시 보내도 된다.
다만 서버가 이미 처리한 뒤 응답만 늦어졌을 수 있으므로, 두 번 처리되면 안 되는 요청은 서버 쪽에서
같은 요청을 구분할 수 있게 만든다.

**수신 한도 초과.** 받은 payload가 수신 한도를 넘으면 그 frame을 전달하지 않고 연결이 끝난다.
서버가 더 큰 packet을 보낼 수 있는 구성이라면
[Connector 옵션](03-connector-options.ko.md)에서 수신 한도를 키운다.

**서버 오류 응답.** 서버가 오류 응답을 보내면 그 request가 실패하고, 어느 request에도 맞지 않으면
오류 이벤트로 전달한다. 연결은 그대로 유지되므로 다른 packet은 영향을 받지 않는다.

## 6. 런타임에 따라 나지 않는 코드

브라우저 런타임에서는 TLS 검증 실패가 발생하지 않는다. 브라우저의 WebSocket API가 TLS 실패를
일반 연결 실패와 구분해 주지 않기 때문이다. 코드는 집합에 그대로 남고 그 런타임에서 쓰이지 않을
뿐이므로, 코드별 처리를 적어 둔 분기는 런타임마다 달라지지 않는다.

## 7. 관련 장

- 연결이 끝난 이유 확인 — [연결 생명주기](06-lifecycle.ko.md)
- 한도와 검증 시점 — [Connector 옵션](03-connector-options.ko.md)
- 송신 실패가 나는 자리 — [packet 송신](04-sending.ko.md)
