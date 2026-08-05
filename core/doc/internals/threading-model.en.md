[한국어](threading-model.ko.md) | English

# Core Threading Model

## 1. Thread kinds

| Thread | Responsibility | Count |
|---|---|---|
| Application thread | Public API calls and blocking waits | Application-owned |
| I/O thread | Transport completions, engine state, and socket callbacks | Context `io_threads` |
| Reaper thread | Cleanup of terminated sockets and owned objects | One per Context |
| Timer scheduler | Generic timer deadlines and fire counts | Runtime-owned |

Core has no service mailbox or MeshNode-specific ingress thread.

## 2. Cross-thread communication

Commands from application threads reach owner threads through mailboxes.
Payload data moves between socket semantics and engines through pipe queues.
Each connection is pinned to one I/O thread, so multiple I/O threads do not
mutate one connection's engine state concurrently.

The `send`/`publish`/`send_rid` hot paths can be called concurrently on the
supported handle types. Low-frequency control paths serialize for correctness.
Receive remains single-consumer unless a formal API states otherwise.

## 3. Callbacks

Socket-message and transport-monitor callbacks run on the thread defined by
their formal API. Destructive close or receive-mode mutation that reenters the
same handle from its callback is rejected with the formal result and errno. No
new callback begins after Context termination.
