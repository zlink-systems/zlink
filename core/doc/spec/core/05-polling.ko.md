---
title: "Polling"
---

[English](https://zlink-systems.github.io/zlink/spec/core/05-polling/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Events](04-events.ko.md) | [다음: Monitoring](06-monitoring.ko.md)
<!-- zlink-nav:end -->

# Polling

> **이 장이 정의하는 것** — `zlink_poll`과 `zlink_poller_*` API로 여러
> [socket](glossary.ko.md#socket)과 source의 readiness를 기다리는 공개 계약.

## 1. Polling 개요

이 문서는 ZLink Core의 readiness 공개 계약을 정의한다. source가 receive 또는 send를
진행할 가치가 있는 상태를 readiness라 하며, application은 raw socket, OS file
descriptor와 generic timer 세 종류의 source를 하나의 event loop에서 함께 기다릴 수 있다.
대상 독자는 이 계약을 C API와 각 언어 binding으로 옮기는 개발자다. 이 문서는 “각
source의 `POLLIN`과 `POLLOUT`, single-consumer receive mode와 lifetime은 무엇인가?”에
답한다.

관련 계약의 소유 문서는 다음과 같다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| event family 구분과 readiness 의미의 경계 | [Events](04-events.ko.md) |
| socket type별 `ZLINK_POLLIN`·`ZLINK_POLLOUT`의 구체 의미 | [Socket — DEALER](socket/06-dealer.ko.md#5-result와-readiness), [Socket — ROUTER](socket/07-router.ko.md#10-result와-readiness) |
| result와 errno 대응 | [Errors](03-errors.ko.md#result와-errno-대응) |
| generic timer의 생성·수신 (`zlink_timer_*`) | [유틸리티](07-utilities.ko.md) |

## 2. 일회성 poll과 재사용 poller

readiness를 기다리는 방법은 두 가지다.

- **일회성 poll** — [`zlink_poll`](#zlink_poll)은 매 호출마다 item 배열로 대상을
  전달받아 한 번 기다린다.
- **재사용 poller** — [`zlink_poller_*`](#poller-함수) 함수는 poller 객체에 source를
  등록해 두고 `zlink_poller_wait`로 반복해서 기다린다.

## 3. Source 종류와 readiness

각 source 종류가 `ZLINK_POLLIN`과 `ZLINK_POLLOUT`으로 알리는 readiness는 다음과 같다.

| Source | `POLLIN` | `POLLOUT` | 추가 readiness·규칙 |
|---|---|---|---|
| raw socket | complete record를 수신할 수 있음 | submit 재시도 가치가 있음 | socket별 receive mode 적용. close된 등록 socket은 `ZLINK_POLLERR` 1회 |
| timer | fire count를 받을 수 있음 | 미지원 | `zlink_timer_recv()`로 drain |
| FD | platform readable | platform writable | platform `POLLPRI`는 `ZLINK_POLLPRI`, 그 밖의 platform 오류 bit는 `ZLINK_POLLERR`로 변환 |

여러 peer를 가진 raw socket의 `ZLINK_POLLOUT`은 socket 전체의 집계 readiness다.
이 event는 writable해진 routing ID나 transport pair를 식별하지 않으며, 다른 peer의
여유 때문에 설 수 있다. 따라서 특정 target의 nonblocking submit이 backpressure를
반환한 뒤 `ZLINK_POLLOUT`을 관측해도 그 target의 다음 submit 성공은 보장되지 않는다.
operation별 admission 대기는 `zlink_send_async`와 completion 통지를 사용한다.

`ZLINK_POLLITEMS_DFLT`는 내부·application stack buffer의 권장 초기 item 수이며
readiness bit가 아니다. `ZLINK_HAVE_POLLER == 1`은 이 public poller API가 build에
포함되었음을 뜻한다.

## 4. Completion polling

`ZLINK_POLLCOMPLETION`은 completion channel을 보유한 raw `PAIR`, `DEALER`, `ROUTER`,
`STREAM`을 `zlink_poller_add()`로 등록할 때 사용할 수 있다. `DEALER`·`ROUTER`는
reply completion, `PAIR`·`STREAM`을 포함한 asynchronous send 지원 socket은 send
completion channel을 사용한다. 단독으로 등록하거나 `ZLINK_POLLIN`, `ZLINK_POLLOUT`과
OR할 수 있다.
이 조합을 사용하면 poller 하나가 같은 socket의 receive, send와 completion 진행을 함께
담당한다.

Request completion signal은 public receive record가 아니다. `zlink_poller_wait()`가
signal을 관측하면 paired Completion transport에서 reply payload를 수신하고 등록된
reply callback을 그 wait 호출의 thread에서 dispatch한다. Completion signal만 처리한
경우에도 `ZLINK_POLLCOMPLETION` bit를 가진 public event를 event array에 기록하고 그
event를 반환 count에 포함한다. 따라서 completion을 처리한 wait가 그 이유로
`0`을 반환하지 않는다. `recv_part` 계열은 이 completion을 drain하지 않는다.

```mermaid
sequenceDiagram
    participant App as Application
    participant P as Poller
    participant CT as paired Completion transport
    App->>P: zlink_poller_wait() 호출
    Note over P: completion signal 관측
    P->>CT: reply payload 수신
    P->>App: 등록된 reply callback을 wait 호출 thread에서 dispatch
    P-->>App: POLLCOMPLETION event + count 반환
    Note over App: callback 결과와 event bit 확인 후 진행
```

`ZLINK_POLLCOMPLETION`을 다른 source, `zlink_poll()` item 또는 `zlink_poller_modify()`에
사용하면 `ZLINK_CONFIG_INVALID_ARGUMENT`, `errno == EINVAL`이다.

## 5. Source 수명과 직렬화

poller에 socket source를 등록하면 Core가 그 socket의 lifetime pin을 획득한다.
따라서 application이 poller에서 remove하기 전에 등록된 socket을 close해도
안전하다. close된 socket source는 `POLLERR`를 한 번 반환하고, 해당 등록과
lifetime pin은 remove할 때까지 유지된다.

poller 하나의 add, modify, remove와 wait는 caller가 직렬화한다. 서로 다른 poller는
동시에 사용할 수 있다. wait가 반환한 event array는 caller-owned이며 Core 내부
pointer를 포함하지 않는다.

## 6. 공개 타입

```c
#if defined _WIN32
typedef uintptr_t zlink_fd_t;
#else
typedef int zlink_fd_t;
#endif

typedef short zlink_poller_event_mask_t;

typedef enum zlink_poller_event_flag_e {
  ZLINK_POLLIN         = 1,   // receive 진행 가능 (source별 의미는 §3)
  ZLINK_POLLOUT        = 2,   // send/submit 재시도 가치 (source별 의미는 §3)
  ZLINK_POLLERR        = 4,   // socket close 또는 FD platform 오류 (§3, §5)
  ZLINK_POLLPRI        = 8,   // FD의 platform POLLPRI (§3)
  ZLINK_POLLITEMS_DFLT = 16,  // 권장 초기 item 수. readiness bit가 아니다 (§3)
  ZLINK_POLLCOMPLETION = 32   // completion channel socket의 polling (§4)
} zlink_poller_event_flag_e;

#define ZLINK_HAVE_POLLER 1   // public poller API가 build에 포함됨

typedef enum zlink_poller_source_kind_t {
  ZLINK_POLLER_SOURCE_SOCKET    = 1,  // raw socket
  ZLINK_POLLER_SOURCE_FD        = 2,  // OS file descriptor
  ZLINK_POLLER_SOURCE_TIMER     = 3   // generic timer
} zlink_poller_source_kind_t;

typedef struct zlink_pollitem_t {
  void *socket;    // SOCKET source일 때만 유효
  zlink_fd_t fd;   // FD source일 때만 유효
  short events;    // 기다릴 event bit
  short revents;   // 반환된 readiness. 호출 전 0으로 초기화한다 (§7 zlink_poll)
} zlink_pollitem_t;

typedef struct zlink_poller_event_t {
  zlink_poller_source_kind_t source_kind;  // 이 event의 source 종류
  void *socket;     // SOCKET source일 때만 유효
  zlink_fd_t fd;    // FD source일 때만 유효
  void *timer;      // TIMER source일 때만 유효
  void *user_data;  // 등록 시 받은 pointer를 그대로 돌려주는 borrowed value
  short events;     // 관측된 readiness bit
} zlink_poller_event_t;
```

## 7. 함수

### zlink_poll

item 배열의 readiness를 한 번 기다린다.

```c
ZLINK_EXPORT int zlink_poll(
  zlink_pollitem_t *items,
  int item_count,
  long timeout_ms,
  zlink_config_result_t *error_out);
```

return은 readiness가 있는 item 수, timeout은 0, 실패는 -1이다. 실패하면 `error_out`과
errno를 함께 설정한다. `timeout_ms == -1`은 무기한, 0은 즉시 반환한다. item의
`revents`는 호출 전에 0으로 초기화하고 함수 반환 뒤의 snapshot만 유효하다.
`error_out`은 NULL을 허용하는 선택 output이다.

### Poller 함수

```c
ZLINK_EXPORT void *zlink_poller_new(void);
ZLINK_EXPORT zlink_close_result_t zlink_poller_destroy(void **poller_p);
ZLINK_EXPORT int zlink_poller_size(void *poller, zlink_config_result_t *error_out);
ZLINK_EXPORT zlink_config_result_t zlink_poller_add(
  void *poller,
  void *source,
  void *user_data,
  short events);
ZLINK_EXPORT zlink_config_result_t zlink_poller_modify(
  void *poller,
  void *source,
  short events);
ZLINK_EXPORT zlink_config_result_t zlink_poller_remove(void *poller, void *source);
ZLINK_EXPORT zlink_config_result_t zlink_poller_add_fd(
  void *poller,
  zlink_fd_t fd,
  void *user_data,
  short events);
ZLINK_EXPORT zlink_config_result_t zlink_poller_modify_fd(
  void *poller,
  zlink_fd_t fd,
  short events);
ZLINK_EXPORT zlink_config_result_t zlink_poller_remove_fd(void *poller, zlink_fd_t fd);
ZLINK_EXPORT zlink_config_result_t zlink_poller_add_timer(
  void *poller,
  void *timer,
  void *user_data);
ZLINK_EXPORT zlink_config_result_t zlink_poller_remove_timer(
  void *poller,
  void *timer);
ZLINK_EXPORT int zlink_poller_wait(
  void *poller,
  zlink_poller_event_t *events,
  int event_capacity,
  long timeout_ms,
  zlink_config_result_t *error_out);
```

`zlink_poller_new()`는 성공 시 새 poller를 반환하고, allocation이 실패하면
`NULL`을 반환하고 `errno`를 `ENOMEM`으로 설정한다. `zlink_poller_destroy()`가
성공하면 caller가 제공한 pointer를 NULL로 설정한다.

`zlink_poller_size()`는 성공 시 현재 등록 count를, 실패 시 `-1`을 반환한다.
`zlink_poller_wait()`는 성공 시 기록한 event 수를, timeout이면 `0`, 실패하면
`-1`을 반환한다. `events == NULL`이거나 `event_capacity <= 0`이면 `EINVAL`로
실패한다. `zlink_poller_size()`와 `zlink_poller_wait()`의 `error_out`은 NULL을
허용하는 선택 output이다.

같은 source를 두 번 add하면 `ZLINK_CONFIG_CONFLICT`/`EEXIST`다. 없는 source의
modify·remove는 `ZLINK_CONFIG_NOT_FOUND`/`ENOENT`다. 잘못된 event bit는
`ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`, source가 지원하지 않는 event는
`ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`이다. poller destroy 중 wait가 active이면
`ZLINK_CLOSE_BUSY`/`EBUSY`다. result와 errno의 전체 대응은
[errno map](03-errors.ko.md#result와-errno-대응)을 따른다.

## 8. 내부 구조

> **이 절의 계약 소유** — completion polling의 공개 계약은 이 문서의
> [Completion polling](#4-completion-polling) 절과 [검증 요구](#9-구현-및-contract-test-검증-요구)
> 절이 소유한다. 이 절은 그 계약을 내부에서 어떻게 달성하는지 설명한다.

Completion reply payload를 두 번째 내부 payload queue로 복사하지 않는다. Timeout,
shutdown과 같이 payload가 없는 terminal 결과는 callback 실행 thread를 유지하기 위한
작은 control queue를 사용할 수 있다.

## 9. 구현 및 contract test 검증 요구

공개 표면(`zlink_poll`·`zlink_poller_*` 함수, 반환값·errno, event array 내용, reply
callback 호출)만으로 다음을 확인한다. 각 항목은 unit test 하나로 이어진다.

**zlink_poll**
- readiness가 있는 item 수를 반환하고, timeout이면 `0`, 실패하면 `-1`을 반환하며 `error_out`과 errno를 함께 설정한다.
- `timeout_ms == -1`은 무기한 대기하고, `0`은 즉시 반환한다.
- 호출 전 `0`으로 초기화한 `revents`는 함수 반환 뒤의 snapshot만 유효하다.

**poller 등록**
- 같은 source를 두 번 add하면 `ZLINK_CONFIG_CONFLICT`/`EEXIST`다.
- 없는 source를 modify·remove하면 `ZLINK_CONFIG_NOT_FOUND`/`ENOENT`다.
- 잘못된 event bit는 `ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`이고, source가 지원하지 않는 event는 `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`이다.
- `ZLINK_POLLCOMPLETION`은 completion channel을 보유한 raw PAIR·DEALER·ROUTER·STREAM의 `zlink_poller_add()`에 사용할 수 있다. 그 외의 source, `zlink_poll()` item 또는 `zlink_poller_modify()`에 사용하면 `ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`이다.

**wait와 event**
- socket source 등록은 lifetime pin을 획득하므로 socket을 remove 전에 close해도 안전하다. close 후 `POLLERR`를 한 번 반환하고 등록은 remove할 때까지 유지된다.
- FD의 platform `POLLPRI`는 `ZLINK_POLLPRI`로, 그 밖의 platform 오류 bit는 `ZLINK_POLLERR`로 변환된다.
- event의 `socket`·`fd`·`timer` field는 각각 SOCKET·FD·TIMER source에서만 유효하고, `user_data`는 등록 시 받은 pointer를 그대로 돌려준다.
- wait가 반환한 event array는 caller-owned이며 Core 내부 pointer를 포함하지 않는다.

**completion polling**
- completion signal은 public receive record가 아니며, wait가 관측하면 paired Completion transport에서 reply payload를 수신하고 등록된 reply callback을 그 wait 호출의 thread에서 dispatch한다.
- completion signal을 처리한 wait는 `ZLINK_POLLCOMPLETION` event를 기록하고 그 event를 반환 count에 포함한다.
- `recv_part` 계열은 completion을 drain하지 않는다.

**수명**
- poller destroy 중 wait가 active이면 `ZLINK_CLOSE_BUSY`/`EBUSY`다.

**poller 함수 반환과 output**
- `zlink_poller_new`의 allocation 실패는 `NULL`/`ENOMEM`이고, `zlink_poller_destroy`가 성공하면 caller pointer가 NULL이 된다.
- `zlink_poller_size`는 등록 count 또는 실패 `-1`을 반환한다.
- `zlink_poller_wait`는 event count, timeout `0`, 실패 `-1`을 반환하며 `events == NULL` 또는 `event_capacity <= 0`은 `EINVAL`이다.
- `zlink_poll`·`zlink_poller_size`·`zlink_poller_wait`의 `error_out`은 NULL을 허용하는 선택 output이다.

poller 하나의 add·modify·remove와 wait를 caller가 직렬화하는 것은 caller의 사용 전제이며([§5](#5-source-수명과-직렬화)), 서로 다른 poller의 동시 사용은 허용된다.
