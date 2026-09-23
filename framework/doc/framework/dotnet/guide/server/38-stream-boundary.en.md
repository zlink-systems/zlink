---
title: "How STREAM Works · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/38-stream-boundary.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# How STREAM Works

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Handlers and Message Processing](31-handler-dispatch.en.md) | [Next: How Session Binding Works](39-session-binding.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/38-stream-boundary.en.md) · **C#/.NET** · [Java](../../../java/guide/server/38-stream-boundary.en.md) · [Kotlin](../../../kotlin/guide/server/38-stream-boundary.en.md) · [Node/TypeScript](../../../node/guide/server/38-stream-boundary.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can follow the order in which a packet on one connection reaches the session callback, what
    happens when processing falls behind, how a reply is matched to its request, and where errors and
    connection close go.

[STREAM](23-stream.en.md) went as far as accepting a connection and answering one packet. This
chapter walks through what the framework does in between, in order.

## 1. The Order One Connection Goes Through

<iframe class="zlink-diagram" src="/common/diagrams/38-stream-dispatch-en.html" title="What one connection passes through" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/38-stream-dispatch-en.html" target="_blank">↗ View larger</a></p>

When a client connects, the framework creates that connection's session and runs the connected
callback. After that, one packet is handled in this order.

1. The framework secures one slot in the host's processing queue.
2. Only after securing the slot does it take one packet from Core.
3. It decodes the header and puts the packet name, metadata, request information, and **the packet's
   Actor** into the dispatch context. The packet's Actor is present only when the client sent through
   an Actor handle and that slot is a current binding — [How Session Binding Works](39-session-binding.en.md) covers this.
4. It passes the dispatch context and the not-yet-converted payload to the session callback. Inside
   the session callback, the payload is read as the type you want through the common decode surface.

When the connection drops, the framework runs the disconnected callback. What identifies a
connection is the routing ID that Core attaches to each connection, and it reaches the session
callback unchanged.

## 2. When Processing Falls Behind — Order and Backpressure

### 2.1 Client → Server

**While the processing queue is full, complete packets aren't dropped but stay in Core, and once
processing resumes they reach the session callback once each, in per-connection order.** A packet with
bad or incomplete framing isn't passed to the callback; the connection is closed.

When the processing queue has no free slot, the framework doesn't take the next packet. A packet not
taken stays in Core's receive buffer, and when that buffer reaches its limit (HWM), Core stops reading
from that connection's socket. TCP flow control then fills the send buffer of the client's socket and
the client's sends slow down. When a slot frees up, the held packets flow again in order. The queue
slots are shared by the whole host, so this applies to every connection on the host, not just one.
Limits and state transitions are covered in [Backpressure](33-backpressure.en.md).

### 2.2 Server → Client

Replies and pushes the server sends to a session queue up in Core's send buffer **per connection**.
When the server sends faster than that connection actually drains and the buffer reaches its limit
(HWM), a send to that connection waits until space frees up. Sends to other connections aren't
affected. If the deadline passes while waiting, the send ends with `DeadlineExceeded`, and the
framework doesn't resend the same content.

The client connector uses the runtime's own socket and keeps reading whatever arrives without a limit,
so slow processing in the client application doesn't hold back the server's sends. Pushes (`Send`)
pile up in the client's unbounded receive queue, while replies and error replies skip that queue and
complete the waiting request. The server's send waits when the network is slow or the client
can't read its socket. The receive size limit (§4) doesn't apply in this direction.

## 3. The Lifetime of a Reply Token

To answer a request you use the current dispatch's **single-use reply token**. The token is valid only
for the current request and can be submitted once. Even if sending fails through a timeout or
cancellation, the same token can't be used again.

A reply carries the request's sequence back unchanged, and the client uses that sequence to find the
request it was waiting on. A reply carries no packet name. The type the reply is read as is the type the client named when it made
the request. An error reply comes back under the same sequence.

When the server sends first to a client that has no pending request, it uses a send (push), not a
reply.

## 4. Size Limit on Received Messages

The limit for one message a client sends (header plus payload, excluding the 6-byte length prefix) is
64 KiB by default. It doesn't apply
to messages the server sends. A message over the limit reaches the session callback not even in part;
the server records `EMSGSIZE` and closes the connection. The client receives no error code and sees
only the connection close. Setting it to `0` removes the framework limit.

## 5. Where Errors Go

**The session error callback receives only transport errors that belong to that session.** Everything
else goes elsewhere.

| Error | Where it goes |
| --- | --- |
| A transport error of that session | The session error callback |
| Handshake failure | Runtime observation. There's no session yet to call |
| Socket- or node-level error | Runtime observation. It can't be pinned to one session |
| Application handler exception | The handler exception path |

## 6. When a Connection Closes

Once a connection starts closing, the framework accepts no new packets on it, completes or cancels
the reads and writes in progress, and then releases the TCP, TLS, or WebSocket resources. A transport
completion that arrives late after that doesn't touch released resources. What happens to the Actors
bound to that connection is covered in
[How Session Binding Works](39-session-binding.en.md#3-notification-when-the-connection-drops).

## 7. Settings Refused Before Startup

A stream node registration is defined by the node name, the bind endpoint, and the session type. The
following setting errors aren't deferred to the first connection; they are refused **before the host
starts**.

| Condition |
| --- |
| The node name is empty |
| The same node name was registered twice |
| There is no bind endpoint |
| The same session type was registered on more than one node of a host |
| More than one session was registered on one node |
| TLS is on but the certificate path is empty |
| TLS is on but the key path is empty |
| A client certificate is required without configuring a TLS server |
| The message size limit is negative |

When you turn TLS on, set the certificate and key paths together. Requiring a client certificate is
off by default; when on, a connection that fails verification is refused **before a session is
created**.

## 8. Related Documents

- From accepting a connection to answering — [STREAM](23-stream.en.md)
- Binding one connection to an Actor — [Session and Actor Connection](24-actor-session.en.md)
- Binding rules and telling packets apart by Actor — [How Session Binding Works](39-session-binding.en.md)
- The processing queue and its limits — [Backpressure](33-backpressure.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
