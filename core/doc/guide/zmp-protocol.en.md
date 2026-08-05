[한국어](zmp-protocol.ko.md)

# ZMP Protocol

> **What this chapter answers** — it introduces the concept of the wire
> protocol that exchanges the handshake and message frames between sockets.
> The exact frame format is owned by
> [ZMP protocol internals](../internals/protocol-zmp.en.md).

ZMP is the wire protocol that exchanges the handshake and message frames
between zlink raw sockets. An application generally does not implement the
protocol directly — it uses the socket API instead.

## Frame

A ZMP data frame expresses message boundaries and multipart continuation.
Socket metadata such as the routing id and subscription is returned separately
from the payload by that socket pattern's typed API.

## Handshake And Metadata

The connection handshake exchanges the socket type, identity, and protocol
metadata. When the transport is TLS or WSS, the TLS handshake must complete
first before the ZMP handshake proceeds.

## Request/Reply Envelope

DEALER's and ROUTER's typed request/reply APIs carry the message type and
request sequence in a Core-managed control part. The application payload
stays after the control part, and the receive API returns the payload with
the control part stripped, along with the request metadata.

See [ZMP internals](../internals/protocol-zmp.en.md) for the wire format's
implementation detail. The application topology protocol is owned by the
Framework packages and is outside the scope of the Core ZMP guide.
