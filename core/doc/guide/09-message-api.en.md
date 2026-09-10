
<!-- zlink-nav:start -->
[← Design Rationale — Why It Was Built This Way](design-rationale.en.md) | [Thread safety →](11-thread-safety.en.md)
<!-- zlink-nav:end -->

# Message API and ownership

`zlink_msg_t` owns one message part. Initialize it before use and close it once
unless ownership was moved or the part was consumed by a send. A whole-message
send consumes every array slot on both success and failure; each consumed slot
is left empty and initialized and can be closed or reused as is.

## Create a part

- `zlink_msg_init()` creates an empty part.
- `zlink_msg_init_size()` allocates writable storage.
- `zlink_msg_init_data()` wraps caller-provided data with a release callback.
- `zlink_msg_copy()` shares message storage; `zlink_msg_move()` transfers it.

## Multipart send

Place all parts in array order and send the complete record with one
`zlink_send()` call. The array contents are consumed on both success and
failure, so copy the complete record before the call if it may need to be sent
again.

```c
zlink_msg_t parts[1];
zlink_msg_init_size(&parts[0], payload_size);
memcpy(zlink_msg_data(&parts[0]), payload, payload_size);
/* A send consumes the full array on success and failure; the slot is empty afterwards. */
zlink_send(socket, parts, 1, ZLINK_SEND_FLAGS_NONE, NULL, NULL);
```

## Receive

Typed receive functions fill a caller-provided `zlink_msg_t` array with a
complete record and return its part count. The slots need not be initialized.
Close a successful array with `zlink_multipart_close()` or move each part
exactly once. Routing ids and topics are returned as metadata rather than
payload frames.

For a successful REQUEST completion, `zlink_completion_recv()` transfers a
Core-owned contiguous reply array into `zlink_completion_t`. Read or move its
parts, then call `zlink_completion_close()`; never free the array directly.
