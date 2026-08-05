[한국어](02-message.ko.md) | English

[Reference index](README.en.md)

# 02. Message

This category covers the entry points a `zlink_msg_t` provides: initialization, ownership
transfer, and accessors. The public message API is a payload-part container only — it exposes
no request-reply or per-message metadata operations; those belong to the socket contracts (see
the Socket lifecycle and per-socket-type categories). The exact signatures are owned by the
[Message specification](../spec/core/02-message.en.md).

---

## `zlink_msg_init` / `zlink_msg_init_size` / `zlink_msg_init_data`

Initializes a `zlink_msg_t` before its first use — empty, sized with an uninitialized buffer, or
wrapping an external buffer with zero-copy semantics.

```c
zlink_msg_t empty;
zlink_msg_init(&empty);

zlink_msg_t sized;
zlink_msg_init_size(&sized, 128);
memcpy(zlink_msg_data(&sized), payload, 128);

zlink_msg_t zero_copy;
zlink_msg_init_data(&zero_copy, buffer, buffer_len, free_callback, hint);
```

**Parameters.** `init` takes only the message pointer. `init_size` adds `size_` (bytes to
allocate; contents are uninitialized). `init_data` adds `data_`/`size_` (the caller-owned
buffer), `ffn_` (a `zlink_free_fn` invoked when the library no longer needs the buffer, or `NULL`
if the caller guarantees the buffer outlives the message), and `hint_` (passed through to
`ffn_`).

**Return and errno.** All three return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.
`init_size` fails with `ENOMEM` if allocation fails.

**When to use.** Always initialize a `zlink_msg_t` with exactly one of these before passing it to
any other message function. Use `init` for an empty placeholder you will move/adopt content
into, `init_size` when you will fill a buffer Core owns, and `init_data` for true zero-copy —
where the payload already lives in a buffer you control and don't want a second copy.

---

## `zlink_msg_close` / `zlink_multipart_close`

Releases a message's resources, individually or across a whole part array.

```c
zlink_msg_close(&msg);

zlink_msg_t parts[4];
// ... received into parts ...
zlink_multipart_close(parts, 4);
```

**Parameters.** `close` takes the message pointer. `multipart_close` takes a contiguous
`zlink_msg_t` array and its element count.

**Return and errno.** `close` returns `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.
`multipart_close` returns nothing (`void`) — it is a convenience wrapper that calls `close` on
each element.

**When to use.** Every initialized message must be closed exactly once. Use `multipart_close` as
the cleanup step after receiving or constructing a multipart message stored as a contiguous
array, instead of a hand-written loop. After `close`, the `zlink_msg_t` is invalid until
re-initialized.

---

## `zlink_msg_move` / `zlink_msg_copy` / `zlink_msg_adopt`

Transfers or duplicates message content between two `zlink_msg_t` instances.

```c
zlink_msg_move(&dest, &src);   // src becomes empty; dest gets the content
zlink_msg_copy(&dest, &src);   // both now share the content (refcounted for large storage)
zlink_msg_adopt(&dest, &src);  // like move, but dest must not already own a message
```

**Parameters.** All three take only `dest_`/`src_` message pointers.

**Return and errno.** All three return `zlink_config_result_t` — `ZLINK_CONFIG_OK` on success.
None documents a distinct failure beyond the common `zlink_config_result_t` values.

**When to use.** Use `move` to hand content off between two already-initialized messages — the
source becomes an empty initialized message afterward, and any previous content of `dest_` is
released. Use `copy` when both sides need independent handles to the same logical content; large
or zero-copy storage is shared by reference count rather than duplicated, so this is cheap. Use
`adopt` only when `dest_` does not currently own an initialized message (typically inside a
binding taking ownership of a freshly received native message) — calling it on an
already-initialized `dest_` is undefined behavior, unlike `move`, which handles that case safely
by releasing the previous content first.

---

## `zlink_msg_data` / `zlink_msg_size`

Reads the message payload pointer and its length.

```c
void *ptr = zlink_msg_data(&msg);
size_t len = zlink_msg_size(&msg);
```

**Parameters.** Both take only the message pointer (`data` takes non-`const`, `size` takes
`const`).

**Return and errno.** `data` returns a pointer to the raw payload, or `NULL` if the message is
uninitialized — no `errno` is set. `size` returns the payload length in bytes (`0` for an empty
message) — also no `errno`.

**When to use.** Call these after `init_size` to populate the buffer before sending, or after a
receive to read what arrived. The pointer `data` returns stays valid only until the message is
closed, moved, or sent — do not retain it past that point.

---

## `zlink_msg_refcnt`

Reads the current reference count of a message's underlying storage.

```c
zlink_config_result_t err;
int refs = zlink_msg_refcnt(&msg, &err);
```

**Parameters.** Takes the message pointer and an `error_out_` output parameter.

**Return and errno.** Returns the current reference count on success (`1` for storage kinds that
are not internally reference-counted, such as inline or borrowed-constant messages), or `-1` on
failure with `zlink_config_result_t` written through `error_out_`.

**When to use.** Use this only for diagnostics and assertions, not for control decisions — the
count is an atomic point-in-time snapshot, and another thread may change it via `copy`/`close` on
a different handle sharing the same storage before the caller acts on the value. `copy`
atomically increments the count and `close` atomically decrements it; those two operations are
safe to call concurrently from different threads on different handles sharing storage, but no
`zlink_msg_*` call — including `refcnt` itself — is safe to make concurrently on the *same*
handle from multiple threads.

---

See the [Message specification](../spec/core/02-message.en.md) for the full rationale.
