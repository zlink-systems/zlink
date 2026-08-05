[한국어](08-utilities.ko.md) | English

[Spec Index](../README.en.md) · [Core Index](README.en.md)

# Utilities

Helper functions for atomic counters, timers, high-resolution timing, thread
management, and miscellaneous operations. These utilities complement the core
messaging API and are useful for building event loops, benchmarking, and
managing background threads.

## Atomic Counter

Atomic counters provide atomic increment, decrement, and read operations on a
shared integer. The counter is created with `zlink_atomic_counter_new` and
must be destroyed with `zlink_atomic_counter_destroy`.

### zlink_atomic_counter_new

Create a new atomic counter initialized to zero.

```c
ZLINK_EXPORT void *zlink_atomic_counter_new (void);
```

Allocates and returns an opaque handle to an atomic counter with an initial
value of zero.

**Returns:** Counter handle on success, or `NULL` on failure (out of memory).

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_atomic_counter_set`, `zlink_atomic_counter_destroy`

---

### zlink_atomic_counter_set

Set the counter to an explicit value.

```c
ZLINK_EXPORT void zlink_atomic_counter_set (void *counter_, int value_);
```

Replaces the current counter value with `value_`.

**Thread safety:** Not thread-safe. Do not call concurrently with other
operations on the same counter; typically used only during setup.

**See also:** `zlink_atomic_counter_value`

---

### zlink_atomic_counter_inc

Increment the counter by one.

```c
ZLINK_EXPORT int zlink_atomic_counter_inc (void *counter_);
```

Atomically increments the counter and returns the previous value (the value
immediately before the increment).

**Returns:** The value of the counter before the increment.

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_atomic_counter_dec`

---

### zlink_atomic_counter_dec

Decrement the counter by one.

```c
ZLINK_EXPORT int zlink_atomic_counter_dec (void *counter_);
```

Atomically decrements the counter and reports whether it is still nonzero:
returns `1` when the counter remains greater than zero after the decrement, and
`0` when it reaches zero.

**Returns:** `1` if the counter is still nonzero after the decrement, `0` if it
reached zero.

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_atomic_counter_inc`

---

### zlink_atomic_counter_value

Return the current counter value.

```c
ZLINK_EXPORT int zlink_atomic_counter_value (void *counter_);
```

Reads the current value of the counter atomically.

**Returns:** The current counter value.

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_atomic_counter_set`

---

### zlink_atomic_counter_destroy

Destroy the counter and release its memory.

```c
ZLINK_EXPORT void zlink_atomic_counter_destroy (void **counter_p_);
```

Releases the counter handle. The pointer at `*counter_p_` is set to `NULL`
after destruction.

**Thread safety:** Must not be called while other threads are operating on the
same counter.

**See also:** `zlink_atomic_counter_new`

---

## Callback Types

```c
typedef void (*zlink_timer_handler_fn) (void *timer_,
                                        uint64_t fire_count_,
                                        void *userdata_);

typedef void (zlink_thread_fn) (void *);
```

`zlink_timer_handler_fn` is the callback signature for timer fire
notifications. `timer_` is the timer handle, `fire_count_` is the cumulative
number of times the timer has fired, and `userdata_` is the pointer supplied
when the handler was attached.

`zlink_thread_fn` is the entry-point signature for threads started with
`zlink_thread_start`.

## Timers

Timers provide nanosecond-resolution periodic or one-shot scheduling. Create
a standalone generic timer with `zlink_timer_new`. Timers can be consumed synchronously with
`zlink_timer_recv` or driven by a callback with `zlink_timer_handler`. They
can also be integrated into a poller with `zlink_poller_add_timer`.

### zlink_timer_new

Create a new standalone timer.

```c
ZLINK_EXPORT void *zlink_timer_new (void);
```

Allocates and returns an opaque timer handle. Destroy with
`zlink_timer_destroy` when no longer needed.

**Returns:** Timer handle on success, or `NULL` on failure (errno is set).

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_timer_destroy`

---

### zlink_timer_destroy

Destroy a timer and release its resources.

```c
ZLINK_EXPORT zlink_close_result_t zlink_timer_destroy (void **timer_p_);
```

Stops the timer if running and frees the handle. The pointer at `*timer_p_`
is set to `NULL` after destruction.

**Returns:** `ZLINK_CLOSE_OK` on success; otherwise a `zlink_close_result_t`
value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Thread safety:** Must not be called while another thread is using the same
timer.

**See also:** `zlink_timer_new`

---

### zlink_timer_start

Start the timer with a nanosecond interval and repeat count.

```c
ZLINK_EXPORT zlink_config_result_t zlink_timer_start (void *timer_,
                                         uint64_t interval_ns_,
                                         uint64_t repeat_count_);
