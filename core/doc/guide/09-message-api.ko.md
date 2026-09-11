---
title: "Message API와 ownership"
---

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: 설계 근거](design-rationale.ko.md) | [다음: Thread safety](11-thread-safety.ko.md)
<!-- zlink-nav:end -->

# Message API와 ownership

> **이 장의 계약 소유 문서** — [Message](../spec/core/02-message.ko.md)가 다룬다. 이
> 챕터는 message 소유권과 API 사용법을 설명한다.

`zlink_msg_t`는 message part 하나를 소유한다. 사용 전에 초기화하고, ownership이 move되거나 send에
전달해 소비되지 않았다면 정확히 한 번 close한다. Whole-message send는 성공·실패 모두 배열의 모든
part를 소비하며, 소비된 슬롯은 빈 초기화 상태로 남아 그대로 close하거나 다시 쓸 수 있다.

## Part 생성

- `zlink_msg_init()`은 빈 part를 만든다.
- `zlink_msg_init_size()`는 쓸 수 있는 storage를 할당한다.
- `zlink_msg_init_data()`는 caller가 제공한 data와 release callback을 연결한다.
- `zlink_msg_copy()`는 storage를 공유하고 `zlink_msg_move()`는 ownership을 옮긴다.

## Multipart send

모든 part를 배열 순서대로 두고 한 번의 `zlink_send()` 호출로 record 전체를 보낸다. Send가 소비한
배열의 내용은 성공·실패 모두 사라지므로, 같은 record를 다시 보내려면 호출 전에 전체를 복사해 둔다.

```c
zlink_msg_t parts[1];
zlink_msg_init_size(&parts[0], payload_size);
memcpy(zlink_msg_data(&parts[0]), payload, payload_size);
/* send는 성공·실패 모두 배열 전체를 소비한다 — 호출 뒤 슬롯은 빈 상태다. */
zlink_send(socket, parts, 1, ZLINK_SEND_FLAGS_NONE, NULL, NULL);
```

## Receive

Typed receive 함수는 caller가 제공한 `zlink_msg_t` 배열에 record 전체를 채우고 part 수를 반환한다.
슬롯은 미리 초기화하지 않아도 된다. 성공한 배열은 `zlink_multipart_close()`로 닫거나 각 part를
정확히 한 번 move한다. Routing id와 topic은 payload frame이 아니라 metadata로 반환된다.

성공한 REQUEST completion에서 `zlink_completion_recv()`는 Core가 소유하던 연속 reply 배열을
`zlink_completion_t`로 옮긴다. Part를 읽거나 move한 뒤 `zlink_completion_close()`를 호출하고,
배열을 직접 free하지 않는다.
