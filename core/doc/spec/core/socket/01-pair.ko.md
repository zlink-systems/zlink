---
title: "소켓 — PAIR"
---

[English](01-pair.en.md) | 한국어

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: 소켓 공통](README.ko.md) | [다음: PUB](02-pub.ko.md)
<!-- zlink-nav:end -->

# 소켓 — PAIR

> **이 장이 정의하는 것** — PAIR 소켓의 1:1 독점 연결 동작과 공개 계약.

1:1 양방향 소켓입니다. 양쪽 모두 메시지를 송수신할 수 있으며 타입 전용
옵션은 없습니다.

## 적용 함수

### zlink_send_part

메시지 파트 하나를 전송합니다.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part (
  void *s_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

단일 파트 메시지는 `ZLINK_PART_FINAL`로 전송합니다. 멀티파트 메시지는
`ZLINK_PART_MORE`로 시작해 같은 스레드에서 같은 함수와 `flags_`를 사용하여
`ZLINK_PART_FINAL`까지 이어서 전송합니다.

이 함수는 성공과 실패 모두에서 `part_`의 내용을 소비합니다. 같은 내용을
다시 사용할 가능성이 있으면 호출 전에 복사해야 하며, 소비된 `zlink_msg_t`를
다시 사용하려면 먼저 초기화해야 합니다. `flags_`에는
`ZLINK_SEND_FLAGS_NONE` 또는 `ZLINK_DONTWAIT`를 전달합니다. 논블로킹 호출이
즉시 진행할 수 없으면 `ZLINK_SUBMIT_BACKPRESSURED`를 반환합니다.

Core는 성공한 중간 파트를 `ZLINK_PART_FINAL`이 성공할 때까지 하나의 record로
staging합니다. 열린 sequence에서 중간 또는 마지막 submit 하나라도 실패하면
이전에 staging한 파트와 실패한 파트를 원자적으로 폐기하고 sequence를 닫습니다.
peer에는 그 record의 어떤 파트도 보이지 않습니다. 실패한 호출의 `part_`도 위
규칙대로 소비되며, 다음 submit은 새 record의 첫 파트로 시작합니다. 따라서
재시도하려면 호출 전에 보관한 전체 record를 첫 파트부터 다시 제출해야 합니다.

**반환값:** 성공 시 `ZLINK_SUBMIT_OK`, 실패 시 원인을 나타내는
`zlink_submit_result_t` 값. 전체 대응은 [errno map](../04-errno-map.ko.md)을
따릅니다.

**참고:** `zlink_recv_part`, `zlink_send_ready_handler`

---

### zlink_recv_part

메시지 파트 하나를 수신합니다.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (
  void *s_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

`part_out_`은 초기화된 메시지여야 하며 `has_more_out_`과 함께 필수입니다.
`source_rid_out_`은 선택 사항이고 PAIR에서는 성공 시 `NULL`을 받습니다.
성공하면 수신 파트의 소유권이 호출자에게 이전되므로
`zlink_msg_close(part_out_)`를 정확히 한 번 호출해야 합니다. 수신 파트를
얻기 전에 실패하면 소유권은 이전되지 않습니다.

`*has_more_out_`은 다음 파트가 있으면 `ZLINK_PART_MORE`, 마지막 파트이면
`ZLINK_PART_FINAL`입니다. 한 멀티파트 메시지는 첫 파트부터 마지막 파트까지
같은 스레드에서 이 함수로 계속 수신합니다. 일반적인 경로는 poller에서
`ZLINK_POLLIN`을 관찰한 뒤 호출하는 방식입니다. `ZLINK_DONTWAIT` 호출에
수신할 데이터가 없으면 `ZLINK_RECV_NO_DATA`를 반환합니다.

**반환값:** 성공 시 `ZLINK_RECV_OK`, 실패 시 `zlink_recv_result_t` 값.

**참고:** `zlink_send_part`, `zlink_msg_close`

---

### zlink_send_ready_handler

send-ready 콜백을 설정하거나 교체합니다.

```c
ZLINK_EXPORT zlink_handler_result_t zlink_send_ready_handler (
  void *s_, zlink_send_ready_handler_fn handler_, void *userdata_);
```

핸들러는 교체 전용이며 `NULL`은 유효하지 않습니다. 교체 성공은 다음 쓰기
가능 전환부터 반영됩니다. 같은 핸들의 send-ready 콜백 안에서 재진입하면
`ZLINK_HANDLER_DEADLOCK`, `errno == EDEADLK`로 실패합니다.

이 콜백과 `ZLINK_POLLOUT`은 같은 send-recovery readiness 축을 나타냅니다.
신호는 송신 재시도를 시도할 가치가 있다는 뜻이며 다음 재시도의 성공을
보장하지 않습니다.

**반환값:** 성공 시 `ZLINK_HANDLER_OK`, 실패 시 `zlink_handler_result_t` 값.

**참고:** `zlink_send_part`