```

Arms the timer to fire after `interval_ns_` nanoseconds. If `repeat_count_`
is greater than zero, the timer fires that many times then stops automatically.
If `repeat_count_` is zero, the timer repeats indefinitely until explicitly
stopped.

**Parameters:**

| Name | Description |
|------|-------------|
| `timer_` | Timer handle |
| `interval_ns_` | Interval between fires in nanoseconds |
| `repeat_count_` | Number of times to fire (`0` = indefinite) |

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t`
value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Thread safety:** Must not be called concurrently with other operations on
the same timer.

**See also:** `zlink_timer_stop`

---

### zlink_timer_stop

Stop a running timer.

```c
ZLINK_EXPORT zlink_config_result_t zlink_timer_stop (void *timer_);
```

Disarms the timer. No further fire events will be generated until the timer
is started again.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t`
value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Thread safety:** Must not be called concurrently with other operations on
the same timer.

**See also:** `zlink_timer_start`

---

### zlink_timer_recv

Synchronously receive a timer fire event.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_timer_recv (void *timer_, uint64_t *fire_count_out_);
```

Waits for the timer to fire and writes the cumulative fire count into
`fire_count_out_`. This provides a synchronous, poll-style interface to the
timer.

**Parameters:**

| Name | Description |
|------|-------------|
| `timer_` | Timer handle |
| `fire_count_out_` | Pointer to receive the cumulative fire count |

**Returns:** `ZLINK_RECV_OK` on success; otherwise a `zlink_recv_result_t`
value. `ZLINK_RECV_NO_DATA` (internal `EAGAIN`) when the timer has stopped
and no fire event remains to receive. `zlink_errno()` retains the detailed
internal errno for diagnostics.

**Thread safety:** Must not be called concurrently with other operations on
the same timer.

**See also:** `zlink_timer_handler`, `zlink_timer_start`

---

### zlink_timer_handler

Attach a callback handler to the timer.

```c
ZLINK_EXPORT zlink_handler_result_t zlink_timer_handler (void *timer_,
                                            zlink_timer_handler_fn handler_,
                                            void *userdata_);
```

Registers `handler_` to be called each time the timer fires. The callback
receives the timer handle, cumulative fire count, and `userdata_`. A NULL
`handler_` is invalid and fails with `ZLINK_HANDLER_INVALID_ARGUMENT` (`EINVAL`).
After a handler is attached, `zlink_timer_recv()` on the same timer returns
`ZLINK_RECV_BUSY`.

**Parameters:**

| Name | Description |
|------|-------------|
| `timer_` | Timer handle |
| `handler_` | Callback function (must not be NULL) |
| `userdata_` | Opaque pointer passed to the callback |

**Returns:** `ZLINK_HANDLER_OK` on success; otherwise a `zlink_handler_result_t`
value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Thread safety:** Must not be called concurrently with other operations on
the same timer.

**See also:** `zlink_timer_recv`, `zlink_timer_start`

---

## Stopwatch

High-resolution timing functions for benchmarking and profiling. Start a
stopwatch, take intermediate readings, and stop it to get the total elapsed
time in microseconds.

### zlink_stopwatch_start

Start a high-resolution stopwatch.

```c
ZLINK_EXPORT void *zlink_stopwatch_start (void);
```

Captures the current time and returns an opaque handle used to measure elapsed
time. The handle must eventually be released by `zlink_stopwatch_stop`.

**Returns:** An opaque stopwatch handle on success, or `NULL` on failure.

**Thread safety:** Safe to call from any thread. The returned handle should be
used by one thread at a time.

**See also:** `zlink_stopwatch_intermediate`, `zlink_stopwatch_stop`

---

### zlink_stopwatch_intermediate

Return elapsed microseconds without stopping the stopwatch.

```c
ZLINK_EXPORT unsigned long zlink_stopwatch_intermediate (void *watch_);
```

Reads the elapsed time since `zlink_stopwatch_start` was called, without
releasing the handle. May be called multiple times to take successive
measurements.

