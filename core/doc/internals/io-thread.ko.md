---
title: "I/O Thread 내부 구조"
---

[English](io-thread.en.md) | 한국어

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: Thread safety](thread-safety.ko.md) | [다음: Design decisions](design-decisions.ko.md)
<!-- zlink-nav:end -->

# I/O Thread 내부 구조

> **이 장이 답하는 것** — I/O thread가 무엇을 하고, 어떻게 생성되며, 작업이 어떻게
> 분배되는가. 고수준 thread 종류는 [Threading model](threading-model.ko.md)이 다룬다.

이 문서는 zlink context 내부에서 I/O 스레드가 어떤 일을 하는지,
어떻게 생성되고, 작업이 어떻게 분배되는지 설명한다.

고수준 스레딩 모델(application thread, reaper thread, 스레드 간 통신)은
[Threading Model](threading-model.ko.md)을 참고.

## 1. 개요

각 I/O 스레드는 전용 **비동기 이벤트 루프**를 실행하며 다음을 수행한다.

1. 등록된 소켓의 읽기/쓰기 준비 상태를 폴링
2. mailbox(스레드 간 명령 전달 채널)를 통해 수신된 명령 처리
3. 타이머 실행

I/O 스레드는 zlink 네트워킹의 핵심이다. 실제 네트워크 송수신, 프로토콜
인코딩/디코딩, 연결 관리가 모두 I/O 스레드에서 일어난다.

## 2. 생성과 수명

I/O 스레드는 지연(lazy) 생성된다 — `zlink_ctx_new()`는 context를 할당하지만
첫 번째 소켓이 생성될 때까지 스레드를 실행하지 않는다.

```c
void *ctx = zlink_ctx_new();
zlink_ctx_set(ctx, ZLINK_IO_THREADS, 4);  /* must be set before first socket */

void *socket = zlink_socket(ctx, ZLINK_SOCKET_DEALER);  /* triggers thread launch */
```

내부적으로 `ctx_runtime_resources.cpp:start_io_threads_locked()`가
`io_thread_count`개의 `io_thread_t` 인스턴스를 생성한다. 각 스레드는
고유 slot ID를 받고, mailbox가 context의 slot registry에 등록되어
명령 라우팅에 쓰인다.

스레드 이름은 `IO/0`, `IO/1`, ... `IO/N-1` 패턴을 따른다.

## 3. 이벤트 루프

각 I/O 스레드는 Boost ASIO 기반 poller(`asio_poller.cpp`)를 소유한다.
`poller_t::loop()`의 메인 루프는 다음 사이클을 반복한다:

```
┌─────────────────────────────────────────────┐
│                Event Loop                   │
│                                             │
│  1. 만료된 타이머 실행                      │
│  2. io_context.poll()  — non-blocking       │
│     준비된 I/O 이벤트 일괄 처리             │
│  3. 준비된 이벤트가 없으면:                 │
│     io_context.run_for(≤100ms) — blocking   │
│  4. 폐기된 poll entry 정리                  │
│                                             │
│  ← 반복 ───────────────────────────────────→│
└─────────────────────────────────────────────┘
```

- **2단계**: non-blocking `poll()`로 준비된 이벤트를 한 번에 배치 처리해
  throughput을 높인다.
- **3단계**: 대기 중인 이벤트가 없으면 최대 100ms 블로킹해 busy-wait
  CPU 소비를 방지한다.

## 4. 소켓 I/O 처리

네트워크 I/O는 Proactor 모델로 처리한다. engine(`asio_engine_t`)이 transport에
`async_read_some()` / `async_write_some()`을 요청하면 I/O 스레드의 `io_context`가
OS 비동기 I/O 완료를 기다렸다가 completion 콜백을 부른다. engine은 읽기/쓰기 준비
상태를 직접 폴링하지 않고 완료 결과만 처리한다.

- **Read 완료** → 읽은 바이트를 프로토콜 디코더에 넘겨 프레임을 디코딩한 뒤
  receive pipe로 메시지를 전달하고, 다시 `async_read_some()`을 건다.
