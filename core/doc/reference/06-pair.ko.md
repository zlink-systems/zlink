한국어 | [English](06-pair.en.md)

[레퍼런스 목차](README.ko.md)

# 06. PAIR

1:1 양방향 raw socket 타입이다. PAIR에는 타입 전용 옵션도, 전용 수신 함수도 없다 — 다른
socket 타입과 `zlink_recv_part`(Raw receive category), `zlink_send_ready_handler`(Socket
lifecycle category)를 공유한다. 타입 전용 항목은 send 쪽 하나뿐이다. 정확한 signature는
[PAIR 스펙](../spec/core/socket/01-pair.ko.md)이 소유한다.

---

## `zlink_send_part`

PAIR(그리고 DEALER — DEALER category 참고) socket에서 메시지 part 하나를 보낸다.

```c
zlink_msg_t part;
zlink_msg_init_size(&part, payload_len);
memcpy(zlink_msg_data(&part), payload, payload_len);
zlink_submit_result_t result = zlink_send_part(s, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `part_`은 보낼 message다 — 성공·실패 관계없이 내용이 소비된다(다시 보내야
하면 먼저 복사하고, 재사용 전에 다시 초기화한다). `flags_`는 `ZLINK_SEND_FLAGS_NONE` 또는
`ZLINK_DONTWAIT`다. `part_flag_`는 단일 part message면 `ZLINK_PART_FINAL`이고, multipart
sequence를 시작하려면 `ZLINK_PART_MORE`를 쓴 뒤 같은 스레드에서 이 함수를 계속 호출해 마지막
`ZLINK_PART_FINAL` 호출로 마친다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 성공하면 `ZLINK_SUBMIT_OK`(전체
결과 매핑은 Errors, results, and version category 참고). Non-blocking 호출이 즉시 진행할 수
없으면 `ZLINK_SUBMIT_BACKPRESSURED`.

**선택 기준.** Core는 multipart sequence의 성공한 중간 part들을 마지막 part가 성공할
때까지 하나의 record로 스테이징한다 — 열린 sequence의 어느 part든 실패하면 Core는 스테이징된
모든 part와 실패한 part를 원자적으로 폐기하며, 그 record의 어떤 part도 peer에게 보이지
않는다. 실패한 호출도 자신의 `part_`을 소비하므로, 원래 호출 전에 보관해 둔 복사본으로 record
전체를 처음 part부터 재시도한다 — 한 sequence 도중에 다른 send helper를 부르거나 flags를
바꾸지 않는다.

---

전체 근거는 [PAIR 스펙](../spec/core/socket/01-pair.ko.md)을 참고한다. 수신은
`zlink_recv_part`(Raw receive category), send-ready 통지는
`zlink_send_ready_handler`(Socket lifecycle category)를 쓴다 — 여기서 반복하지 않는다.
