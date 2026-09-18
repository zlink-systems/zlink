---
title: "연결 생명주기 · C#/.NET"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/stream-connector/06-lifecycle.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 연결 생명주기

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: packet 수신](05-receiving.ko.md) | [다음: 오류 처리](07-error-handling.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/stream-connector/06-lifecycle.ko.md) · **C#/.NET** · [Java](../../../java/guide/stream-connector/06-lifecycle.ko.md) · [Kotlin](../../../kotlin/guide/stream-connector/06-lifecycle.ko.md) · [Node/TypeScript](../../../node/guide/stream-connector/06-lifecycle.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    연결 상태를 읽고 상태 변화를 handler로 받으며, 자동 재연결이 어디까지 해 주는지 알고
    연결이 왜 끝났는지 확인할 수 있다.

connector 하나는 연결 하나를 대표한다. 연결은 만들고, 연결하고, 끊기고, 다시 연결하고, 닫는
순서를 지나며, 닫은 connector로는 다시 연결하지 않는다. 이 장은 그 과정에서 application이
확인하고 처리해야 하는 것을 다룬다.

## 1. 연결 상태

| 상태 | 의미 |
|---|---|
| 만들어짐 | connector를 만들었지만 아직 연결을 시작하지 않았다 |
| 연결 중 | 초기 연결을 진행하고 있다 |
| 연결됨 | 연결을 완료했다. packet을 주고받을 수 있다 |
| 재연결 중 | 자동 재연결을 진행하고 있다 |
| 끊김 | transport 연결이 끊겼다 |
| 닫힘 | connector를 닫았다. 같은 connector로 다시 연결하지 않는다 |

**만들어짐과 끊김은 서로 다른 상태다.** 한 번도 연결한 적이 없는 것과 연결했다가 끊긴 것을
구분해야 재연결 판단이 달라지기 때문이다.

## 2. 연결과 종료

연결 호출은 연결과 receive 준비가 끝나야 완료된다. 종료 호출은 보내지 못한 frame을 배출하고
transport를 닫은 뒤, 대기 중이던 request를 실패로 정리한다.

```csharp
await connector.Connect.Async();

await connector.Close.Async();
```

연결 요청은 현재 상태에 따라 다르게 동작한다.

| 현재 상태 | 동작 |
|---|---|
| 만들어짐 | 초기 연결을 시작한다 |
| 끊김 | 수동 재연결을 시작한다 |
| 연결 중 | 진행 중인 연결 시도가 끝날 때까지 기다린다 |
| 연결됨 | 이미 연결되어 있으므로 성공으로 즉시 완료된다 |
| 재연결 중 | 진행 중인 자동 재연결의 결과를 기다린다 |
| 닫힘 | 오류로 실패한다 |

## 3. 자동 재연결

자동 재연결은 기본으로 켜져 있다. 연결이 끊기면 상태가 재연결 중으로 바뀌고, 설정한 지연과
시도 횟수만큼 다시 연결한다. 지연과 무제한 지정은
[Connector 옵션](03-connector-options.ko.md)이 다룬다.

재연결이 진행 중인 동안의 송신은 큐에 저장되지 않고 **연결 없음으로 실패한다.** 보관해 두었다가
연결된 뒤 보내는 것은 application의 판단이다 — connector는 언제 보낸 값이 아직 유효한지 알지
못한다.

**연결이 끊기면 대기 중이던 request는 모두 실패한다.** 재연결에 성공해도 자동으로 다시 보내지
않는다. 같은 request를 다시 보낼지는 그 request의 의미에 달렸다.

## 4. 상태 변화와 끊김 받기

연결 상태 handler는 상태가 바뀔 때마다 실행된다. 끊김 handler는 **transport가 끊긴 시점에 한 번,
재연결 시도를 다 쓴 시점에 한 번** 실행된다. 앞의 것은 지금 연결이 없다는 사실을, 뒤의 것은
되살리기를 포기했다는 사실을 알린다. 재연결에 성공하면 뒤의 실행은 일어나지 않는다.

```csharp
using var disconnected = connector.OnDisconnected((closed, cancellationToken) =>
{
    ShowReconnecting(closed.CloseReason);
    return ValueTask.CompletedTask;
});

using var changed = connector.OnConnectionStateChanged((change, cancellationToken) =>
{
    if (change.Current == ZlinkStreamConnectionState.Connected) Resubscribe();
    return ValueTask.CompletedTask;
});
```

**끊김 handler가 사유를 인자로 받는지는 언어가 정한다.** 인자로 받지 않는 언어에서는 읽기 표면
하나로 사유를 확인한다. 어느 쪽이든 사유에 닿는 길이 있다.

연결이 다시 성립하면 수신 개수가 0으로 돌아가고 이전 연결의 미소비 message도 비워진다. 연결된
직후에 다시 보내야 하는 구독이 있다면 상태 handler에서 처리한다.

## 5. heartbeat

heartbeat가 켜져 있으면 connector는 지정 주기마다 control packet을 보낸다. 지정 timeout 동안
들어오는 frame이 하나도 없으면 transport가 끊긴 것으로 처리하고 재연결 정책을 적용한다. 이
control packet은 handler로 전달되지 않으며 수신 큐에도 들어가지 않는다.

heartbeat를 꺼도 서버가 보낸 ping에는 응답한다.

## 6. 종료 사유

연결이 끝나면 그 사유가 남는다. 값은 아래로 닫혀 있다.

| 사유 | 의미 |
|---|---|
| client가 닫음 | client가 연결을 닫았다 |
| 유휴 timeout | 서버가 유휴 세션을 닫았다 |
| heartbeat timeout | heartbeat가 응답하지 않아 끊겼다 |
| 서버 배출 | 서버가 우아한 종료로 세션을 닫았다 |
| 프로토콜 오류 | 프로토콜 위반으로 끊겼다 |
| transport 오류 | transport 수준 실패로 끊겼다 |

서버가 우아한 종료로 닫은 경우 client는 이 값을 보고 재접속 시점과 backoff를 결정한다. 서버가
대체 endpoint를 알려주지는 않으므로, 어디로 다시 붙을지는 client의 구성이 정한다.

```csharp
var reason = connector.CloseReason;   // 끊긴 적이 없으면 null이다
```

**사유는 언제든 읽을 수 있다.** 끊김 handler를 등록하지 않은 code도 연결이 닫힌 뒤 같은 값을
읽는다. 첫 연결이 실패한 경우에도 사유가 남고, 다시 연결해도 값을 지우지 않고 마지막 종료의
사유를 유지한다.

## 7. 종료가 기다리는 것

종료 호출은 **connector 자신의 일만** 기다린다. 보내지 못한 frame의 배출, transport 종료, 대기
중인 request의 실패 처리가 끝나면 돌아온다.

**connector는 handler의 완료를 기다리지 않는다.** 끊김 handler를 실행한 뒤 그 handler가 끝났는지는
보지 않고 돌아오며, 재연결도 같다. 그래서 끝나지 않는 handler 하나가 종료를 막지 못한다. handler
안에서 반드시 끝내야 하는 일이 있으면 그 일을 handler 밖에서 기다린다.

## 8. 다음 장

- 끊김과 함께 전달되는 오류 코드 — [오류 처리](07-error-handling.ko.md)
- 재연결 지연과 heartbeat 설정 — [Connector 옵션](03-connector-options.ko.md)
