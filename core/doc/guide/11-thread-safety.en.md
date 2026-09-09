
<!-- zlink-nav:start -->
[← Message API and ownership](09-message-api.en.md) | [Routing IDs →](08-routing-id.en.md)
<!-- zlink-nav:end -->

# Thread safety

Core uses a tiered same-handle concurrency contract.

## Data path

Concurrent send operations on a supported handle are admitted. The conceptual
`send`/`publish`/`send_rid` hot paths map to the typed `*_part` APIs. A successful
multipart sequence remains contiguous, but callers must not split one logical
multipart sequence across threads.

Receive is single-consumer unless a specific API states otherwise. Do not run
two receive calls on the same socket concurrently. The routing-id view returned
by receive is owned by the socket — do not use it after the same socket's next
data-recv call (success or failure) or after close. Copy the value immediately
if it must be retained.

## Control path

Configuration and endpoint operations serialize internally where required.
Serialization prevents data races; it does not make conflicting lifecycle
changes meaningful. Configure options before traffic starts whenever possible.

## Close

Close uses a stricter lifecycle gate. It returns `ZLINK_CLOSE_BUSY` (`EBUSY`)
while another thread is executing an API call on the same handle. After close
is accepted, new entries return `ESHUTDOWN`.

## Pull model — no callbacks

Core registers no application callbacks. Socket data, completions, monitor
events, and timer fires are all pulled by an application thread: wait for
readiness with a poller, then call `*_recv_part()`, `zlink_completion_recv()`,
`zlink_socket_monitor_recv()`, or `zlink_timer_recv()`. There is therefore no
"keep callbacks short" rule; the application decides which thread receives —
keep one socket's receive to a single consumer.
