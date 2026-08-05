[한국어](11-thread-safety.ko.md)

# Thread safety

Core uses a tiered same-handle concurrency contract.

## Data path

Concurrent send operations on a supported handle are admitted. The conceptual
`send`/`publish`/`send_rid` hot paths map to the typed `*_part` APIs. A successful
multipart sequence remains contiguous, but callers must not split one logical
multipart sequence across threads.

Receive is single-consumer unless a specific API states otherwise. Do not run
two receive calls on the same socket concurrently. Routing-id and topic views
returned by receive are thread-local and may be invalidated by the next
receive-like call on that thread.

## Control path

Configuration and endpoint operations serialize internally where required.
Serialization prevents data races; it does not make conflicting lifecycle
changes meaningful. Configure options before traffic starts whenever possible.

## Close

Close uses a stricter lifecycle gate. It reports busy while another admitted
operation or callback prevents safe destruction. After close is accepted, new
entries report shutdown. STREAM raw callbacks cannot close their own handle.

## Callbacks

Socket receive callbacks run on the owning socket I/O thread. Monitor and
generic-timer callbacks run on the Core control runtime. Keep callbacks short
and move blocking application work to an application-owned queue.
