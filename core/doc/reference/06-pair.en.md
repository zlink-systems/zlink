[한국어](06-pair.ko.md) | English

[Reference index](README.en.md)

# 06. PAIR

A 1:1 bidirectional raw socket type. PAIR has no type-specific options and no dedicated receive
function — it shares `zlink_recv_part` (Raw receive category) and `zlink_send_ready_handler`
(Socket lifecycle category) with other socket types. Its one type-specific entry is the send
side. The exact signatures are owned by the [PAIR specification](../spec/core/socket/01-pair.en.md).

---

## `zlink_send_part`

Sends one message part on a PAIR (also DEALER — see the DEALER category) socket.

```c
zlink_msg_t part;
zlink_msg_init_size(&part, payload_len);
memcpy(zlink_msg_data(&part), payload, payload_len);
zlink_submit_result_t result = zlink_send_part(s, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `part_` is the message to send — its content is consumed on both success and
failure (copy it first if it must be sent again; re-initialize it before reuse). `flags_` is
`ZLINK_SEND_FLAGS_NONE` or `ZLINK_DONTWAIT`. `part_flag_` is `ZLINK_PART_FINAL` for a
single-part message, or `ZLINK_PART_MORE` to start a multipart sequence continued by further
calls to this same function on the same thread through a final `ZLINK_PART_FINAL` call.

**Return and errno.** Returns `zlink_submit_result_t` — `ZLINK_SUBMIT_OK` on success (see the
Errors, results, and version category for the full result mapping). `ZLINK_SUBMIT_BACKPRESSURED`
for a non-blocking call that can't proceed immediately.

**When to use.** Core stages successful intermediate parts of a multipart sequence as one record
until the final part succeeds; if any part in the open sequence fails, Core atomically discards
every staged part along with the failed one, and no part of that record becomes visible to the
peer. Because the failed call still consumes its `part_`, retry the entire record from its first
part using copies you retained before the original calls — don't call a different send helper or
switch flags partway through one sequence.

---

See the [PAIR specification](../spec/core/socket/01-pair.en.md) for the full rationale. Receive
uses `zlink_recv_part` (Raw receive category); send-ready notification uses
`zlink_send_ready_handler` (Socket lifecycle category) — neither is repeated here.
