# C++ Stream Connector Async Runtime Guide

이 문서는 core connector를 사용하는 client가 callback과 `dispatch()`를 어떻게 다뤄야 하는지
설명한다. 내부 socket 구조나 Boost.Asio 세부 구현은 공개 사용법이 아니므로 설명하지 않는다.

## Dispatch Mode

`manual` mode에서는 user callback이 `dispatch()`를 호출한 thread에서 실행된다. Unreal, Godot,
Axmol 같은 engine wrapper는 이 mode를 사용해 engine main thread에서 delegate나 signal을 실행한다.

`immediate` mode에서는 completion을 만든 runtime worker가 user callback을 실행할 수 있다. CLI,
tool, perf client처럼 engine main thread 제약이 없는 code에서 사용한다.

## Callback Completion

`request().submit(callback)`, `wait_for().submit(callback)`, `connect(callback)`,
`close(callback)`은 operation을 등록하고 즉시 반환한다. 동기 `request().submit()`,
`wait_for().submit()`, `connect()`, `close()`는 기존 사용자를 위해 유지한다.

TCP, TLS, WebSocket, WSS transport에서 `connect(callback)`은 연결 완료나 handshake를 기다리기
위해 호출 thread를 막지 않는다. TLS와 WSS는 OpenSSL feature가 켜진 build에서 사용할 수 있다.

`send().submit()`은 one-way 전송 요청을 제출하고 완료 결과를 호출자에게 반환하지 않는다.
송신 수락과 backpressure 처리는 connector 내부 책임이다.

`request().submit(callback)`은 request frame write를 등록한 뒤 반환한다. reply, timeout, close,
transport 오류 중 하나가 발생하면 callback에 `result_t<T>`로 전달된다.
reply를 기다리는 동안 호출 thread나 worker thread를 blocking wait에 묶어 두지 않는다.

`wait_for().submit(callback)`도 matching packet, timeout, close 중 하나가 발생할 때까지 callback
completion으로 대기한다. 기다리는 동안 호출 thread를 점유하지 않는다.

callback 안에서 다시 connector API를 호출할 수 있어야 한다. 따라서 implementation은 connector
내부 lock을 잡은 상태로 user callback을 호출하지 않는다.

connector가 닫히면 아직 끝나지 않은 callback operation은 성공으로 남으면 안 된다. 사용자가
`close()` 또는 `close(callback)`을 호출해 종료한 경우에는 `closed` 오류로 완료한다. 사용자가
coroutine task나 명시 취소 토큰으로 operation을 취소한 경우에는 `canceled` 오류를 사용한다.
transport가 끊긴 경우는 기존처럼 `disconnected`로 구분한다.

## Engine Rule

engine wrapper는 public API에서 core type을 노출하지 않는다. core callback은 adapter queue에 넣고,
engine Tick, Update, 또는 명시 `Dispatch()`에서 engine delegate로 바꿔 실행한다.
