---
title: "Python 바인딩 가이드"
---

<!-- bindings-nav:start -->
[가이드 목록](../README.ko.md) | [이전: Node.js](../node/index.ko.md) | [다음: Go](../go/index.ko.md)
<!-- bindings-nav:end -->

# Python 바인딩 사용 안내

> **이 장의 계약 소유 문서** — [Python bindings 스펙](../../spec/python/README.ko.md)이
> 다룬다. 이 장은 그 계약을 실제 샘플 코드로 보여준다.

이 문서는 `zlink` Python package로 Core raw messaging을 사용하는 방법을 설명한다. Python 3.9
이상을 지원하며, 현재 Core 0.9.0 wheel의 native runtime target은 Linux x86_64이다. 다른 운영체제나
CPU architecture는 별도 Core 0.9.0 candidate와 clean consumer 검증이 끝날 때까지 지원 target으로
간주하지 않는다.

## 설치와 첫 송수신

```python
import zlink

with zlink.create_context() as ctx:
    with zlink.create_pair_socket(ctx) as sender:
        with zlink.create_pair_socket(ctx) as receiver:
            sender.bind("inproc://python-guide-pair")
            receiver.connect("inproc://python-guide-pair")

            sender.send().message(b"hello").submit_sync()
            received = zlink.create_received()  # 호출자가 수신 저장 공간을 소유한다.
            assert receiver.recv_into(received)
            with received:
                assert received.to_bytes_list() == [b"hello"]
```

`with` 블록은 context와 socket의 native resource를 해제한다. 독립 process 사이의 TCP 예제는
`bindings/python/samples/pair_recv_sample.py`에서 확인한다.

## Message와 Received

송신 builder에는 여러 part를 추가할 수 있다. `Message.from_(value)`는 caller 값에서 독립된 message를
만든다. 수신 결과는 `Received`가 보유하며, owner가 열린 동안에만 `parts`의 native view를 사용한다.
다른 task나 오래 유지할 값으로 넘기려면 아래처럼 명시적으로 복사한다.

```python
message = zlink.Message.from_(bytearray(b"payload"))  # caller buffer와 분리된 message를 만든다.
with message:
    socket.send().message(message).submit_sync()

received = zlink.create_received()
socket.recv_into(received)  # no-data를 허용하는 non-blocking 호출이면 False를 반환한다.
with received:
    parts = received.to_bytes_list()  # owner 밖으로 전달할 snapshot을 만든다.
```

`RecvFlags.DONT_WAIT`를 지정한 receive는 message가 없으면 `False`를 반환한다. timer나 monitor처럼
pending value를 직접 반환하는 control API는 값이 없을 때 `None`을 반환한다.

HWM 대기 가능 send는 async `submit()`과 sync
`submit_sync()`을 제공한다. async 코드에서는
`await socket.send().message(message).submit()`을 사용한다. 이 terminal은 DONTWAIT을
사용하고 socket completion queue에서 settle된다. Plain thread의 `submit_sync()`은 local
admission까지 Core 안에서 blocking한다.

Request는 reply까지 blocking하는 `submit_sync()`과 socket completion queue에서 settle되는
awaitable `list[Message]`를 반환하는 `submit()`을 제공한다. Reply는 terminal 결과이며 별도
DATA receive가 아니다.

Core가 pre-admission operation을 접수한 뒤 retry를 소유하므로 caller retry queue를 만들거나
payload를 재전송하지 않는다. 공용 native `ZLINK_OPT_PENDING_MAX_MSGS/BYTES` cap은 pending
SEND와 REQUEST에 함께 적용되고 send 전용 pending 이름은 없다. Completion은 local
admission일 뿐 peer delivery나 application acknowledgement가 아니다.

asyncio Task 취소는 Python waiter의 대기를 멈출 수 있다. Core submit 전에는 Core를 호출하지
않고 중단하지만, Core가 payload를 접수한 뒤에는 admission이나 request가 계속될 수 있고
socket owner가 늦은 completion을 drain한다. Bind/connect 전에
`stream.options.recv_mode`를 `zlink.StreamRecvMode.RAW` 또는 `.PACKET`으로 정한 뒤 각각
`recv_into` 또는 `recv_packet_into`를 사용한다.

public poller가 socket의 `zlink.PollEventFlag.POLLCOMPLETION` owner이면 blocking request나
awaitable이 남아 있는 동안 다른 thread가 `wait()` loop를 계속 실행해야 한다. `wait()`가
native completion을 drain해 Python state를 settle/cleanup하므로 같은 thread에서 wait 사이에
blocking terminal을 호출하면 진행이 멈출 수 있다.

## DEALER와 ROUTER

DEALER와 ROUTER는 raw `RoutingId`를 유지하며 ROUTER request에는 opaque `ReplyToken`도 있다.
Reply builder에는 `Received`에서 얻은 두 값을 함께 넘긴다.

```python
--8<-- "bindings/python/samples/request_reply_async_sample.py:doc"
```

completion-backed awaitable과 reply-token lifetime은 `request_reply_async_sample.py`를 참고한다.

## Routing ID와 오류

고정 길이 routing id는 `RoutingId.from_(bytes)`로 만든다. 빈 값과 Core 최대 길이를 넘는 값은 입력
검사에서 거부된다.

```python
rid = zlink.RoutingId.from_(b"server-01")
try:
    socket.send().message(b"data").submit_sync()
except zlink.SubmitError as exc:
    if exc.result == zlink.SubmitResult.BACKPRESSURED:
        # back-pressure는 결과를 확인한 뒤 application policy로 처리한다.
        pass
    else:
        raise
```

`SubmitError`, `RequestError`, `RecvError`, `BindError`, `ConnectError`, `ConfigError`와 `CloseError`는
`ZlinkError` 계열이며 `result`, `code`, `native_errno`를 제공한다.

## 스레딩 유의사항

`submit_sync()`은 HWM admission을 기다리는 동안 호출 thread를 멈춘다.
plain thread에서는 그 thread만 대기하므로 사용할 수 있다. 그러나 asyncio 이벤트 루프
안에서 호출하면 루프 전체가 멈춰 다른 task와 send completion도 진행되지 않는다.
asyncio 코드에서는 async `submit()`을 `await`한다.

## Sample와 perf

raw sample runner에는 다음 process가 포함된다.

- `pair_recv_sample.py`
- `dealer_router_recv_sample.py`
- `request_reply_async_sample.py`
- `pubsub_recv_sample.py`
- `stream_recv_sample.py`
- `stream_packet_recv_sample.py`
- `monitor_recv_sample.py`

Perf runner는 사용할 Core 또는 wheel runtime을 명시해야 한다. runner가 출력하는 path와 SHA-256을
candidate evidence와 비교한다.

```bash
ZLINK_LIBRARY_PATH=/absolute/path/to/libzlink.so \
  bindings/python/perf/run_benchmarks.sh --smoke --pattern PAIR \
  --duration 1 --msg-sizes 64 --transports inproc --runs 1
```

Smoke mode는 process lifecycle과 필수 `RESULT` row를 확인하며 공식 report를 만들지 않는다.
