---
title: "Message API와 ownership"
---

[English](09-message-api.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: 설계 근거](design-rationale.ko.md) | [다음: Thread safety](11-thread-safety.ko.md)
<!-- zlink-nav:end -->

# Message API와 ownership

> **이 장의 계약 소유 문서** — [Message](../spec/core/02-message.ko.md)가 다룬다. 이
> 챕터는 message 소유권과 API 사용법을 설명한다.

`zlink_msg_t`는 message part 하나를 소유한다. 사용 전에 초기화하고, ownership이 move되거나 성공한
send가 소비하지 않았다면 정확히 한 번 close한다.

## Part 생성

- `zlink_msg_init()`은 빈 part를 만든다.
- `zlink_msg_init_size()`는 쓸 수 있는 storage를 할당한다.
- `zlink_msg_init_data()`는 caller가 제공한 data와 release callback을 연결한다.
- `zlink_msg_copy()`는 storage를 공유하고 `zlink_msg_move()`는 ownership을 옮긴다.

## Multipart send

Typed part API로 각 part를 보낸다. 마지막 전 part에는 `ZLINK_PART_MORE`, 마지막 part에는
`ZLINK_PART_FINAL`을 사용한다. 성공한 send가 소비한 part를 다시 사용하지 않는다.

```c
zlink_msg_t part;
zlink_msg_init_size(&part, payload_size);
memcpy(zlink_msg_data(&part), payload, payload_size);
/* 성공한 final send는 ownership을 Core로 옮긴다. */
zlink_send_part(socket, &part, 0, ZLINK_PART_FINAL);
```

## Receive

Typed receive 함수는 caller가 초기화한 `zlink_msg_t`를 채우고 다음 part가 있는지 반환한다. 수신한
모든 part를 정확히 한 번 close하거나 move한다. Routing id와 topic은 payload frame이 아니라
metadata로 반환된다.

Request/reply completion callback은 전달받은 모든 part를 callback 실행 동안 소유하며 반환하기 전에
close하거나 move해야 한다.
