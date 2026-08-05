한국어 | [English](07-pub.en.md)

[레퍼런스 목차](README.ko.md)

# 07. PUB

Topic 기반 fan-out만 하는 publish 전용 raw socket 타입이다. PUB은 send 전용이다 — 수신
함수가 없다. 다른 socket 타입과 `zlink_send_ready_handler`(Socket lifecycle category)를
공유한다. 정확한 signature는 [PUB 스펙](../spec/core/socket/02-pub.ko.md)이 소유한다.

---

## `zlink_set_pub_option` / `zlink_get_pub_option`

PUB/XPUB 전용 옵션을 설정하거나 읽는다.

```c
int nodrop = 1;
zlink_set_pub_option(s, ZLINK_PUB_OPT_NODROP, &nodrop, sizeof(nodrop));
```

**Parameters.** `option_`은 `zlink_pub_option_t` 값이다 — `VERBOSE`/`VERBOSER`(구독·구독
해제 메시지를 upstream으로 전달), `MANUAL`/`MANUAL_LAST_VALUE`(수동 구독 관리와 last-value
caching), `NODROP`(기본값 `0` — 아래 참고), `WELCOME_MSG`(연결 시 새 구독자에게 보낼
메시지), `TOPICS_COUNT`(읽기 전용), `APPROVE_SUBSCRIBE`/`REJECT_SUBSCRIBE`(manual 모드
구독 결정).

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. 모든 socket 타입이 공유하는 옵션은 대신
`zlink_set_option`/`zlink_get_option`(Socket options and identity category)을 쓴다.

**선택 기준.** Fanout 전달은 기본적으로 손실을 허용한다 — HWM에 도달하면
`zlink_publish_part`는 영향받는 구독자에 대해 메시지를 버리고 성공을 보고한다. Application이
버리지 않고 send queue가 가득 차서 publisher를 backpressure해야 한다면
`ZLINK_PUB_OPT_NODROP`을 `1`로 설정한다 — 그러면 `zlink_publish_part`가
`ZLINK_SUBMIT_BACKPRESSURED`를 반환한다 — 다만 이는 publisher를 가장 느린 구독자에게
결합시킨다. 하나의 가득 찬 pipe가 그 socket의 모든 구독자에 대한 전달을 멈추기 때문이다.
구독자 속도에 의존하면 안 되는 신뢰성 있는 전달은 PUB/SUB가 아니라 request-reply socket에
속한다.

---

## `zlink_publish_part`

`PUB` 또는 `XPUB` socket에서 topic으로 주소를 지정해 메시지 part 하나를 발행한다.

```c
zlink_msg_t part;
zlink_msg_init_size(&part, payload_len);
memcpy(zlink_msg_data(&part), payload, payload_len);
zlink_publish_part(pub, "lobby.events", &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `topic_id_`가 `NULL`이면 wire-prefix 관례(첫 frame이 topic을 담음)를
쓰고, 널 종료 문자열(내부에 NUL 없이)이면 Core가 그 바이트를 topic frame으로 앞에 붙인다 —
별도의 topic 길이 상한은 없다. Topic 바이트는 메시지·storage 크기 제한에 포함된다.
`part_`/`flags_`/`part_flag_`는 `zlink_send_part`(PAIR category)와 같은 규칙을 따른다 —
두 결과 모두 내용이 소비되며, `ZLINK_PART_MORE`/`ZLINK_PART_FINAL`은 같은 스레드에서
multipart sequence를 시작·계속·종료한다(sequence 도중 topic이나 flags를 바꾸지 않음).

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 성공하면
`ZLINK_SUBMIT_OK`. Topic 바이트가 크기 제한을 넘으면 `ZLINK_SUBMIT_INVALID_ARGUMENT`와
`EMSGSIZE`. Topic-frame storage 할당이 실패하면 `ZLINK_SUBMIT_OUT_OF_MEMORY`와
`ENOMEM`. `PUB`/`XPUB` 외의 raw socket 타입이면 `ZLINK_SUBMIT_NOT_SUPPORTED`와
`ENOTSUP`. Non-blocking 호출이 즉시 진행할 수 없으면 `ZLINK_SUBMIT_BACKPRESSURED`.

**선택 기준.** `zlink_send_part`와 마찬가지로 Core는 multipart publish record를 원자적으로
스테이징한다 — 열린 sequence의 어느 부분이든 실패하면 스테이징된 모든 part가 폐기되어
구독자에게 아무것도 보이지 않으며, 재시도는 보관해 둔 복사본으로 record 전체를 처음 part부터
다시 제출한다.

---

전체 근거는 [PUB 스펙](../spec/core/socket/02-pub.ko.md)을 참고한다. Send-ready 통지는
`zlink_send_ready_handler`(Socket lifecycle category)를 쓴다 — 여기서 반복하지 않는다.
