---
title: "Socket — Common Specification"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/socket/) | English

<!-- zlink-nav:start -->
[Core Spec Index](../README.en.md) | [Previous: Runtime Boundary](../08-runtime-boundary.en.md) | [Next: PAIR](01-pair.en.md)
<!-- zlink-nav:end -->

# Socket — Common Specification

> **What this chapter defines** — the public contract for the common
> foundations (options and API forms) that apply to every socket type.
> Each socket specification defines its type-specific details.

## 1. Socket overview

A zlink [socket](../glossary.en.md#socket) is an endpoint that sends and
receives messages, and it always belongs to a
[Context](../glossary.en.md#context). This document covers the common
foundations shared by every socket type: creation, connection, termination,
common options, the form of send and receive APIs, and thread safety.
Separate files define each type's type-specific options, data-plane APIs, and
behavioral details.

| Socket Type | Spec |
|-------------|------|
| 01. PAIR | [pair.md](01-pair.en.md) |
| 02. PUB | [pub.md](02-pub.en.md) |
| 03. SUB | [sub.md](03-sub.en.md) |
| 04. XPUB | [xpub.md](04-xpub.en.md) |
| 05. XSUB | [xsub.md](05-xsub.en.md) |
| 06. DEALER | [dealer.md](06-dealer.en.md) |
| 07. ROUTER | [router.md](07-router.en.md) |
| 08. STREAM | [stream.md](08-stream.en.md) |

The following documents own the related contracts.

| Related contract | Owning document |
|---|---|
| Type-specific options, data plane, and behavioral details | Each socket specification in the table above |
| Context lifetime and context options | [Context](../01-context.en.md) |
| Message lifecycle and ownership | [Message](../02-message.en.md) |
| Auto HWM budget calculation and admission | [Auto HWM](../systems/06-auto-hwm.en.md) |
| Complete result enums and error tables | [Errors](../03-errors.en.md) |

## 2. Thread safety

Public socket handle APIs are thread-safe by default. Not every API has the
same cost model, though.

- `send` is a hot-path API and can be called concurrently from multiple threads.
- `bind/connect/disconnect`, subscribe/unsubscribe, option/query, and monitor
  operations are valid runtime control-path calls. Correctness is preserved,
  but execution order may follow internal serialization.
- `close` uses a fail-fast lifecycle gate. If another thread is running an
  admitted API on the same handle, close fails with `EBUSY`. Once close is
  accepted, new API entry fails with `ESHUTDOWN`.
- Only a small set of exceptions remain outside the default allowance:
  init-only configuration and concurrent sharing of the same `zlink_msg_t`
  instance.

## 3. Pull receive and completion model

Core exposes work to the application through poller readiness and pull receive. Core does not invoke
application notification callbacks.

| Content | Readiness | Function that removes it |
|---|---|---|
| Ordinary DATA | `ZLINK_POLLIN` | Socket-specific `*_recv_part()` |
| STREAM packet | `ZLINK_POLLIN` | `zlink_stream_recv_packet()` |
| SEND and REQUEST completion | `ZLINK_POLLCOMPLETION` | `zlink_completion_recv()` |
| Socket monitor event | `ZLINK_POLLIN` | `zlink_socket_monitor_recv()` |
| Timer fire count | Timer readiness | `zlink_timer_recv()` |

Ordinary DATA receive functions are divided as follows.

| Function | Socket and record |
|---|---|
| `zlink_recv_part()` | PAIR and DEALER DATA; RAW-mode STREAM byte records |
| `zlink_router_recv_part()` | ROUTER DATA or REQUEST |
| `zlink_subscribe_part()` | SUB and XSUB topic DATA |
| `zlink_xpub_recv_part()` | XPUB subscribe and unsubscribe events |

`ZLINK_POLLCOMPLETION` is not payload. Poller wait does not remove completions or add operation
payload to `zlink_poller_event_t`. For each ready socket, the caller invokes
`zlink_completion_recv(..., ZLINK_RECV_FLAGS_DONTWAIT)` until `ZLINK_RECV_NO_DATA` drains the queue.
`zlink_free_fn` releases zero-copy memory, and `zlink_thread_fn` is a user-thread entry type rather
than an application notification.

## 4. Types and constants

### Socket Types

```c
typedef enum zlink_socket_type_t
{
    ZLINK_SOCKET_ANY    = 0,       // Reserved wildcard value; not for creation and consumed by no API
    ZLINK_SOCKET_PAIR   = 0x1001,
    ZLINK_SOCKET_PUB    = 0x1002,
    ZLINK_SOCKET_SUB    = 0x1003,
    ZLINK_SOCKET_DEALER = 0x1004,
    ZLINK_SOCKET_ROUTER = 0x1005,
    ZLINK_SOCKET_XPUB   = 0x1006,
    ZLINK_SOCKET_XSUB   = 0x1007,
    ZLINK_SOCKET_STREAM = 0x1008
} zlink_socket_type_t;
```

`ZLINK_SOCKET_ANY` is a reserved wildcard value. It is not used to create a
socket, and no API consumes it. Use the normalized `ZLINK_SOCKET_*` constants
shown above to create an actual socket.

### Send Flags

```c
typedef enum zlink_send_flags_t
{
    ZLINK_SEND_FLAGS_NONE     = 0,       // No flags; blocking send behavior
    ZLINK_SEND_FLAGS_DONTWAIT = 0x0001u  // Non-blocking; return ZLINK_SUBMIT_BACKPRESSURED if it would block
} zlink_send_flags_t;

#define ZLINK_DONTWAIT ZLINK_SEND_FLAGS_DONTWAIT  // Short public name
```

### Recv Flags

```c
typedef enum zlink_recv_flags_t
{
    ZLINK_RECV_FLAGS_NONE     = 0,       // No flags; blocking receive behavior
    ZLINK_RECV_FLAGS_DONTWAIT = 0x0001u  // Non-blocking receive; return ZLINK_RECV_NO_DATA immediately when no message is available
} zlink_recv_flags_t;
```

Used by `zlink_recv_part`, `zlink_subscribe_part`, the socket-specific
`zlink_*_recv_part` family, and the monitor `zlink_*_monitor_recv` functions.

### Message part flag

```c
typedef enum zlink_part_flag_t
{
    ZLINK_PART_FINAL = 0,  // The current part is the last part
    ZLINK_PART_MORE = 1    // Another part follows in the same multipart message
} zlink_part_flag_t;
```

### Routing ID duplicate policy

```c
typedef enum zlink_rid_duplicate_policy_t
{
    ZLINK_RID_DUPLICATE_REJECT = 0,   // Keep the existing pipe and do not register the new duplicate pipe (default)
    ZLINK_RID_DUPLICATE_HANDOVER = 1  // A reconnecting pipe in the same direction takes over the existing pipe
} zlink_rid_duplicate_policy_t;
```

`ZLINK_OPT_RID_DUPLICATE_POLICY` controls what happens when a local socket
observes another peer with the same routing id. The option value is an
`int`; the default is `ZLINK_RID_DUPLICATE_REJECT`.

`ZLINK_RID_DUPLICATE_REJECT` keeps the existing pipe and does not register the
new duplicate pipe. Under `ZLINK_RID_DUPLICATE_HANDOVER`, a reconnecting pipe
in the same direction takes over the existing pipe. If pipes in opposite
directions collide, both peers compare their routing IDs and choose the same
single direction.

This option is meaningful only for sockets that can observe a peer-advertised
routing id. STREAM assigns its own 4-byte connection routing ids, so this
option does not affect STREAM.

### Submit retry mode

```c
typedef enum zlink_submit_retry_mode_t
{
    ZLINK_SUBMIT_RETRY_OFF = 0,           // Do not retry automatically
    ZLINK_SUBMIT_RETRY_LOCAL_FAILURE = 1  // Retry only local failures before handoff to the peer queue
} zlink_submit_retry_mode_t;
```

`ZLINK_SUBMIT_RETRY_OFF` disables automatic retry.
`ZLINK_SUBMIT_RETRY_LOCAL_FAILURE` permits retry only for a local failure that
occurs before the send is handed to a peer queue. This mode does not guarantee
peer delivery or processing. [Send retry in §5 Options](#send-retry) describes
retry eligibility and results.

### Receive flow state

```c
typedef enum zlink_receive_flow_state_t
{
    ZLINK_RECEIVE_FLOW_RUNNING = 0,  // Tell peers to keep sending
    ZLINK_RECEIVE_FLOW_PAUSED = 1    // Tell peers not to send new messages to this socket
} zlink_receive_flow_state_t;
```

This is the receive-flow state a DEALER or ROUTER socket publishes to peers
that send to it over the paired
[completion progress lane](../glossary.en.md#completion-progress-lane).
`ZLINK_RECEIVE_FLOW_RUNNING`
asks those peers to keep sending; `ZLINK_RECEIVE_FLOW_PAUSED` asks them to stop
sending new messages to this socket. The value is an absolute socket-wide
state, not a counter, so setting the state a socket already holds changes
nothing and succeeds. Only DEALER and ROUTER have this lane;
[DEALER](06-dealer.en.md) and [ROUTER](07-router.en.md) own the resulting
behavior.

### Send result

```c
typedef enum zlink_submit_result_t
{
    /* Submit succeeded. */
    ZLINK_SUBMIT_OK = 0,                 // The message was sent successfully

    /* Normal control-flow result. */
    ZLINK_SUBMIT_BACKPRESSURED = 1,      // The send queue is full (HWM reached)
    ZLINK_SUBMIT_NOT_CONNECTED = 2,      // The target path or peer is not connected yet
    ZLINK_SUBMIT_NOT_FOUND = 3,          // The target peer or routed destination was not found
    ZLINK_SUBMIT_NOT_ADMITTED = 13,      // The target route was identified, but admission policy rejected the submit

    /* Runtime / lifecycle failure. */
    ZLINK_SUBMIT_TERMINATED = 4,         // The context was terminated

    /* Caller contract violation. */
    ZLINK_SUBMIT_INVALID_HANDLE = 5,     // The handle is NULL or invalid
    ZLINK_SUBMIT_INVALID_ARGUMENT = 6,   // An argument violates the API contract
    ZLINK_SUBMIT_NOT_SUPPORTED = 7,      // The operation or flags are not supported
    ZLINK_SUBMIT_INVALID_STATE = 8,      // The handle is in an invalid state
    ZLINK_SUBMIT_THREAD_VIOLATION = 9,   // The allowed thread model was violated

    /* Internal failure. */
    ZLINK_SUBMIT_OUT_OF_MEMORY = 10,     // Memory allocation failed while preparing the submit
    ZLINK_SUBMIT_SEQ_EXHAUSTED = 11,     // Request sequence space was exhausted
    ZLINK_SUBMIT_INTERNAL_ERROR = 12     // Internal send/request/reply submit error
} zlink_submit_result_t;
```

Used as the canonical normalized submit outcome for send, request submit,
and reply submit APIs. Exported C APIs return this enum directly. Internal
implementation paths still use detailed `errno`, and exported API
boundaries normalize those values into this public contract.

### Completion result and record

```c
typedef enum zlink_request_result_t
{
    /* Reply completed successfully. */
    ZLINK_REQUEST_OK = 0,                  // Reply payload was received successfully

    /* Completion failure visible to the requester. */
    ZLINK_REQUEST_TIMED_OUT       = 101,   // No reply arrived within the configured time
    ZLINK_REQUEST_NOT_FOUND       = 102,   // The target was absent and the request completed with an error reply
    ZLINK_REQUEST_TERMINATED      = 103,   // Context or socket ended before a terminal reply (ETERM or ESHUTDOWN)
    ZLINK_REQUEST_PROTOCOL_ERROR  = 104,   // The reply metadata or error-reply payload was malformed
    ZLINK_REQUEST_INTERNAL_ERROR  = 105,   // Completion failed without a more specific public bucket
    ZLINK_REQUEST_REJECTED        = 106,   // The target explicitly rejected the request
    ZLINK_REQUEST_CONFLICT        = 107,   // The request conflicts with current routing or operation state
    ZLINK_REQUEST_BUSY            = 108,   // The target is busy and cannot accept the request now
    ZLINK_REQUEST_NOT_CONNECTED   = 109,   // There is no active connection to the target
    ZLINK_REQUEST_INVALID_ARGUMENT = 110,  // The request contains an invalid argument
    ZLINK_REQUEST_INVALID_STATE   = 111,   // The target is in a state that rejects this request
    ZLINK_REQUEST_NOT_SUPPORTED   = 112,   // The target does not support the operation
    ZLINK_REQUEST_BACKPRESSURED   = 113    // Non-blocking outbound admission lacked capacity
} zlink_request_result_t;
```

Used as the canonical normalized result for REQUEST completion.

```c
typedef uint64_t zlink_completion_id_t;
typedef uint64_t zlink_reply_token_t;  // DATA is 0; REQUEST is nonzero

typedef enum zlink_completion_kind_t {
  ZLINK_COMPLETION_SEND = 1,     // Deferred admission result of a DONTWAIT send
  ZLINK_COMPLETION_REQUEST = 2   // Reply, timeout, or terminal result of a request
} zlink_completion_kind_t;

typedef enum zlink_send_complete_result_t {
  ZLINK_SEND_ADMITTED = 0,       // Local send-queue admission completed
  ZLINK_SEND_TERMINAL = 202      // send_terminal_errno contains the terminal cause
} zlink_send_complete_result_t;

typedef struct zlink_completion_t {
  uint32_t struct_size;                     // sizeof(zlink_completion_t)
  zlink_completion_kind_t kind;             // SEND or REQUEST
  zlink_completion_id_t completion_id;      // Socket-local and always nonzero
  void *user_context;                       // Returned unchanged from submit
  zlink_routing_id_t peer_rid;              // Logical peer; empty when not applicable
  zlink_send_complete_result_t send_result; // Used only for SEND
  int send_terminal_errno;                  // Used only for SEND TERMINAL
  zlink_request_result_t request_result;    // Used only for REQUEST
  zlink_msg_t *reply_parts;                  // REQUEST payload; NULL when absent
  size_t reply_part_count;                   // Number of REQUEST payload parts
} zlink_completion_t;
```

SEND and REQUEST share the socket-local completion ID. Zero means either that admission already
completed or that Core did not accept the operation, so no later completion exists. A nonzero ID is
not reused before socket close and is not a cancellation handle. If Core cannot produce the next
nonzero ID, submit fails with `ZLINK_SUBMIT_SEQ_EXHAUSTED`, `errno == EOVERFLOW`, and ID `0`.

### Security Mechanisms

```c
#define ZLINK_NULL 0   // No security mechanism (default)
#define ZLINK_PLAIN 1  // PLAIN username/password authentication
```

## 5. Options

Socket options use type-specific enums and functions. Common options use
`zlink_set_option()` / `zlink_get_option()`, while socket-type-specific
options use dedicated functions such as
`zlink_set_router_option()`, `zlink_set_dealer_option()`,
`zlink_set_pub_option()`, `zlink_set_sub_option()`, and
`zlink_set_stream_option()`. ROUTING_ID uses the dedicated
`zlink_set_routing_id()` / `zlink_get_routing_id()` functions. Standard TLS
server/client role configuration uses `zlink_set_tls_server()` /
`zlink_set_tls_client()`, while `ZLINK_OPT_TLS_*` configures or queries
individual TLS values only on supported raw network sockets.
SUBSCRIBE/UNSUBSCRIBE uses `zlink_set_subscription()` /
`zlink_unset_subscription()`.

### Common options (`zlink_option_t`)

```c
typedef enum zlink_option_t {
  ZLINK_OPT_AFFINITY                  = 0x3001,  // I/O thread affinity bitmask (uint64_t)
  ZLINK_OPT_RATE                      = 0x3003,  // Multicast data rate (kbps, int)
  ZLINK_OPT_RECOVERY_IVL              = 0x3004,  // Multicast recovery interval (ms, int)
  ZLINK_OPT_SNDBUF                    = 0x3005,  // Kernel send-buffer size (int; -1=keep OS default, >=0=request size from OS)
  ZLINK_OPT_RCVBUF                    = 0x3006,  // Kernel receive-buffer size (int; -1=keep OS default, >=0=request size from OS)
  ZLINK_OPT_FD                        = 0x3007,  // File descriptor (zlink_fd_t, read-only)
  ZLINK_OPT_EVENTS                    = 0x3008,  // Event-state bitmask (int, read-only)
  ZLINK_OPT_TYPE                      = 0x3009,  // Socket type (int, read-only)
  ZLINK_OPT_LINGER                    = 0x300A,  // Shutdown wait (ms, int; -1=infinite, 0=immediate)
  ZLINK_OPT_RECONNECT_IVL             = 0x300B,  // Initial reconnect interval (ms, int)
  ZLINK_OPT_BACKLOG                   = 0x300C,  // Listener backlog (int)
  ZLINK_OPT_RECONNECT_IVL_MAX         = 0x300D,  // Maximum reconnect interval (ms, int; 0=use IVL only)
  ZLINK_OPT_MAXMSGSIZE                = 0x300E,  // Maximum inbound message size (int64_t; positive=limit, nonpositive=unlimited, default -1)
  ZLINK_OPT_SNDHWM                    = 0x300F,  // Accounted-byte HWM for a directional send pipe (uint64_t; default 4,096,000, 0=unlimited)
  ZLINK_OPT_RCVHWM                    = 0x3010,  // Accounted-byte HWM for a directional receive pipe (uint64_t; default 4,096,000, 0=unlimited)
  ZLINK_OPT_MULTICAST_HOPS            = 0x3011,  // Multicast TTL (int)
  ZLINK_OPT_RCVTIMEO                  = 0x3012,  // Receive timeout (ms, int; default 1000; explicitly setting -1 means infinite)
  ZLINK_OPT_SNDTIMEO                  = 0x3013,  // Send timeout (ms, int; default 1000; explicitly setting -1 means infinite)
  ZLINK_OPT_LAST_ENDPOINT             = 0x3014,  // Bound endpoint (string, read-only)
  ZLINK_OPT_TCP_KEEPALIVE             = 0x3015,  // SO_KEEPALIVE (int; -1=OS, 0=off, 1=on)
  ZLINK_OPT_TCP_KEEPALIVE_CNT         = 0x3016,  // TCP_KEEPCNT (int; -1=OS default)
  ZLINK_OPT_TCP_KEEPALIVE_IDLE        = 0x3017,  // TCP_KEEPIDLE (seconds, int; -1=OS default)
  ZLINK_OPT_TCP_KEEPALIVE_INTVL       = 0x3018,  // TCP_KEEPINTVL (seconds, int; -1=OS default)
  ZLINK_OPT_IMMEDIATE                 = 0x3019,  // Queue messages only to completed connections (int)
  ZLINK_OPT_IPV6                      = 0x301A,  // Enable IPv6 on the socket (int; 0=off, positive=on, getter returns 0/1)
  ZLINK_OPT_CONFLATE                  = 0x301B,  // PUB/SUB keep only the latest message per topic (int; DEALER cannot enable it)
  ZLINK_OPT_TOS                       = 0x301C,  // IP Type-of-Service value (int)
  ZLINK_OPT_HANDSHAKE_IVL             = 0x301D,  // ZMTP handshake timeout (ms, int)
  ZLINK_OPT_BLOCKY                    = 0x301E,  // Identifier unsupported by the socket option API; see below
  ZLINK_OPT_INVERT_MATCHING           = 0x3020,  // Invert topic matching (int)
  ZLINK_OPT_CONNECT_TIMEOUT           = 0x3024,  // Connection timeout (ms, int)
  ZLINK_OPT_TCP_MAXRT                 = 0x3025,  // Maximum TCP retransmission timeout (ms, int)
  ZLINK_OPT_MULTICAST_MAXTPDU         = 0x3026,  // Maximum multicast TPDU size (int)
  ZLINK_OPT_BINDTODEVICE              = 0x3027,  // Network-interface binding (string)
  ZLINK_OPT_TLS_CERT                   = 0x3028,  // Path to a PEM-encoded TLS certificate (string)
  ZLINK_OPT_TLS_KEY                    = 0x3029,  // Path to a PEM-encoded TLS private key (string)
  ZLINK_OPT_TLS_CA                     = 0x302A,  // Path to a PEM-encoded CA certificate bundle (string)
  ZLINK_OPT_TLS_VERIFY                 = 0x302B,  // Enable TLS peer verification (int; 0=off, positive=on, getter returns 0/1)
  ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT    = 0x302C,  // Require a client certificate (int; 0=off, positive=on, getter returns 0/1)
  ZLINK_OPT_TLS_HOSTNAME               = 0x302D,  // Hostname for SNI and certificate verification (string)
  ZLINK_OPT_TLS_TRUST_SYSTEM           = 0x302E,  // Trust the system CA certificate store (int; 0=off, positive=on, getter returns 0/1)
  ZLINK_OPT_TLS_PASSWORD               = 0x302F,  // Private-key password (string)
  ZLINK_OPT_ZMP_METADATA               = 0x3030,  // Enable or disable attached ZMP metadata (int; 0=off, positive=on, getter returns 0/1)
  ZLINK_OPT_TCP_NODELAY                = 0x3031,  // Enable TCP_NODELAY (int; 0=off, positive=on, getter returns 0/1)
  ZLINK_OPT_RID_DUPLICATE_POLICY       = 0x3033,  // Peer routing-ID duplicate policy (int; default REJECT; see §4)
  ZLINK_OPT_SUBMIT_RETRY_MODE          = 0x3037,  // Local submit-failure retry mode (int; ZLINK_SUBMIT_RETRY_OFF or ZLINK_SUBMIT_RETRY_LOCAL_FAILURE; raw socket default off)
  ZLINK_OPT_SUBMIT_RETRY_TIMEOUT       = 0x3038,  // Local submit-failure retry budget (ms, int; raw socket default 0, 0 disables retry)
  ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS      = 0x3039,  // Additional retry attempts after the first submit (int; raw socket default 0, current maximum 16)
  ZLINK_OPT_PENDING_MAX_MSGS           = 0x303A,  // Shared DONTWAIT SEND/REQUEST pending-record limit (uint64_t, 0 unlimited, default 0)
  ZLINK_OPT_PENDING_MAX_BYTES          = 0x303B   // Shared DONTWAIT SEND/REQUEST pending-byte limit (uint64_t, 0 unlimited, default 0)
} zlink_option_t;
```

These options are used with `zlink_set_option()` / `zlink_get_option()` and
apply to raw sockets and discovery.

`ZLINK_OPT_PENDING_MAX_MSGS` and `ZLINK_OPT_PENDING_MAX_BYTES` are shared by
DONTWAIT SEND and REQUEST operations whose payload Core retains because it
cannot admit them immediately. `MAX_MSGS` counts complete multipart records.
`MAX_BYTES` sums `max(payload size, sizeof(zlink_msg_t))` for every part and
saturates the sum at `UINT64_MAX` on overflow. No reservation occurs while
`MORE` parts are staged; `FINAL` atomically checks and reserves the entire
charge. The count and byte charge are released when a pending payload is
admitted or reaches a terminal result. Lowering a limit at runtime does not
remove existing reservations; the new value applies only to later
reservations. Immediate admission and `NONE` waits do not use this pool.

Only PAIR, DEALER, ROUTER, and STREAM support these two options. Getting or
setting either option on another socket fails with
`ZLINK_CONFIG_NOT_SUPPORTED` and `errno == ENOTSUP` without changing existing
option state.

`ZLINK_OPT_BLOCKY` is an identifier unsupported by the socket option API.
`zlink_set_option()` / `zlink_get_option()` return
`ZLINK_CONFIG_NOT_SUPPORTED` / `ENOTSUP`. Configure context-termination
behavior with `ZLINK_CTX_OPT_BLOCKY` (`int`; 0=off, positive=on, getter returns
0/1).

#### Conflation

`ZLINK_OPT_CONFLATE` remains enabled and queryable as `1` on PUB and SUB. On DEALER, setting it to
`1` returns `ZLINK_CONFIG_NOT_SUPPORTED` with `ENOTSUP`, setting it to `0` succeeds as a no-op, and
the getter returns `0`.

DEALER carries Application records and internal protocol controls on the same Application pipe.
Frame-level conflation cannot preserve both classes: replacing one frame can lose either the latest
Application record or a required control. DEALER therefore does not provide partial conflation.

#### Transport/Buffer

The two [HWM](../glossary.en.md#hwm) `uint64_t` options
(`ZLINK_OPT_SNDHWM`, `ZLINK_OPT_RCVHWM`) require exactly
`sizeof(uint64_t)` bytes in
`zlink_set_option()` and `zlink_get_option()`. A four-byte value is rejected
with `ZLINK_CONFIG_INVALID_ARGUMENT`. The unsupported socket option value
`0x3034` is also unknown and fails with `ZLINK_CONFIG_INVALID_ARGUMENT` and `EINVAL`.
Pipe admission accounts the actual retained bytes.

HWM is applied to each HWM-controlled application directional pipe. The
DEALER/ROUTER completion progress lane carries only terminal replies and error
replies and applies no automatic HWM, manual `SNDHWM` or `RCVHWM`, LWM, or Core
budget reservation. Once the accounted bytes reach the
limit, further writes wait until the receiver returns enough byte credit. This
limiting behavior is [backpressure](../glossary.en.md#backpressure). An
empty pipe may admit one message whose accounted size is larger than its HWM,
so a finite HWM does not reject every legal large message. The message must
still satisfy `ZLINK_OPT_MAXMSGSIZE`. This exception admits at most one such
message before further writes wait. Even when `ZLINK_OPT_MAXMSGSIZE` is
unlimited, the exception applies only to one complete message whose total
accounted size is known at admission: a single-part or total-known message. An
incremental multipart whose final total is unknown follows the ordinary byte
HWM from its first `MORE` frame, so frames cannot accumulate without a bound.
Core adds neither known-total metadata nor a whole-transaction reservation for
this exception.

Admission charges one frame at a time. An ordinary frame is charged its
payload byte count plus `sizeof(zlink_msg_t)`, so an empty frame is not free
and a pipe holding many small frames reaches its HWM before its payload sum
does. A delimiter, join, or leave frame carries no application payload and is
charged the `sizeof(zlink_msg_t)` metadata cost only. The same charge is
returned when the frame leaves the pipe.

The low water mark is the byte level at which a pipe returns read credit to a
blocked writer. Its default is `ceil(hwm_bytes / 2)` for the applied HWM of
that direction. A pipe may also carry a low-water-mark hint. A hint is used
only when it is below that default; a hint at or above the default leaves the
default in place. A hint greater than or equal to the HWM is clamped to
`hwm_bytes - 1`, and a clamped value below `1` becomes `1`, so the resulting
mark is always inside `1 .. hwm_bytes - 1`. A hint of `0` means no hint. An
unlimited HWM has no low water mark.

Core normally batches credit at that low water mark. If a sender actually
reaches its HWM, it first checks the monotonic bytes already read by its peer.
If the receiver later drains all currently visible input, it may return one
credit update before the LWM and wake the blocked writer then. This recovery
applies only to an HWM-blocked sender and therefore does not create a
cross-thread command for every normal low-depth message. A receiver that
drains a pipe no writer is waiting on sends no wakeup. This pipe threshold is
independent of a Framework receive-resume threshold.

#### Send retry

Submit retry only retries local submit failures classified as `ENOTCONN`,
`EHOSTUNREACH`, or `ECONNREFUSED`. A blocking submit to a locally initiated
paired endpoint treats these connectivity errors as retryable until the pair
validates. When the wait budget or attempt count is exhausted, the last
attempt's connectivity errno is preserved and normalized into the public
result. `ENOTCONN` and `EHOSTUNREACH` return
`ZLINK_SUBMIT_NOT_CONNECTED`; `ECONNREFUSED` returns
`ZLINK_SUBMIT_NOT_ADMITTED`. `ZLINK_DONTWAIT` calls, backpressure (`EAGAIN`),
admission rejection, argument errors, and reply timeout after successful
request submit are not retried.

### Dedicated functions (not option enums)

- **Routing ID**: `zlink_set_routing_id()` / `zlink_get_routing_id()`
- **TLS**: `zlink_set_tls_server()` / `zlink_set_tls_client()`
- **Subscribe/Unsubscribe**: `zlink_set_subscription()` / `zlink_unset_subscription()`

## 6. Functions

### zlink_socket

Create a socket.

```c
ZLINK_EXPORT void *zlink_socket (void *context_, zlink_socket_type_t type_);
```

Creates a new socket within the given context. The `type_` parameter selects
the messaging pattern. Raw sockets use the pull functions in
[Section 3](#3-pull-receive-and-completion-model). STREAM explicitly selects
RAW or PACKET receive mode before its first successful bind or connect. The
socket must be closed with `zlink_close()` before the context is terminated.

**Returns:** Socket handle on success, `NULL` on failure (errno is set).

**Errors:** `EINVAL` if the socket type is invalid. `EMFILE` if the maximum
number of sockets has been reached. `ETERM` if the context was terminated.

**Thread safety:** Thread-safe with respect to the context.

**See also:** `zlink_close`, `zlink_ctx_new`

---

### zlink_recv_part

Receive one message part from a raw socket.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (void *s_,
                                                  const zlink_routing_id_t **source_rid_out_,
                                                  zlink_msg_t *part_out_,
                                                  zlink_part_flag_t *has_more_out_,
                                                  zlink_recv_flags_t flags_);
```

Supported types are raw `PAIR`, `DEALER`, and `STREAM`. Raw `PUB`, `XPUB`,
`SUB`, `XSUB`, and `ROUTER` are not supported; the function returns
`ZLINK_RECV_NOT_SUPPORTED` and sets `errno` to `ENOTSUP`. `part_out_` and
`has_more_out_` are required, and `part_out_` must be initialized before the
call. `source_rid_out_` is optional. A successful receive closes the existing
contents of `part_out_` and transfers ownership of the new part to the caller.
The caller moves the message or closes it with `zlink_msg_close()` before the
next successful overwrite. STREAM returns a Core-owned routing-ID view; PAIR
and DEALER return `NULL`. `*has_more_out_` is `ZLINK_PART_MORE` when another
part follows and `ZLINK_PART_FINAL` for the last part.

The same thread and receive family receive every part from the first part of a
multipart record through `FINAL`. Entry by another thread or receive family
mid-record returns `ZLINK_RECV_INVALID_STATE` with `errno == EBUSY`; the
original owner can continue receiving the staged record. `flags_` accepts only
`NONE` or `DONTWAIT`. An unknown bit returns `ZLINK_RECV_INVALID_STATE` with
`errno == EINVAL`.

When no record is available, `DONTWAIT` immediately returns
`ZLINK_RECV_NO_DATA` with `errno == EAGAIN`. `NONE` snapshots
`ZLINK_OPT_RCVTIMEO` on entry: the default is 1,000 ms, `0` is immediate, and
`-1` waits indefinitely. A timeout returns `ZLINK_RECV_NO_DATA` with
`errno == EAGAIN`. Context termination during a blocking wait returns
`ZLINK_RECV_TERMINATED` with `errno == ETERM`; socket shutdown returns
`ZLINK_RECV_INVALID_STATE` with `errno == ESHUTDOWN`. Every failure leaves all
outputs and message contents unchanged.

A returned RID view remains valid until entry to the next data receive API on
the same socket or until socket close. Poller wait, completion receive, monitor
receive, and data receive on another socket do not invalidate it. Entry to the
next data receive on the same socket invalidates the previous view regardless
of whether that call succeeds. A caller or binding that retains the RID longer
copies it to owned storage immediately after receive.

---

### Routed and subscription receive family

Dedicated pull functions receive ROUTER DATA and REQUEST records, SUB and XSUB
topic DATA, and XPUB subscription events.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_router_recv_part(
  void *router_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_reply_token_t *reply_token_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);

ZLINK_EXPORT zlink_recv_result_t zlink_subscribe_part(
  void *sub_,
  const zlink_routing_id_t **source_rid_out_,
  char *topic_id_buf_,
  size_t topic_id_capacity_,
  size_t *topic_id_len_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);

ZLINK_EXPORT zlink_recv_result_t zlink_xpub_recv_part(
  void *xpub_,
  const zlink_routing_id_t **source_rid_out_,
  int *subscribed_out_,
  char *topic_id_buf_,
  size_t topic_id_capacity_,
  size_t *topic_id_len_out_,
  zlink_recv_flags_t flags_);
```

| Function | Required outputs | Optional output | Value on success |
|---|---|---|---|
| `zlink_router_recv_part` | `source_rid_out_`, `reply_token_out_`, initialized `part_out_`, `has_more_out_` | none | token `0` for DATA; the same nonzero token on every REQUEST part |
| `zlink_subscribe_part` | `topic_id_len_out_`, initialized `part_out_`, `has_more_out_` | `source_rid_out_` | `NULL` source for SUB and XSUB; topic bytes copied without NUL |
| `zlink_xpub_recv_part` | `subscribed_out_`, `topic_id_len_out_` | `source_rid_out_` | `1` for subscribe or `0` for unsubscribe, peer RID, and topic bytes |

A NULL required handle or output returns `ZLINK_RECV_INVALID_HANDLE` with
`EFAULT`. Unknown flag bits and entry by a thread or family that does not own
an in-progress multipart record return `ZLINK_RECV_INVALID_STATE` with
`EINVAL` and `ZLINK_RECV_INVALID_STATE` with `EBUSY`, respectively. `NONE`
timeouts and termination, DONTWAIT behavior, part ownership, unchanged outputs
on failure, and borrowed RID lifetime follow the common rules under
[`zlink_recv_part`](#zlink_recv_part). ROUTER DATA returns its source logical
RID and token `0`. REQUEST returns the same source RID and a nonzero opaque
reply token created by Core; every part of a multipart REQUEST repeats that RID
and token. The token is not a wire sequence, and applications do not interpret,
create, or modify it.

For SUB, XSUB, and XPUB, if `topic_id_capacity_` is less than the required
length, only `*topic_id_len_out_` is changed and the function returns
`ZLINK_RECV_BUFFER_TOO_SMALL` with `ENOBUFS`. The queued record and all other
outputs remain unchanged, so retrying with a sufficient buffer receives the
same record exactly once. A zero-length topic succeeds and consumes the record
with capacity 0 and a NULL buffer. A positive capacity with a NULL buffer
returns `ZLINK_RECV_INVALID_HANDLE` with `EFAULT` and does not consume the
record, regardless of the actual topic length.

A reply to a REQUEST sent by the requester appears only as a REQUEST
completion, never in a data receive function. DEALER neither receives typed
REQUEST records nor replies to them.

---

### zlink_close

Close a socket and release its resources.

```c
ZLINK_EXPORT zlink_close_result_t zlink_close (void *s_);
```

Closes the socket and releases all associated resources. Any outstanding
messages in the send queue are discarded or sent depending on the
`ZLINK_OPT_LINGER` setting. Public handles follow a tiered contract: hot-path
send operations can be called concurrently from multiple threads,
low-frequency control paths serialize for correctness, and close/destroy uses
a stricter lifecycle gate. If another thread has an in-flight API call on the
same handle, close fails with `errno=EBUSY`. After close is accepted, new API
entry fails with `errno=ESHUTDOWN`. Close internally releases pending
operations and completion or packet records that the application has not yet
pulled. A caller that needs a result or payload drains the queue before close.

**Returns:** `ZLINK_CLOSE_OK` on success; otherwise a `zlink_close_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:** `EFAULT` if the pointer is invalid, or `ESTALE` if the opaque
value is stale. `EBUSY` if another operation is in flight.

**See also:** `zlink_socket`

---

### zlink_set_option

Set a common socket option.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_option (void *handle_,
                      zlink_option_t option_,
                      const void *optval_,
                      size_t optvallen_);
```

Configures a common option. `handle_` may be a raw socket or discovery. The
`option_` parameter is a value from the `zlink_option_t` enum. The `optval_`
pointer supplies the value and `optvallen_` specifies its size in bytes.
`ZLINK_OPT_SNDHWM` and `ZLINK_OPT_RCVHWM` require an exact `uint64_t` value.

Configuration timing for raw sockets and discovery follows each option contract.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:** `EINVAL` if the option is unknown, its value is out of range, or a
byte-count option does not use the exact required size.
`ETERM` if the context was terminated.

**See also:** `zlink_get_option`

---

### zlink_get_option

Get a common socket option.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_option (void *handle_,
                      zlink_option_t option_,
                      void *optval_,
                      size_t *optvallen_);
```

Retrieves the current value of a common option. `handle_` may be a raw socket or
discovery. The two HWM byte-count options require a `uint64_t` output buffer
and an exact `*optvallen_` of `sizeof(uint64_t)` on input. Any other size,
including a larger scratch buffer or a legacy 4-byte one, fails with
`ZLINK_CONFIG_INVALID_ARGUMENT` and `errno == EINVAL` instead of truncating or
partially filling the value. On success `*optvallen_` stays `sizeof(uint64_t)`.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_set_option`

---

### zlink_socket_set_receive_flow_state

Set this socket's receive-flow state and synchronise it to the paired
completion lane.

```c
ZLINK_EXPORT zlink_config_result_t zlink_socket_set_receive_flow_state (
  void *handle_, zlink_receive_flow_state_t state_);
```

Stores `state_` as the socket-wide receive-flow state and sends it to every
peer connected over the paired DEALER/ROUTER completion lane. The call
completes when the socket-owning runtime thread has stored the local state; it
does not wait for any peer to observe it. Repeating the current state succeeds
and sends nothing new.

**Returns:** `ZLINK_CONFIG_OK` on success, including a repeat of the current
state. A socket type without a completion lane returns
`ZLINK_CONFIG_NOT_SUPPORTED` and keeps its byte HWM and transport backpressure
unchanged. [Errors](../03-errors.en.md) owns the full result table.

**See also:** `zlink_monitor_status`

---

### zlink_set_routing_id

Set the routing identity on a socket.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_routing_id (void *handle_,
                           const void *data_,
                           size_t size_);
```

Sets the routing ID of a raw socket. Its length is 1..255 bytes and the value is
binary-safe. Set it before bind or connect. Other handle kinds return
`ZLINK_CONFIG_NOT_SUPPORTED` with `errno == ENOTSUP`.
Raw `STREAM` is an exception: Core assigns a 4-byte routing ID per connection,
so setting it through this function is rejected with
`ZLINK_CONFIG_INVALID_ARGUMENT` and `errno == EINVAL`.
If the caller does not set a routing ID, Core assigns a 16-byte binary routing
ID with the RFC 4122 UUID v4 bit layout when it creates the socket. The default
value is raw UUID bytes, not a UUID string.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_get_routing_id`

---

### zlink_get_routing_id

Get the routing identity of a socket.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_routing_id (void *handle_,
                           zlink_routing_id_t *out_);
```

Copies the caller-configured or Core-generated routing ID of a raw socket into
a caller-owned `zlink_routing_id_t`.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_set_routing_id`

---

### zlink_set_tls_server

Configure TLS for a server socket.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_tls_server (void *handle_,
                           const char *cert_,
                           const char *key_,
                           int require_client_cert_);
```

Configures a TLS certificate and private key on a server socket and selects
whether to require a client certificate.

This function applies to raw server sockets that support TLS. Unsupported raw socket types and other handle kinds
return `ZLINK_CONFIG_NOT_SUPPORTED` with `errno == ENOTSUP`.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_set_tls_client`, `zlink_bind`

---

### zlink_set_tls_client

Configure TLS for a client socket.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_tls_client (void *handle_,
                           const char *ca_cert_,
                           const char *hostname_,
                           int trust_system_);
```

Configures a CA certificate, a hostname for SNI and certificate verification,
and whether to trust the system CA certificate store on a client socket.

This function applies to raw client sockets that support TLS. Unsupported raw socket types and other handle kinds return
`ZLINK_CONFIG_NOT_SUPPORTED` with `errno == ENOTSUP`.

**Returns:** `ZLINK_CONFIG_OK` on success; otherwise a `zlink_config_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_set_tls_server`, `zlink_connect`

---

### zlink_bind

Bind a socket to an address.

```c
ZLINK_EXPORT zlink_bind_result_t zlink_bind (void *s_, const char *addr_);
```

Binds the socket to a local endpoint. The endpoint string uses the format
`transport://address`. The supported `transport` values are:

- `tcp://interface:port` or `tcp://*:port`
- `inproc://name` (in-process)
- `ipc://pathname` (inter-process, POSIX only)
- `ws://interface:port` (WebSocket)
- `tls://interface:port` (TLS-encrypted TCP)

A socket can be bound to multiple endpoints. For TCP, if port 0 is specified
the system assigns an ephemeral port; use `ZLINK_OPT_LAST_ENDPOINT` to retrieve
the actual endpoint.

**Returns:** `ZLINK_BIND_OK` on success; otherwise a `zlink_bind_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**Errors:** `EADDRINUSE` if the address is already in use. `EADDRNOTAVAIL` if
the interface does not exist. `EPROTONOSUPPORT` if the transport is not
supported.

**See also:** `zlink_connect`, `zlink_unbind`

---

### zlink_connect

Connect a socket to a remote address.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_connect (void *s_, const char *addr_);
```

Connects the socket to a remote endpoint. The endpoint format is the same as
for `zlink_bind()`. A socket can connect to multiple endpoints, and the
library handles reconnection automatically if the peer becomes unavailable.

**Returns:** `ZLINK_CONNECT_OK` on success; otherwise a `zlink_connect_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_bind`, `zlink_disconnect`

---

### zlink_unbind

Unbind a socket from an address.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_unbind (void *s_, const char *addr_);
```

Removes a previously established binding.

**Returns:** `ZLINK_CONNECT_OK` on success; otherwise a `zlink_connect_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_bind`

---

### zlink_disconnect

Disconnect a socket from a remote address.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect (void *s_, const char *addr_);
```

Removes a previously established connection.

**Returns:** `ZLINK_CONNECT_OK` on success; otherwise a `zlink_connect_result_t` value. `zlink_errno()` retains the detailed internal errno for diagnostics.

**See also:** `zlink_connect`

---

### zlink_disconnect_rid

Disconnect a connected peer by routing id.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect_rid (
  void *s_,
  const zlink_routing_id_t *peer_rid_);
```

`peer_rid_` must not be empty. On success, the matched peer pipe enters the
asynchronous termination flow. A successful return does not mean the remote
peer has already processed the termination event.

ROUTER and STREAM use their routing maps for lookup. For STREAM,
`peer_rid_` must be the 4-byte connection routing id. Other socket types scan
the current connected-pipe source routing id snapshot. If more than one pipe
has the same routing id, the target is ambiguous and the call fails.

**Returns:** `ZLINK_CONNECT_OK` on success. Missing target maps to
`ZLINK_CONNECT_NOT_FOUND`, duplicate routing id maps to
`ZLINK_CONNECT_CONFLICT`, and lifecycle ownership conflict maps to
`ZLINK_CONNECT_BUSY`. `zlink_errno()` keeps the detailed internal errno for
diagnostics.

**See also:** `zlink_disconnect`, `ZLINK_OPT_RID_DUPLICATE_POLICY`

---

### Part send and pending admission

Sockets for which Core selects the logical target, such as PAIR and DEALER,
use `zlink_send_part()`. Sockets for which the caller supplies a routing ID,
such as ROUTER and STREAM, use `zlink_send_part_rid()`. A physical connection
ID or generation is not a public target. `zlink_publish_part()` on PUB and XPUB
does not produce completions.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part(
  void *s_, zlink_msg_t *part_, zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_, void *user_context_,
  zlink_completion_id_t *completion_id_out_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid(
  void *s_, const zlink_routing_id_t *target_rid_, zlink_msg_t *part_,
  zlink_send_flags_t flags_, zlink_part_flag_t part_flag_,
  void *user_context_, zlink_completion_id_t *completion_id_out_);
```

Both functions consume `part_` on every result and leave it in an empty,
initialized state. `MORE` stages a part in a socket-local sequence; a successful
`FINAL` admits the sequence as one record. Every call in a sequence uses the
same function family, target, and flags. An intermediate failure discards both
the staged prefix and the failing part. A caller that may retry retains a
separate copy of the complete record before submitting its first part.

`flags_` accepts only `NONE` or `DONTWAIT`, and `part_flag_` accepts only `MORE`
or `FINAL`. An out-of-range value or unknown bit discards the entire sequence
and returns `ZLINK_SUBMIT_INVALID_ARGUMENT` with `errno == EINVAL`.
`completion_id_out_` is optional; when non-NULL it is set to `0` before any
other validation. `user_context_` may be non-NULL only on a DONTWAIT `FINAL`.
A non-NULL context on `MORE` or a NONE `FINAL` discards the sequence and returns
`ZLINK_SUBMIT_INVALID_ARGUMENT` with `errno == EINVAL`. Core neither reads nor
frees the context pointer. The caller keeps its pointee alive until it receives
and closes the completion or discards the socket.

| Call result | Submit return | Completion ID | Later completion |
|---|---|---:|---|
| Successful `MORE` staging | `ZLINK_SUBMIT_OK` | 0 | none |
| `NONE FINAL` local send-queue admission | `ZLINK_SUBMIT_OK` | 0 | none |
| Immediate `DONTWAIT FINAL` admission | `ZLINK_SUBMIT_OK` | 0 | none |
| Core retains `DONTWAIT FINAL` as pending | `ZLINK_SUBMIT_OK` | nonzero | one SEND record |
| Pending or completion reservation limit exceeded | `ZLINK_SUBMIT_BACKPRESSURED`, `EAGAIN` | 0 | none |
| Validation or target failure | applicable submit result | 0 | none |

`NONE FINAL` snapshots `ZLINK_OPT_SNDTIMEO` on entry and waits for local
send-queue admission. The default is 1,000 ms, `0` is immediate, and `-1`
waits indefinitely. Expiration returns `ZLINK_SUBMIT_BACKPRESSURED` with
`errno == EAGAIN`, ID `0`, and no completion. `DONTWAIT FINAL` does not wait:
it either admits immediately or reserves the complete record in the
[pending pool](#common-options-zlink_option_t). A failed `FINAL` on either path
consumes and discards the staged prefix with the final part.

SEND and REQUEST share 65,536 unified completion reservations per socket. SEND
reserves a slot only when a DONTWAIT `FINAL` becomes pending; every successful
REQUEST `FINAL` reserves one. A slot remains reserved from reservation until
`zlink_completion_recv()` removes its record from the queue. Socket close also
releases slots for unread records. At the limit, Core does not accept the
operation: it returns `ZLINK_SUBMIT_BACKPRESSURED` with `errno == EAGAIN` and
ID `0`, and consumes and discards the entire sequence.

Once Core accepts a pending record, Core owns the payload and admission
retries. The application neither cancels the operation nor resubmits the same
payload. A transient transport shutdown is not terminal for a pending record
before admission or for an in-progress NONE `FINAL` wait. PAIR fixes its single
logical route, DEALER fixes the configured endpoint selected at `FINAL`, and
ROUTER and STREAM fix the logical peer RID. DONTWAIT pending records preserve
FIFO order while retrying admission on a new connection for the same logical
target. NONE does not use the pending pool; it waits for reconnect and
admission to the same target within the snapshotted `SNDTIMEO`.

Explicit removal of the configured endpoint or logical RID, or a permanent
protocol rejection, produces a SEND terminal completion only for a pending
record accepted by a successful DONTWAIT submit. A NONE wait that has not
returned completes synchronously: target removal returns
`ZLINK_SUBMIT_NOT_FOUND` with `ENOENT`, peer-type rejection returns
`ZLINK_SUBMIT_NOT_ADMITTED` with `EPROTOTYPE`, context termination returns
`ZLINK_SUBMIT_TERMINATED` with `ETERM`, and socket shutdown returns
`ZLINK_SUBMIT_TERMINATED` with `ESHUTDOWN`. Allocation failure before admission
returns `ZLINK_SUBMIT_OUT_OF_MEMORY` with `ENOMEM`; another runtime failure
returns `ZLINK_SUBMIT_INTERNAL_ERROR` with `EIO`. All carry ID `0`, produce no
completion, and consume and discard the complete sequence.

After ID `0` or `ZLINK_SEND_ADMITTED`, the payload follows the existing
transport-delivery contract. Core creates no separate application-record copy,
delivery ACK, or deduplication sequence, and does not replay the application
record on a new connection after a later disconnect. `ZLINK_SEND_ADMITTED`
means local send-queue admission, not confirmation of peer receipt.

### Request and reply

DEALER requests over a ROUTER logical route selected by Core. ROUTER requests
the specified ROUTER RID. A responding ROUTER replies with the source RID and
opaque reply token returned by receive.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_request_part(
  void *s_, const zlink_routing_id_t *target_router_rid_or_null_,
  zlink_msg_t *part_, zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_, uint32_t timeout_ms_,
  void *user_context_, zlink_completion_id_t *completion_id_out_);

ZLINK_EXPORT zlink_submit_result_t zlink_reply_part(
  void *router_, const zlink_routing_id_t *source_rid_,
  zlink_reply_token_t reply_token_, zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);
```

The DEALER target is always `NULL`; the ROUTER target is always non-NULL. Other
socket types return `ZLINK_SUBMIT_NOT_SUPPORTED` with `errno == ENOTSUP`. A
ROUTER typed request to a DEALER RID returns `ZLINK_SUBMIT_NOT_ADMITTED` with
`errno == EPROTOTYPE`; ordinary DATA send to that RID remains valid. A RID not
present in the routing map returns `ZLINK_SUBMIT_NOT_FOUND` with
`errno == ENOENT`.

Request `MORE` uses `timeout_ms_ == 0` and `user_context_ == NULL`. Violating
this rule discards the entire sequence and returns
`ZLINK_SUBMIT_INVALID_ARGUMENT` with `errno == EINVAL`. An optional ID output is
set to `0` before other validation and remains `0` for `MORE` or a submit
failure. A successful `FINAL` creates a nonzero ID and queues exactly one
REQUEST completion whether or not the caller requests the ID output. A request
`FINAL` accepts a context with both NONE and DONTWAIT and returns it unchanged
in that completion. Core neither reads nor frees the pointer; the caller keeps
its pointee alive until it receives and closes the completion or discards the
socket. A failed submit does not echo the context, so the caller can release
its own context state immediately after return.

Core reserves the completion ID and shared slot before exposing the request on
the wire. Slot exhaustion immediately returns `ZLINK_SUBMIT_BACKPRESSURED`
with `errno == EAGAIN`, ID `0`, and no completion, regardless of flags. A NONE
`FINAL` temporarily reserves the slot and ID, then waits within `SNDTIMEO` for
outbound local admission. A pre-admission failure releases the reservation and
returns the synchronous result and errno from
[part send](#part-send-and-pending-admission), ID `0`, and no completion. If the
pending pool permits, a DONTWAIT `FINAL` lets Core own the record before
admission and returns a nonzero REQUEST ID. This stage does not produce a
separate SEND completion.

`timeout_ms_ == 0` snapshots the requester socket's request timeout, whose
default is 5,000 ms. The reply timeout begins monotonically when the request
record enters the outbound local send queue; pending time before admission is
excluded. A disconnect after admission does not replay the request payload;
correlation and the running budget remain. The first resolver to remove pending
correlation, reply or timeout, creates the completion and discards the late
result.

`zlink_reply_part()` is a synchronous admission function without flags,
timeout, context, or completion ID. Every call consumes `part_`. The first
`MORE` or `FINAL` validates the RID, token, and completed REQUEST state, then
checks out the token to the reply sequence. `MORE` preserves staging and the
checkout. `FINAL` snapshots `SNDTIMEO` and waits for admission on the completion
route to the same logical source RID. Only a successful `FINAL` consumes the
token.

Reply-wait expiration returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN`;
allocation failure returns `ZLINK_SUBMIT_OUT_OF_MEMORY` with `ENOMEM`; another
runtime failure returns `ZLINK_SUBMIT_INTERNAL_ERROR` with `EIO`; context
termination returns `ZLINK_SUBMIT_TERMINATED` with `ETERM`; and socket shutdown
returns `ZLINK_SUBMIT_TERMINATED` with `ESHUTDOWN`. RID removal and a missing,
consumed, or RID-mismatched token return `ZLINK_SUBMIT_NOT_FOUND` with `ENOENT`.
A reply before REQUEST `FINAL` returns `ZLINK_SUBMIT_INVALID_STATE` with
`EBUSY`. A failed sequence clears its staging and checkout, but a token whose
RID and socket remain live can be retried from the beginning with the complete
reply retained by the caller. A second sequence for the same token returns
`ZLINK_SUBMIT_INVALID_STATE` with `EBUSY`, consumes only that call's part, and
preserves the first sequence. A later part in an active sequence that supplies
a different RID or token returns `ZLINK_SUBMIT_INVALID_ARGUMENT` with `EINVAL`,
discards the original sequence, and releases its checkout.

A reply token is an opaque nonzero capability scoped to `(responding ROUTER
socket, source logical RID)`. Applications do not interpret, create, or modify
it. Physical disconnect, generation change, and requester timeout do not
invalidate it. Only successful reply `FINAL`, logical RID removal, responding
socket close, or context termination invalidates it. There is no public abandon
or cancel API. A responder closes each received REQUEST with a successful reply
`FINAL`; if it has no payload, it sends one valid zero-length message. Omitting
`FINAL` after the first `MORE`, or discarding a token, retains its checkout,
staging, and slot until logical RID removal or responding socket close.

The live-token registry of a responding ROUTER holds 65,536 entries per
socket. At capacity, Core does not dequeue a new REQUEST to the application;
it stops reads and credit on that source pipe. DATA on other pipes and already
admitted records can proceed, but DATA behind the REQUEST on the same pipe does
not overtake it. When a slot is released, paused pipes resume round-robin. Core
neither evicts a token automatically nor drops the REQUEST.

### Completion pull and ownership

SEND and REQUEST share one socket-local completion queue.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_completion_recv(
  void *s_, zlink_completion_t *completion_out_, zlink_recv_flags_t flags_);

ZLINK_EXPORT void zlink_completion_close(zlink_completion_t *completion_);
```

The caller zero-initializes the output and sets
`struct_size = sizeof(zlink_completion_t)`. An empty output has every public
member other than `struct_size` set to its field-specific zero, empty, or NULL
value; padding bytes are not compared. An incorrect `struct_size` or non-empty
output returns `ZLINK_RECV_INVALID_STATE` with `errno == EINVAL`, without
removing a record or changing existing contents. A NULL socket or output
returns `ZLINK_RECV_INVALID_HANDLE` with `errno == EFAULT`. `NO_DATA` and every
other failure preserve an output that was empty on entry.

One successful receive returns exactly one kind, SEND or REQUEST, and leaves
fields unused by that kind zero, empty, or NULL. `peer_rid` is a snapshot of the
logical peer at reservation. It is empty for PAIR and DEALER SEND and DEALER
REQUEST; it is the submitted RID for ROUTER and STREAM SEND and ROUTER REQUEST.
It does not change to a physical connection identity after reconnect and is
not a capability for a later send target.

| Cause | SEND completion | REQUEST completion |
|---|---|---|
| Normal local admission or valid reply | `ZLINK_SEND_ADMITTED`, errno 0 | `ZLINK_REQUEST_OK` or wire error-reply mapping |
| Request reply timeout | not applicable | `ZLINK_REQUEST_TIMED_OUT` |
| Explicit endpoint or logical RID removal | `ZLINK_SEND_TERMINAL`, `ENOENT` | `ZLINK_REQUEST_NOT_FOUND` |
| Permanent peer-type rejection | `ZLINK_SEND_TERMINAL`, `EPROTOTYPE` | `ZLINK_REQUEST_REJECTED` |
| Malformed protocol | `ZLINK_SEND_TERMINAL`, `EPROTO` | `ZLINK_REQUEST_PROTOCOL_ERROR` |
| Allocation or runtime failure after acceptance | `ZLINK_SEND_TERMINAL`, `ENOMEM` or `EIO` | `ZLINK_REQUEST_INTERNAL_ERROR` |
| Transient physical disconnect | no terminal; retry same target before admission | retry before admission; after admission preserve the existing budget without replay |
| Context termination or socket close | internally discard pending and unread records; no new completion is guaranteed | same |

Core stores a REQUEST reply in a contiguous `zlink_msg_t[]` allocated before
enqueue. For a wire error reply, Core closes the errno part and normalizes only
the application payload into a new Core allocation beginning at index 0. With
no payload, the pointer is `NULL` and the count is `0`. If allocation fails,
Core closes the original payload and creates a payload-free
`ZLINK_REQUEST_INTERNAL_ERROR` completion. A successful receive transfers the
array and ownership of each message to the caller; receive itself allocates
nothing. The caller does not free the array directly and instead calls
`zlink_completion_close()` to release remaining messages and the allocator
base.

`zlink_completion_close()` is safe and idempotent for NULL, SEND, and empty
records. It resets every field to zero while preserving `struct_size`. If
`struct_size` is `0` or differs from the exact structure size, it does not free
a pointer and is a no-op. Every successfully received record, including SEND,
is closed.

`ZLINK_POLLCOMPLETION` is level-triggered while the completion queue is
nonempty. Poller wait does not consume a record. The caller repeats DONTWAIT
receive through `NO_DATA`. One socket queue has one drain owner; concurrent
drain by two threads is unsupported. SEND and REQUEST results are returned in
the linearization order in which resolvers append them to the socket-local
ready queue. This is neither submit order nor per-target wire order, so callers
distinguish results by ID or context.

Only PAIR, DEALER, ROUTER, and STREAM support `zlink_completion_recv()`; other
sockets return `ZLINK_RECV_NOT_SUPPORTED` with `ENOTSUP`. `flags_` accepts only
`NONE` or `DONTWAIT`; unknown bits return `ZLINK_RECV_INVALID_STATE` with
`EINVAL`. An empty queue under DONTWAIT and a NONE timeout return
`ZLINK_RECV_NO_DATA` with `EAGAIN`. NONE snapshots `RCVTIMEO` on entry: the
default is 1,000 ms, `0` is immediate, and `-1` waits indefinitely. Context
termination during a blocking wait returns `ZLINK_RECV_TERMINATED` with
`ETERM`; socket shutdown returns `ZLINK_RECV_INVALID_STATE` with `ESHUTDOWN`.
The output remains empty.

### zlink_multipart_close

Close all parts in a multipart message array.

```c
ZLINK_EXPORT void zlink_multipart_close (zlink_msg_t *parts, size_t part_count);
```

Convenience function that calls `zlink_msg_close()` on each element.

**See also:** `zlink_msg_close`

---

### zlink_socket_monitor_open

Open a socket monitor handle for pull receive.

```c
ZLINK_EXPORT void *zlink_socket_monitor_open (void *s_,
                                 const zlink_socket_monitor_open_options_t *options_);
```

Creates a monitor for socket `s_` and returns a handle. The `options_->events`
bitmask selects which events to observe. If `options_->monitor_hwm_bytes` is
`0`, the monitor queue uses Core's default byte budget; a positive value uses
that value as the monitor queue's byte HWM. [Monitoring](../06-monitoring.en.md)
owns the budget rules. Events are pulled with
`zlink_socket_monitor_recv()`. The monitor handle must be closed with
`zlink_monitor_close()` when no longer needed.

**Returns:** Monitor handle on success, `NULL` on failure (errno is set).

**See also:** `zlink_socket_monitor_recv`, `zlink_monitor_status`,
`zlink_monitor_close`

## 7. Internals

> **The document that owns this chapter's contract** — the public contract
> for each option is covered by the contract part of this document and the
> [socket options guide](../../../guide/12-socket-options.en.md). This section
> explains internal defaults and storage layout.

`options_t` stores common raw-socket and transport defaults. Typed socket
implementations validate pattern-specific options before applying them.

### Queue planning

`sndhwm` and `rcvhwm` are 64-bit accounted-byte limits. Their manual default is
`4,096,000` bytes, and `0` means unlimited. There is no message-count HWM
compatibility state. A runtime shrink keeps already queued messages and defers
the effective reduction until retained bytes fall below the new limit, then
applies the deferred shrink immediately.

Automatic HWM uses the context Core memory budget, profile role bounds, and a
registry of unique physical directional queues. The registry records one
inproc ypipe once rather than once per endpoint and identifies it with a stable
queue ID and generation. After manual reservations, bounded
[water-filling](../glossary.en.md#water-filling) starts each physical queue at
its role minimum and raises unsaturated queues to
their role maximum. Division remainders are granted one byte at a time in
stable queue-ID order.

Core does not add the values of two inproc endpoints. One finite-manual endpoint
sets the cap; two finite-manual endpoints use the smaller cap; an
unlimited-manual endpoint paired with an automatic endpoint uses the automatic
plan. Two unlimited endpoints remain unlimited for admission while reserving
the role maximum once for planning.

The DEALER/ROUTER completion progress lane carries only terminal replies and
error replies. It applies no automatic or manual HWM, LWM, inproc boost, role
bounds, or Core budget reservation. Disabling automatic HWM preserves the last
applied HWM on live pipes and excludes them from subsequent automatic planning.

The Core pipe low watermark is `ceil(hwm_bytes / 2)`. This value controls byte
credit updates and is not configurable through a Framework receive-resume
profile.

### Application-visible state

`zlink_monitor_status()` ABI version 4 exposes planned, applied, and deferred
64-bit HWM byte values; pending-message counts and pending bytes; bytes in
flight; the minimum message charge; and oversize single-message admission
counters, and adds a receive-flow-state detail flag plus five flow-metric
fields. The context budget snapshot distinguishes physical-queue capacity,
provisional and committed queue bytes, and completion and monitor queues. Its
ABI-compatibility retained-credit fields are always zero. These fields are diagnostic snapshots. Applications
configure policy inputs through public options rather than mutating internal
values.

### Transport defaults

Reconnect, TCP keepalive, kernel buffers, TOS, handshake intervals, and TLS
fields are applied by the relevant transport. Unsupported combinations fail
through the typed configuration result.

## 8. Implementation and contract-test verification requirements

Verify the following using only the public surface: socket creation,
connection, options, send/receive/completion functions, return values, and
`errno`. Each item maps to one unit test.

**Creation and lifetime**
- On success, `zlink_socket` returns a non-NULL handle. An invalid type produces
  `EINVAL`, reaching the maximum socket count produces `EMFILE`, and a
  terminated Context produces `ETERM`.
- On success, `zlink_close` returns `ZLINK_CLOSE_OK`. An invalid pointer
  produces `EFAULT`, and a stale opaque value produces `ESTALE`.
- When another thread is executing an admitted API on the same handle,
  `zlink_close` fails with `EBUSY`; after close is accepted, new API entry fails
  with `ESHUTDOWN`.
- On success, `zlink_socket_monitor_open` returns a pull monitor handle. On
  failure, it returns `NULL` with `errno` set.

**Options**
- `ZLINK_OPT_SNDHWM` and `ZLINK_OPT_RCVHWM` accept exactly
  `sizeof(uint64_t)` for both set and get. Any other size, including four
  bytes, fails with `ZLINK_CONFIG_INVALID_ARGUMENT` and `EINVAL` without
  truncating or partially filling the value. After a successful get,
  `*optvallen_` remains `sizeof(uint64_t)`.
- The unsupported socket option value `0x3034` fails with
  `ZLINK_CONFIG_INVALID_ARGUMENT` and `EINVAL`.
- Passing `ZLINK_OPT_BLOCKY` to `zlink_set_option()` or `zlink_get_option()`
  produces `ZLINK_CONFIG_NOT_SUPPORTED` / `ENOTSUP`.
- On DEALER, `ZLINK_OPT_CONFLATE=1` produces `ZLINK_CONFIG_NOT_SUPPORTED` / `ENOTSUP`, setting `0`
  succeeds, and the getter remains `0`. PUB and SUB accept `1` and return `1` from the getter.
- An unknown option, out-of-range value, or invalid byte-count size produces
  `EINVAL`; a terminated Context produces `ETERM`.
- `ZLINK_OPT_PENDING_MAX_MSGS/BYTES` are 0x303A/0x303B, default to
  0/unlimited, and are shared by DONTWAIT SEND and REQUEST. They reserve
  nothing on `MORE`, atomically reserve the complete record on `FINAL`, and
  release the pending charge on admission or terminal completion. Getting or
  setting either option outside PAIR, DEALER, ROUTER, and STREAM produces
  `ZLINK_CONFIG_NOT_SUPPORTED` with `ENOTSUP`.

**HWM admission** (see [Transport/Buffer](#transportbuffer))
- When accounted bytes reach the HWM, subsequent writes wait until the receiver
  returns byte credit.
- An empty pipe accepts one complete message whose total accounted size is
  known at admission even when it exceeds the HWM. That message must still
  pass `ZLINK_OPT_MAXMSGSIZE`, and writes after the one accepted message wait.
- An incremental multipart whose final size is unknown follows the ordinary
  byte HWM starting with its first `MORE` frame.
- An empty frame still has a nonzero charge (payload plus
  `sizeof(zlink_msg_t)`), so repeatedly sending empty frames reaches the HWM;
  the same charge is returned when a frame leaves the pipe.
- The default low water mark is `ceil(hwm_bytes / 2)`, a hint is always clamped
  to `1 .. hwm_bytes - 1`, and a sender that reached HWM can wake before LWM
  after the receiver drains all currently visible input.

**Receive**
- `zlink_recv_part` succeeds only on raw `PAIR`, `DEALER`, and `STREAM`. On raw
  `PUB`, `XPUB`, `SUB`, `XSUB`, and `ROUTER`, it produces
  `ZLINK_RECV_NOT_SUPPORTED` and `ENOTSUP`.
- When no part is available under `ZLINK_RECV_FLAGS_DONTWAIT`, the result is
  `ZLINK_RECV_NO_DATA` with `EAGAIN`.
- A successful receive transfers part ownership to the caller, which closes it
  exactly once; a failed receive does not transfer ownership.
  `source_rid_out_` is a Core-owned view on `STREAM` and is `NULL` on `PAIR`
  and `DEALER`.
- Entry to the next data receive on the same socket invalidates a borrowed RID;
  data receive on another socket, poller wait, completion receive, and monitor
  receive do not.
- A NONE receive snapshots `RCVTIMEO` 0/positive/-1 on entry. Timeout returns
  `ZLINK_RECV_NO_DATA` with `EAGAIN`, context termination returns
  `ZLINK_RECV_TERMINATED` with `ETERM`, and socket shutdown returns
  `ZLINK_RECV_INVALID_STATE` with `ESHUTDOWN`; outputs remain unchanged.
- A zero or undersized buffer for a nonempty SUB or XPUB topic changes only the
  required length and returns `ZLINK_RECV_BUFFER_TOO_SMALL` with `ENOBUFS`
  while preserving the record. Retrying with enough space receives that same
  record once, followed by `NO_DATA`.
- An empty topic succeeds and is consumed with capacity 0 and a NULL buffer. A
  positive capacity with a NULL buffer returns `ZLINK_RECV_INVALID_HANDLE`
  with `EFAULT` without consuming the record, regardless of topic length.

**Routing ID and connection termination**
- If no routing ID is set, socket creation assigns a 16-byte binary routing ID
  with the RFC 4122 UUID v4 bit layout, which `zlink_get_routing_id` returns.
- `zlink_set_routing_id` accepts a binary-safe value of 1..255 bytes. A
  non-raw-socket handle produces `ZLINK_CONFIG_NOT_SUPPORTED` and `ENOTSUP`.
- TLS setters succeed only on raw sockets that support TLS. Unsupported types
  and other handles produce `ZLINK_CONFIG_NOT_SUPPORTED` and `ENOTSUP`.
- `zlink_bind` produces `EADDRINUSE` for an address in use,
  `EADDRNOTAVAIL` for a nonexistent interface, and `EPROTONOSUPPORT` for an
  unsupported transport. After binding TCP port 0, the actual endpoint is
  available through `ZLINK_OPT_LAST_ENDPOINT`.
- `zlink_disconnect_rid` produces `ZLINK_CONNECT_NOT_FOUND` for no target,
  `ZLINK_CONNECT_CONFLICT` for a duplicate routing ID, and
  `ZLINK_CONNECT_BUSY` for a lifecycle ownership conflict.

**Part send and completion**
- An immediately admitted DONTWAIT `FINAL` returns ID `0` and no completion. If
  Core retains it as pending, it returns a nonzero ID and exactly one SEND
  completion. A NONE `FINAL` waits for admission to the same logical target
  within the snapshotted `SNDTIMEO` and returns ID `0` with no completion.
- Every part call consumes its input on success and failure. A failed `FINAL`
  also discards its staged prefix. Rejection by a pending or completion limit
  returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN` and ID `0`.
- A transient disconnect before admission retries FIFO on a new connection to
  the same logical target. After ID `0` or `ZLINK_SEND_ADMITTED`, Core does not
  replay the application payload.
- Filling all 65,536 slots with a mix of SEND and REQUEST completions makes the
  next completion-bearing `FINAL` return `ZLINK_SUBMIT_BACKPRESSURED` with
  `EAGAIN` and ID `0`. Receiving one record allows the next submit again.

**Request and reply**
- DEALER requests a known positive-weight ROUTER route with a NULL target;
  ROUTER requests a non-NULL ROUTER RID. A ROUTER request to a DEALER RID
  returns `ZLINK_SUBMIT_NOT_ADMITTED` with `EPROTOTYPE`, while DATA send to the
  same RID remains valid.
- A successful request `FINAL` creates a nonzero ID and exactly one REQUEST
  completion. Submit failure returns ID `0`, no completion, and no context
  echo. Reply timeout starts at local admission and excludes pre-admission
  pending time.
- Only a successful `zlink_reply_part()` `FINAL` consumes the token scoped to
  `(responding ROUTER, source RID)`. Physical disconnect, generation change,
  and requester timeout do not invalidate it; RID removal, responder close,
  and context termination do.
- At 65,536 live tokens on a responding ROUTER, Core neither drops nor evicts a
  new REQUEST. It pauses reads from that source and resumes paused sources
  round-robin after a slot is released.
- A non-NULL request ID output is set to `0` before other validation and remains
  `0` for `MORE` and submit failure. A successful `FINAL` whose caller omits the
  output still places an internal nonzero ID and context in exactly one
  completion.
- Reply allocation, runtime, context, and socket failures return
  `OUT_OF_MEMORY` with `ENOMEM`, `INTERNAL_ERROR` with `EIO`, `TERMINATED` with
  `ETERM`, and `TERMINATED` with `ESHUTDOWN`, respectively. Every call consumes
  its part; a live token can be retried from the beginning.
- A token without a reply is not consumed automatically. A zero-length-message
  reply, logical RID removal, or socket close releases its slot.

**Completion receive and ownership**
- While a completion exists, `ZLINK_POLLCOMPLETION` is level-triggered and
  poller wait alone does not shrink the queue. DONTWAIT receive of the final
  record clears readiness.
- An incorrect `struct_size` or non-empty output neither dequeues nor overwrites
  a record. `zlink_completion_close(NULL)` and close of SEND or empty records
  are safe and idempotent and preserve `struct_size`.
- REQUEST success and valid error-reply payloads transfer from base index 0 of
  a contiguous array, and `zlink_completion_close()` releases remaining
  messages and the array. A malformed errno part produces payload-free
  `ZLINK_REQUEST_PROTOCOL_ERROR`; normalization allocation failure produces
  payload-free `ZLINK_REQUEST_INTERNAL_ERROR`.
- Socket close and context termination internally release pending and unread
  records and do not guarantee delivery of a new terminal completion.
- Completion receive with NONE snapshots `RCVTIMEO` 0/positive/-1 on entry.
  Timeout, unknown flags, NULL input, and context or socket termination during
  a blocking wait preserve the queue and empty output with the specified
  result and errno.
- When SEND and REQUEST completions are interleaved, every nonzero ID and
  context is returned once in socket-local append linearization order, without
  loss or coalescing due to event-array size.
- Completion `peer_rid` is empty for PAIR and DEALER SEND and DEALER REQUEST;
  for ROUTER and STREAM SEND and ROUTER REQUEST it is the submitted RID
  snapshot and does not change to a physical identity after reconnect.

**Pull-only surface**
- Socket DATA, STREAM packets, SEND and REQUEST completions, and monitor events
  are consumed through their designated pull functions. A
  `zlink_poller_event_t` contains only readiness bits, not operation payload.

**Receive-flow state**
- Setting the current state again with `zlink_socket_set_receive_flow_state`
  succeeds and sends nothing new.
- A socket type without a completion lane returns
  `ZLINK_CONFIG_NOT_SUPPORTED` and preserves its existing byte HWM and
  transport backpressure.

<!-- zlink-nav:start -->
[Core Spec Index](../README.en.md) | [Previous: Runtime Boundary](../08-runtime-boundary.en.md) | [Next: PAIR](01-pair.en.md)
<!-- zlink-nav:end -->
