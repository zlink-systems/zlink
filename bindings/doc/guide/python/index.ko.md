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
이상을 지원하며, 현재 Core 11 wheel의 native runtime target은 Linux x86_64이다. 다른 운영체제나
CPU architecture는 별도 Core 11 candidate와 clean consumer 검증이 끝날 때까지 지원 target으로
간주하지 않는다.

## 설치와 첫 송수신

```python
import zlink

with zlink.create_context() as ctx:
    with zlink.create_pair_socket(ctx) as sender:
        with zlink.create_pair_socket(ctx) as receiver:
            sender.bind("inproc://python-guide-pair")
            receiver.connect("inproc://python-guide-pair")

            sender.send().message(b"hello").submit()  # 한 part를 전송한다.
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
    socket.send().message(message).submit()  # submit 성공 뒤 native 소유권이 이동한다.

received = zlink.create_received()
socket.recv_into(received)  # no-data를 허용하는 non-blocking 호출이면 False를 반환한다.
with received:
    parts = received.to_bytes_list()  # owner 밖으로 전달할 snapshot을 만든다.
```

`RecvFlags.DONT_WAIT`를 지정한 receive는 message가 없으면 `False`를 반환한다. timer나 monitor처럼
pending value를 직접 반환하는 control API는 값이 없을 때 `None`을 반환한다.

## DEALER와 ROUTER

DEALER와 ROUTER는 raw `RoutingId`와 request sequence를 유지한다. ROUTER가 받은 `Received`에서
`routing_id`를 읽고, 같은 metadata를 reply builder가 사용한다.

```python
with zlink.create_context() as ctx:
    with zlink.create_dealer_socket(ctx) as dealer:
        with zlink.create_router_socket(ctx) as router:
            router.bind("inproc://python-guide-request")
            dealer.connect("inproc://python-guide-request")

            dealer.request().message(b"ping").submit(on_reply)  # reply callback을 등록한다.
            received = zlink.create_received()
            assert router.recv_into(received)
            with received:
                received.reply().message(b"pong").submit()  # 수신 routing metadata로 회신한다.
```

실제 callback lifetime과 timeout 처리는 `request_reply_callback_sample.py`를 참고한다.

## Routing ID와 오류

고정 길이 routing id는 `RoutingId.from_(bytes)`로 만든다. 빈 값과 Core 최대 길이를 넘는 값은 입력
검사에서 거부된다.

```python
rid = zlink.RoutingId.from_(b"server-01")
try:
    socket.send().message(b"data").submit()
except zlink.SubmitError as exc:
    if exc.result == zlink.SubmitResult.BACKPRESSURED:
        # back-pressure는 결과를 확인한 뒤 application policy로 처리한다.
        pass
    else:
        raise
```

`SubmitError`, `RequestError`, `RecvError`, `BindError`, `ConnectError`, `ConfigError`와 `CloseError`는
`ZlinkError` 계열이며 `result`, `code`, `native_errno`를 제공한다.

## Sample와 perf

raw sample runner에는 다음 process가 포함된다.

- `pair_recv_sample.py`
- `dealer_router_recv_sample.py`
- `request_reply_callback_sample.py`
- `pubsub_recv_sample.py`
- `stream_recv_sample.py`
- `stream_packet_callback_sample.py`
- `monitor_recv_sample.py`

Perf runner는 사용할 Core 또는 wheel runtime을 명시해야 한다. runner가 출력하는 path와 SHA-256을
candidate evidence와 비교한다.

```bash
ZLINK_LIBRARY_PATH=/absolute/path/to/libzlink.so \
  bindings/python/perf/run_benchmarks.sh --smoke --pattern PAIR \
  --duration 1 --msg-sizes 64 --transports inproc --runs 1
```

Smoke mode는 process lifecycle과 필수 `RESULT` row를 확인하며 공식 report를 만들지 않는다.
