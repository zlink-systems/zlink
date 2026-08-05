한국어 | [English](02-message.en.md)

[레퍼런스 목차](README.ko.md)

# 02. Message

이 category는 `zlink_msg_t`가 제공하는 진입점 — 초기화, 소유권 이전, accessor를 다룬다.
공개 message API는 payload-part container일 뿐이다 — request-reply나 message별 metadata
연산은 노출하지 않는다. 그런 연산은 socket 계약(Socket lifecycle과 socket-type별 category
참고)에 속한다. 정확한 signature는 [Message 스펙](../spec/core/02-message.ko.md)이 소유한다.

---

## `zlink_msg_init` / `zlink_msg_init_size` / `zlink_msg_init_data`

`zlink_msg_t`를 첫 사용 전에 초기화한다 — 빈 상태, 초기화 안 된 buffer로 크기만 잡은 상태, 또는
zero-copy로 외부 buffer를 감싸는 상태 중 하나로.

```c
zlink_msg_t empty;
zlink_msg_init(&empty);

zlink_msg_t sized;
zlink_msg_init_size(&sized, 128);
memcpy(zlink_msg_data(&sized), payload, 128);

zlink_msg_t zero_copy;
zlink_msg_init_data(&zero_copy, buffer, buffer_len, free_callback, hint);
```

**Parameters.** `init`은 message pointer만 받는다. `init_size`는 `size_`(할당할
바이트 수 — 내용은 초기화되지 않음)를 더 받는다. `init_data`는 `data_`/`size_`(caller가 소유한
buffer), `ffn_`(library가 buffer를 더 이상 필요로 하지 않을 때 호출하는 `zlink_free_fn` —
caller가 buffer가 message보다 오래 산다고 보장하면 `NULL`), `hint_`(`ffn_`에 그대로 전달)를
더 받는다.

**Return과 errno.** 셋 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. `init_size`는 할당이 실패하면 `ENOMEM`으로 실패한다.

**선택 기준.** 다른 message 함수에 넘기기 전에 `zlink_msg_t`를 항상 이 셋 중 정확히 하나로
초기화한다. 이후 내용을 move·adopt로 채워 넣을 빈 placeholder면 `init`을, Core가 소유한
buffer를 채울 거면 `init_size`를, payload가 이미 caller 소유 buffer에 있어 복사를 원하지
않는 진짜 zero-copy면 `init_data`를 쓴다.

---

## `zlink_msg_close` / `zlink_multipart_close`

Message 하나 또는 part 배열 전체의 자원을 해제한다.

```c
zlink_msg_close(&msg);

zlink_msg_t parts[4];
// ... parts로 수신 ...
zlink_multipart_close(parts, 4);
```

**Parameters.** `close`는 message pointer를 받는다. `multipart_close`는 연속된
`zlink_msg_t` 배열과 원소 개수를 받는다.

**Return과 errno.** `close`는 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. `multipart_close`는 반환값이 없다(`void`) — 각 원소에 `close`를 호출하는
편의 wrapper다.

**선택 기준.** 초기화된 message는 반드시 정확히 한 번 닫는다. Multipart message를 연속 배열로
받거나 만든 뒤 정리할 때는 직접 loop를 짜는 대신 `multipart_close`를 쓴다. `close` 뒤에는
`zlink_msg_t`가 다시 초기화되기 전까지 무효다.

---

## `zlink_msg_move` / `zlink_msg_copy` / `zlink_msg_adopt`

두 `zlink_msg_t` 인스턴스 사이에서 내용을 이전하거나 복제한다.

```c
zlink_msg_move(&dest, &src);   // src는 비게 되고, dest가 내용을 받는다
zlink_msg_copy(&dest, &src);   // 둘 다 내용을 공유한다(큰 storage는 reference count)
zlink_msg_adopt(&dest, &src);  // move와 비슷하지만 dest가 아직 message를 소유하면 안 된다
```

**Parameters.** 셋 다 `dest_`/`src_` message pointer만 받는다.

**Return과 errno.** 셋 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. 공통 `zlink_config_result_t` 값 외에 별도로 문서화된 실패는 없다.

**선택 기준.** 이미 초기화된 두 message 사이에서 내용을 넘길 때는 `move`를 쓴다 — 이후 source는
빈 초기화 message가 되고, `dest_`의 이전 내용은 해제된다. 양쪽이 같은 논리적 내용에 대한 독립
handle이 필요하면 `copy`를 쓴다 — 크거나 zero-copy인 storage는 복제하지 않고 reference count로
공유하므로 저렴하다. `adopt`는 `dest_`가 현재 초기화된 message를 소유하지 않을 때만 쓴다(주로
binding이 방금 수신한 native message의 소유권을 가져올 때) — 이미 초기화된 `dest_`에 호출하면
undefined behavior다. `move`는 그 경우도 이전 내용을 먼저 해제해 안전하게 처리한다는 점이 다르다.

---

## `zlink_msg_data` / `zlink_msg_size`

Message payload pointer와 길이를 읽는다.

```c
void *ptr = zlink_msg_data(&msg);
size_t len = zlink_msg_size(&msg);
```

**Parameters.** 둘 다 message pointer만 받는다(`data`는 non-`const`, `size`는 `const`를 받음).

**Return과 errno.** `data`는 raw payload에 대한 pointer를 반환하고, message가 초기화되지 않았으면
`NULL`이다 — `errno`는 설정하지 않는다. `size`는 payload 길이(바이트, 빈 message면 `0`)를
반환한다 — 역시 `errno`를 설정하지 않는다.

**선택 기준.** `init_size` 뒤 보내기 전에 buffer를 채우거나, 수신 뒤 도착한 내용을 읽을 때
호출한다. `data`가 반환하는 pointer는 message가 닫히거나 move되거나 전송될 때까지만 유효하다 —
그 시점을 넘겨 보관하지 않는다.

---

## `zlink_msg_refcnt`

Message의 기반 storage에 대한 현재 reference count를 읽는다.

```c
zlink_config_result_t err;
int refs = zlink_msg_refcnt(&msg, &err);
```

**Parameters.** Message pointer와 `error_out_` 출력 매개변수를 받는다.

**Return과 errno.** 성공하면 현재 reference count를 반환한다(내부적으로 reference-count되지
않는 storage 종류 — inline, borrowed-constant — 는 `1`). 실패하면 `-1`을 반환하고
`zlink_config_result_t`를 `error_out_`에 쓴다.

**선택 기준.** 이 값은 진단·assertion에만 쓰고 제어 결정에는 쓰지 않는다 — count는 atomic한
그 순간의 스냅샷이며, caller가 값을 살펴보기 전에 같은 storage를 공유하는 다른 handle에서 다른
스레드가 `copy`/`close`로 이미 바꿨을 수 있다. `copy`는 count를 atomic하게 증가시키고
`close`는 atomic하게 감소시킨다 — 이 둘은 같은 storage를 공유하는 서로 다른 handle에서 서로
다른 스레드가 동시에 호출해도 안전하지만, `refcnt` 자신을 포함한 어떤 `zlink_msg_*` 호출도 같은
handle에 대해 여러 스레드에서 동시에 호출하는 것은 안전하지 않다.

---

전체 근거는 [Message 스펙](../spec/core/02-message.ko.md)을 참고한다.
