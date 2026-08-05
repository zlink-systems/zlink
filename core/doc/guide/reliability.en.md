[한국어](reliability.ko.md)

# Raw Messaging Reliability

> **What this chapter answers** — it separates what Core guarantees for
> delivery from what it does not guarantee, which then becomes the
> application's or Framework's responsibility (retry, deduplication, durable
> storage).

Core provides the delivery rules of the transport and socket pattern, but it
does not create an application-level delivery guarantee. Retry,
deduplication, durable storage, and business transactions are the
application's or Framework's responsibility.

## Queue And Backpressure

Once the HWM is reached, a send can return a backpressure result. Setting a
blocking timeout does not mean delivery completed or that remote processing
completed. Even after a send-ready notification, another sender may claim the
queue first, so check the retry result.

## PUB/SUB

A subscriber cannot receive a message published before it sets up the
subscription and connection. A slow subscriber's processing is limited by the
HWM and publisher options. Design a separate synchronization or resend path
for an important event.

## Request/Reply

A request timeout means the reply did not arrive within the time limit — it
is not proof that the remote never processed the request. To retry a request
that has side effects, use an application request id and a deduplication
rule.

## Connection Transitions

Transport state and route availability change during a reconnect. Success
from `zlink_connect()` means the connection intent was accepted, not that
sending is immediately possible. Use the poller, send-ready, and socket
monitor to observe the actual state.
