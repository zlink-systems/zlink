
<!-- zlink-nav:start -->
[← Design Rationale — Why It Was Built This Way](design-rationale.en.md) | [Thread safety →](11-thread-safety.en.md)
<!-- zlink-nav:end -->

# Message API and ownership

`zlink_msg_t` owns one message part. Initialize it before use and close it once
unless ownership was moved or the part was consumed by a send. A `*_part` send
consumes the part on both success and failure; the consumed part is left in the
empty initialized state and can be closed or reused as is.

## Create a part

- `zlink_msg_init()` creates an empty part.
- `zlink_msg_init_size()` allocates writable storage.
- `zlink_msg_init_data()` wraps caller-provided data with a release callback.
- `zlink_msg_copy()` shares message storage; `zlink_msg_move()` transfers it.

## Multipart send

Send each part with the typed part API. Use `ZLINK_PART_MORE` until the final
part and `ZLINK_PART_FINAL` for the last part. The content of a part consumed
by a send is gone on both success and failure, so copy it before the call if
the same content must be sent again.

```c
zlink_msg_t part;
zlink_msg_init_size(&part, payload_size);
memcpy(zlink_msg_data(&part), payload, payload_size);
/* A send consumes the part on success and failure — the part is empty afterwards. */
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
