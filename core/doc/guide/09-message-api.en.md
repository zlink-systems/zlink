
# Message API and ownership

`zlink_msg_t` owns one message part. Initialize it before use and close it once
unless ownership was moved or consumed by a successful send.

## Create a part

- `zlink_msg_init()` creates an empty part.
- `zlink_msg_init_size()` allocates writable storage.
- `zlink_msg_init_data()` wraps caller-provided data with a release callback.
- `zlink_msg_copy()` shares message storage; `zlink_msg_move()` transfers it.

## Multipart send

Send each part with the typed part API. Use `ZLINK_PART_MORE` until the final
part and `ZLINK_PART_FINAL` for the last part. Do not reuse a part after a
successful send consumes it.

```c
zlink_msg_t part;
zlink_msg_init_size(&part, payload_size);
memcpy(zlink_msg_data(&part), payload, payload_size);
/* The successful final send transfers ownership to Core. */
zlink_send_part(socket, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                NULL, NULL);
```

## Receive

Typed receive functions fill a caller-initialized `zlink_msg_t` and report
whether another part follows. Close or move every received part exactly once.
Routing ids and topics are returned as metadata rather than payload frames.

For a successful REQUEST completion, `zlink_completion_recv()` transfers a
Core-owned contiguous reply array into `zlink_completion_t`. Read or move its
parts, then call `zlink_completion_close()`; never free the array directly.