**Returns:** Elapsed time in microseconds.

**Thread safety:** Must not be called concurrently with `zlink_stopwatch_stop`
on the same handle.

**See also:** `zlink_stopwatch_start`, `zlink_stopwatch_stop`

---

### zlink_stopwatch_stop

Stop the stopwatch and return total elapsed microseconds.

```c
ZLINK_EXPORT unsigned long zlink_stopwatch_stop (void *watch_);
```

Returns the total elapsed time since `zlink_stopwatch_start` was called and
releases the stopwatch handle. The handle must not be used after this call.

**Returns:** Elapsed time in microseconds.

**Thread safety:** Must not be called concurrently with other operations on
the same handle.

**See also:** `zlink_stopwatch_start`, `zlink_stopwatch_intermediate`

---

## Miscellaneous

### zlink_has

Check whether the current library build provides a capability.

```c
ZLINK_EXPORT bool zlink_has (const char *capability_);
```

`capability_` is a non-NULL, NUL-terminated string that the function does not
retain. `"tcp"` always returns `true`. `"ipc"`, `"tls"`, `"ws"`, and `"wss"`
return `true` only when that capability is present in the build. Any other
string returns `false`.

**Thread safety:** Does not mutate global state and may be called from any
thread.

---

### zlink_proxy

Forward multipart messages bidirectionally between two raw sockets.

```c
ZLINK_EXPORT zlink_config_result_t zlink_proxy (void *frontend_, void *backend_, void *capture_);
```

`frontend_` and `backend_` are required raw socket handles. `capture_` may be
NULL; when non-NULL, it is a raw socket that receives a copy of every forwarded
message. The call blocks its calling thread until the proxy loop ends.

All three handles are borrowed. The function neither closes nor takes ownership
of them. The proxy receives message frames and forwards them to the opposite
socket without returning frame pointers to the application.

**Returns:** `ZLINK_CONFIG_OK` when the proxy ends normally; otherwise a
`zlink_config_result_t` error. A NULL required handle or a handle that is not a
raw socket returns `ZLINK_CONFIG_INVALID_HANDLE`.

---

### zlink_proxy_steerable

Run a bidirectional proxy whose state can be controlled through a control
socket.

```c
ZLINK_EXPORT zlink_config_result_t zlink_proxy_steerable (void *frontend_,
                                                          void *backend_,
                                                          void *capture_,
                                                          void *control_);
```

`frontend_` and `backend_` are required. `capture_` and `control_` may each be
NULL. A non-NULL `control_` accepts `PAUSE`, `RESUME`, `TERMINATE`, and
`STATISTICS` commands. The call blocks until `TERMINATE`, context termination,
or an error ends the proxy loop.

Every handle is borrowed; the function neither closes nor owns it. Ownership of
the `STATISTICS` reply follows the control socket's ordinary raw send/receive
contract.

**Returns:** `ZLINK_CONFIG_OK` when the proxy ends normally; otherwise a
`zlink_config_result_t` error. A NULL required handle or a non-NULL optional
handle that is not a raw socket returns `ZLINK_CONFIG_INVALID_HANDLE`.

---

### zlink_sleep

Sleep for the given number of seconds.

```c
ZLINK_EXPORT void zlink_sleep (int seconds_);
```

Suspends the calling thread for at least `seconds_` seconds. This is a
portable convenience wrapper around platform-specific sleep functions.

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_stopwatch_start`

---

### zlink_thread_start

Start a new thread running the given function.

```c
ZLINK_EXPORT void *zlink_thread_start (zlink_thread_fn *func_, void *arg_);
```

Creates and starts a new operating-system thread that executes `func_` with
`arg_` as its sole argument. The returned handle must be passed to
`zlink_thread_join` to wait for completion and release resources.

**Returns:** An opaque thread handle on success, or `NULL` on failure.

**Thread safety:** Safe to call from any thread.

**See also:** `zlink_thread_join`

---

### zlink_thread_join

Wait for a thread to finish and release its handle.

```c
ZLINK_EXPORT void zlink_thread_join (void *thread_);
```

Blocks the calling thread until the thread identified by `thread_` has
terminated, then releases the handle. The handle must not be used after this
call.

**Thread safety:** Must be called exactly once per handle. Do not call from
the thread being joined.

**See also:** `zlink_thread_start`
