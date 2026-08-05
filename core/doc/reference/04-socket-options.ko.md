한국어 | [English](04-socket-options.en.md)

[레퍼런스 목차](README.ko.md)

# 04. Socket options and identity

이 category는 생성 이후 socket을 구성하는 진입점 — 공통 옵션 쌍, routing identity, TLS 역할
구성을 다룬다. Socket 타입별 옵션(예: `zlink_set_router_option`)은 여기가 아니라 각자의
socket-type category에 있다. 정확한 signature는
[Socket 공통 스펙](../spec/core/socket/README.ko.md)이 소유한다.

---

## `zlink_set_option` / `zlink_get_option`

모든 socket 타입과 discovery가 공유하는 공통 socket 옵션을 설정하거나 읽는다.

```c
uint64_t sndhwm = 8_000_000;
zlink_set_option(s, ZLINK_OPT_SNDHWM, &sndhwm, sizeof(sndhwm));

int linger = -1;
size_t linger_len = sizeof(linger);
zlink_get_option(s, ZLINK_OPT_LINGER, &linger, &linger_len);
```

**Parameters.** `handle_`는 raw socket 또는 discovery일 수 있다. `option_`은
`zlink_option_t` 값이다(transport/buffer: `AFFINITY`, `RATE`, `RECOVERY_IVL`,
`SNDBUF`/`RCVBUF`, `SNDHWM`/`RCVHWM`, `AUTO_HWM_MSG_UNIT_BYTES`, `MAXMSGSIZE`; timing:
`LINGER`, `RCVTIMEO`/`SNDTIMEO`, `CONNECT_TIMEOUT`, `RECONNECT_IVL`/`_MAX`,
`HANDSHAKE_IVL`, `SUBMIT_RETRY_*`; TCP: `TCP_KEEPALIVE*`, `TCP_MAXRT`, `TCP_NODELAY`;
network: `IPV6`, `TOS`, `MULTICAST_*`, `BINDTODEVICE`, `BACKLOG`; TLS: `TLS_CERT`/`_KEY`/`_CA`/
`_VERIFY`/`_REQUIRE_CLIENT_CERT`/`_HOSTNAME`/`_TRUST_SYSTEM`/`_PASSWORD`; behavior:
`IMMEDIATE`, `CONFLATE`, `BLOCKY`(대신 `ZLINK_CTX_OPT_BLOCKY`로 읽는다 — 전체 목록과
기본값은 Socket 스펙의 상수 표 참고), `INVERT_MATCHING`, `ZMP_METADATA`; 읽기 전용: `FD`,
`EVENTS`, `TYPE`, `LAST_ENDPOINT`, `ROUTE_VALUE_MAX_SIZE`). `optval_`/`optvallen_`는 값과 그
바이트 크기를 공급하거나 받는다 — `SNDHWM`, `RCVHWM`, `AUTO_HWM_MSG_UNIT_BYTES`는 정확히
`sizeof(uint64_t)` 바이트가 필요하다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. 알 수 없는 옵션, 범위를 벗어난 값, 또는 byte-count 옵션이 정확한 크기를
쓰지 않으면(legacy 4바이트 값은 재해석하지 않고 거부) `EINVAL`. Context가 종료됐으면
`ETERM`(`set`에만 해당).

**선택 기준.** Routing ID, TLS 역할 구성, subscribe/unsubscribe(전용 함수가 있음 — 아래와
SUB/XSUB category 참고)를 제외한 모든 옵션에 이 쌍을 쓴다. HWM은 방향별 pipe마다 회계된
바이트 단위로 적용된다 — 한도에 도달하면 이후 write는 receiver로부터의 byte credit을
기다린다. 다만 빈 pipe는 여전히(대신 `MAXMSGSIZE`로 한정된) 큰 메시지 하나는 막기 전에 받을
수 있다. Core는 보통 `ceil(hwm_bytes / 2)`로 credit을 batch하며, sender가 현재 HWM에 막혀
있고 receiver가 보이는 입력을 전부 소화했으면 low-water mark 전에 한 번의 조기 credit 갱신을
돌려줄 수 있다.

---

## `zlink_set_routing_id` / `zlink_get_routing_id`

Socket이 peer에게 내보이는 routing identity를 설정하거나 읽는다.

```c
zlink_set_routing_id(s, "worker-3", 8);

zlink_routing_id_t rid;
zlink_get_routing_id(s, &rid);
```

**Parameters.** `set`은 `data_`/`size_`를 받는다 — 1..255 바이트, binary-safe하며
`bind`/`connect` 전에 호출해야 한다. `get`은 caller가 소유한 `zlink_routing_id_t *out_`에
쓴다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. Routing identity를 지원하지 않는 handle 종류면
`ZLINK_CONFIG_NOT_SUPPORTED`와 `ENOTSUP`.

**선택 기준.** Peer가 안정적이고 예측 가능한 identity를 필요로 하면 연결·bind 전에 설정한다 —
그렇지 않으면 socket이 만들어질 때 Core가 16바이트 RFC 4122 UUID v4를 raw 바이트로(UUID
문자열이 아니라) 발급한다. `ZLINK_OPT_RID_DUPLICATE_POLICY`(`zlink_set_option`을 통해)는
local socket이 같은 routing ID를 광고하는 다른 peer를 관측했을 때의 동작을 제어한다
(`ZLINK_RID_DUPLICATE_REJECT`는 기존 pipe를 유지하고, `ZLINK_RID_DUPLICATE_HANDOVER`는
같은 방향의 재연결이 기존 pipe를 이어받게 한다) — 이 옵션은 자신만의 4바이트 connection
routing ID를 발급하는 `STREAM`에는 영향이 없다.

---

## `zlink_set_tls_server` / `zlink_set_tls_client`

지원하는 socket에 TLS 서버 또는 클라이언트 역할을 구성한다.

```c
zlink_set_tls_server(s, "server.crt", "server.key", /*require_client_cert=*/0);
// 또는 연결하는 쪽에서:
zlink_set_tls_client(s, "ca-bundle.crt", "server.example.com", /*trust_system=*/1);
```

**Parameters.** `set_tls_server`는 `cert_`/`key_`(PEM 파일 경로)와
`require_client_cert_`(상호 인증을 요구하려면 1)를 받는다. `set_tls_client`는
`ca_cert_`(PEM CA bundle 경로), `hostname_`(SNI와 인증서 검증에 쓸 기대 hostname),
`trust_system_`(시스템 CA store도 신뢰하려면 1)를 받는다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. 지원하지 않는 raw socket 타입이나 handle 종류면
`ZLINK_CONFIG_NOT_SUPPORTED`와 `ENOTSUP`.

**선택 기준.** `tls://` endpoint의 bind하는 쪽에는 `set_tls_server`를, 연결하는 쪽에는
`set_tls_client`를 쓴다(Socket lifecycle category의 `zlink_bind`/`zlink_connect`). 이 둘이
표준 역할 구성 진입점이다 — 개별 `ZLINK_OPT_TLS_*` 값(위
`zlink_set_option`/`zlink_get_option`)은 TLS를 지원하는 raw network socket에서만 개별 TLS
값을 구성·조회한다.

---

전체 근거는 [Socket 공통 스펙](../spec/core/socket/README.ko.md)을 참고한다.
