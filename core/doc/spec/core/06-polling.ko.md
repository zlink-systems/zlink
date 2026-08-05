---
title: "Poll과 poller"
---

[English](06-polling.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Events](05-events.ko.md) | [다음: Monitoring](07-monitoring.ko.md)
<!-- zlink-nav:end -->

# Poll과 poller

> **이 장이 정의하는 것** — `zlink_poller_*` API로 여러 소켓의 readiness를 기다리는 공개 계약.

이 문서는 ZLink Core의 readiness 공개 계약을 정의한다. 대상 독자는 raw socket, file descriptor와 generic timer를
하나의 event loop에서 기다리는 C API와 bindings 개발자다. 이 문서는 “각 source의 `POLLIN`과 `POLLOUT`,
single-consumer receive mode와 lifetime은 무엇인가?”에 답한다.

## 1. 공개 타입

```c
#if defined _WIN32
typedef uintptr_t zlink_fd_t;
#else
typedef int zlink_fd_t;
#endif

typedef short zlink_poller_event_mask_t;

typedef enum zlink_poller_event_flag_e {
  ZLINK_POLLIN         = 1,
  ZLINK_POLLOUT        = 2,
  ZLINK_POLLERR        = 4,
  ZLINK_POLLPRI        = 8,
  ZLINK_POLLITEMS_DFLT = 16,
  ZLINK_POLLCOMPLETION = 32
} zlink_poller_event_flag_e;

#define ZLINK_HAVE_POLLER 1

typedef enum zlink_poller_source_kind_t {
  ZLINK_POLLER_SOURCE_SOCKET    = 1,
  ZLINK_POLLER_SOURCE_FD        = 2,
  ZLINK_POLLER_SOURCE_TIMER     = 3
} zlink_poller_source_kind_t;

typedef struct zlink_pollitem_t {
  void *socket;
  zlink_fd_t fd;
  short events;
  short revents;
} zlink_pollitem_t;

typedef struct zlink_poller_event_t {
  zlink_poller_source_kind_t source_kind;
  void *socket;
  zlink_fd_t fd;
  void *timer;
  void *user_data;
  short events;
} zlink_poller_event_t;
```

`SOCKET` source만 `socket`, `FD` source만 `fd`, `TIMER` source만 `timer`가 유효하다. `user_data`는 등록 시
받은 pointer를 그대로 돌려주는 borrowed value다.

## 2. 일회성 poll

```c
ZLINK_EXPORT int zlink_poll(
  zlink_pollitem_t *items,
  int item_count,
  long timeout_ms,
  zlink_config_result_t *error_out);
```

return은 readiness가 있는 item 수, timeout은 0, 실패는 -1이다. 실패하면 `error_out`과 errno를 함께
설정한다. `timeout_ms == -1`은 무기한, 0은 즉시 반환한다. item의 `revents`는 호출 전에 0으로 초기화하고
함수 반환 뒤의 snapshot만 유효하다.

## 3. 재사용 poller

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

같은 source를 두 번 add하면 `ZLINK_CONFIG_CONFLICT`/`EEXIST`다. 없는 source의 modify·remove는
`ZLINK_CONFIG_NOT_FOUND`/`ENOENT`다. poller는 source handle을 빌리므로 등록 source를 destroy하기 전에
remove해야 한다. 등록 source가 close되면 `POLLERR`를 한 번 반환하고 이후 remove할 때까지 유지한다.

poller 하나의 add, modify, remove와 wait는 caller가 직렬화한다. 서로 다른 poller는 동시에 사용할 수
있다. wait가 반환한 event array는 caller-owned이며 Core 내부 pointer를 포함하지 않는다.

## 4. Source별 readiness

| Source | `POLLIN` | `POLLOUT` | 추가 규칙 |
|---|---|---|---|
| raw socket | complete record를 수신할 수 있음 | submit 재시도 가치가 있음 | socket별 receive mode 적용 |
| timer | fire count를 받을 수 있음 | 미지원 | `zlink_timer_recv()`로 drain |
| FD | platform readable | platform writable | platform poll 의미 사용 |

`ZLINK_POLLITEMS_DFLT`는 내부·application stack buffer의 권장 초기 item 수이며 readiness bit가 아니다.
`ZLINK_HAVE_POLLER == 1`은 이 public poller API가 build에 포함되었음을 뜻한다.

`ZLINK_POLLCOMPLETION`은 raw DEALER 또는 ROUTER를 `zlink_poller_add()`로 등록할 때만 사용할 수 있다. 단독으로
등록하거나 `ZLINK_POLLIN`, `ZLINK_POLLOUT`과 OR할 수 있다. 이 조합을 사용하면 poller 하나가 같은 socket의
receive, send와 completion 진행을 함께 담당한다. Request completion signal은 public receive record가 아니다.
`zlink_poller_wait()`가 signal을 관측하면 paired Completion transport에서 reply payload를
수신하고 등록된 reply callback을 그 wait 호출의 thread에서 dispatch한다. Reply payload를
두 번째 내부 payload queue로 복사하지 않는다. Timeout, shutdown과 같이 payload가 없는
terminal 결과는 callback 실행 thread를 유지하기 위한 작은 control queue를 사용할 수 있다.
Completion signal만 처리한 경우 public event를 만들지 않으므로 wait는 `0`을 반환할 수
있으며, callback이 변경한 caller-owned 상태를 확인한 뒤 다음 작업을 진행할 수 있다.
`recv_part` 계열은 이 completion을 drain하지 않는다. 다른 source, `zlink_poll()` item 또는
`zlink_poller_modify()`에 사용하면 `ZLINK_CONFIG_INVALID_ARGUMENT`, `errno == EINVAL`이다.

## 5. 오류와 close

잘못된 event bit는 `ZLINK_CONFIG_INVALID_ARGUMENT`/`EINVAL`, source가 지원하지 않는 event는
`ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`이다. poller destroy 중 wait가 active이면
`ZLINK_CLOSE_BUSY`/`EBUSY`다. result와 errno의 전체 대응은 [errno map](04-errno-map.ko.md)을 따른다.
