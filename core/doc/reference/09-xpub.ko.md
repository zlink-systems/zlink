한국어 | [English](09-xpub.en.md)

[레퍼런스 목차](README.ko.md)

# 09. XPUB

구독자로부터 구독 이벤트도 받고 수동 구독 제어도 하는 확장 publisher다. XPUB은
`zlink_set_pub_option`/`zlink_get_pub_option`과 `zlink_publish_part`를 PUB(PUB category)와
변경 없이 공유한다 — 이 category는 XPUB에 고유한 진입점 하나, 즉 구독 이벤트 자체를 수신하는
항목만 다룬다. 정확한 signature는 [XPUB 스펙](../spec/core/socket/04-xpub.ko.md)이
소유한다.

---

## `zlink_xpub_recv_part`

XPUB socket의 구독자로부터 다음 구독/구독 해제 이벤트를 수신한다.

```c
const zlink_routing_id_t *subscriber_rid;
int subscribed;
char topic[256];
size_t topic_len;
zlink_xpub_recv_part(xpub, &subscriber_rid, &subscribed, topic, sizeof(topic), &topic_len, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `source_rid_out_`는 구독하는 peer의 routing ID에 대한 library 소유
view를 받으며, 이 socket에 대한 다음 호출까지 유효하다. `subscribed_out_`는 구독이면 `1`,
구독 해제면 `0`으로 설정된다. `topic_id_buf_`/`topic_id_capacity_`는 caller의 buffer다.
`topic_id_len_out_`는 topic의 실제 길이를 받는다.

**Return과 errno.** `zlink_recv_result_t`를 반환한다 — 성공하면
`ZLINK_RECV_OK`. Socket handle이 `NULL`이면 `EFAULT`. `ZLINK_DONTWAIT`이고 받을 이벤트가
없으면 `EAGAIN`. Topic이 `topic_id_capacity_`를 넘으면 `EMSGSIZE`. Subject가 XPUB가
아니면 `EINVAL`. `EAGAIN`/`ETERM` 외의 상세 errno는 반환값으로 `ZLINK_RECV_INTERNAL_ERROR`를
드러내며, 구체적 원인은 `zlink_errno()`로 확인한다.

**선택 기준.** 이는 XPUB 전용 recv-mode 호출이다 — raw XPUB만 적용 대상이다. 중복 구독/구독
해제 이벤트를 전달할지는 `ZLINK_PUB_OPT_VERBOSE`/`ZLINK_PUB_OPT_VERBOSER`(PUB category)로
제어하고, Core가 자동으로 구독을 받아들이는 대신 application이 직접
`ZLINK_PUB_OPT_APPROVE_SUBSCRIBE`/`ZLINK_PUB_OPT_REJECT_SUBSCRIBE`로 승인·거부하려면
`ZLINK_PUB_OPT_MANUAL`을 쓴다.

---

전체 근거는 [XPUB 스펙](../spec/core/socket/04-xpub.ko.md)을 참고한다. 발행과 옵션은
`zlink_publish_part`/`zlink_set_pub_option`/`zlink_get_pub_option`(PUB category)을 쓰고,
send-ready 통지는 `zlink_send_ready_handler`(Socket lifecycle category)를 쓴다 — 여기서
반복하지 않는다.