- **Write 완료** → send pipe에서 꺼낸 메시지를 인코딩해 보낸 뒤, 남은 데이터가
  있으면 다음 `async_write_some()`을 건다.

`asio_poller`의 `async_wait` readiness 경로는 네트워크 데이터가 아니라 mailbox
명령 wakeup에만 쓴다.

## 5. 명령(Command) 처리

각 I/O 스레드는 **mailbox**를 가진다 — command pipe
(`ypipe_t<command_t>`, 전송 측은 mutex로 보호) 와 깨우기 신호용 signaler 의 조합이다.

```cpp
// io_thread.cpp — process_mailbox()
do {
    command_t cmd;
    int rc = _mailbox.recv(&cmd, 0);
    while (rc == 0 || errno == EINTR) {      // EINTR 재시도
        if (rc == 0)
            cmd.destination->process_command(cmd);
        rc = _mailbox.recv(&cmd, 0);
    }
} while (_mailbox.reschedule_if_needed());    // 남은 명령 있으면 재예약
```

명령은 application 스레드에서 `ctx_t::send_command()` 로 도착하며,
다음과 같은 종류가 있다:

| Command | 용도 |
|---------|------|
| `plug` | 새 session/engine을 이 I/O 스레드에 부착 |
| `attach` | engine을 session에 부착 |
| `bind` | session과 socket 사이 pipe 수립 |
| `activate_read` | pipe 읽기 재개 |
| `activate_write` | pipe 쓰기 재개 |
| `stop` | I/O 스레드 종료 |

mailbox는 I/O 스레드의 `io_context`에 연결되어(`set_io_context()`), 명령을 send하면
ASIO handler가 post되어 블로킹 대기 중인 이벤트 루프가 깨어나 명령을 처리한다.

## 6. 스레드 할당

소켓이 새 연결을 생성할 때 다음 기준으로 I/O 스레드를 선택한다:

1. **affinity mask** — 설정된 경우 후보 집합을 제한
2. **부하 분산** — 일반 연결은 후보 중 등록된 핸들 수가 가장 적은 스레드(least-load)를
   고르고, STREAM 연결은 기본적으로 후보를 round-robin으로 고른다
   (`ZLINK_ASIO_STREAM_SESSION_SCHED=minload`로 least-load 전환)

이렇게 네트워크 연결이 I/O 스레드에 분산된다. 할당 단위는
소켓이 아닌 **연결(connection)** 이다 — 하나의 소켓이 여러 연결을 가지면
여러 I/O 스레드에 걸칠 수 있다.

## 7. 튜닝 가이드라인

| 시나리오 | 권장 `ZLINK_IO_THREADS` |
|----------|------------------------|
| 소켓 1개, 연결 소수 | 1 |
| 소켓 다수 또는 연결 다수 | 2–4 (기본값 4) |
| 고성능 서버 (100+ 연결) | 가용 CPU 코어 수에 맞춤 |

I/O 스레드를 CPU 코어 수 이상으로 설정해도 이점이 없고 context-switch
오버헤드만 늘어난다. 4 이상으로 올리기 전에
[perf 벤치마크](../../../bindings/c/perf)로 프로파일링하라.

## 주요 소스 파일

| 파일 | 역할 |
|------|------|
| `core/src/runtime/core/io_thread.hpp/.cpp` | I/O thread 클래스, mailbox 처리 |
| `core/src/runtime/core/ctx_runtime_resources.cpp` | `start_io_threads_locked()`에서 스레드 생성 |
| `core/src/runtime/engine/asio/asio_poller.hpp/.cpp` | Boost ASIO 이벤트 루프, 소켓 모니터링 |
| `core/src/runtime/core/poller_base.hpp` | Worker thread 기반 클래스 |
| `core/src/runtime/core/mailbox.hpp` | Lock-free command queue + signaler |

---
[← Threading Model](threading-model.ko.md)
