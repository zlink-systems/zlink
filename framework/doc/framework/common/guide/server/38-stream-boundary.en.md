# How STREAM Works

!!! info "What you get from this chapter"

    You can tell when a stream node registration is refused, where an error goes, and how one
    reply is matched to its request.

[STREAM](23-stream.en.md) went as far as accepting a connection and answering one packet. This
chapter covers what the framework settles at that boundary — the conditions refused at startup, where
errors belong, how long a reply token lives, and the execution mode the connecting side picks.

## 1. Conditions Refused Before Startup

Registration is decided by the node name, the bind endpoint and the session type. **The
bind endpoint must be given.** There is no surface that registers a stream node
automatically from a marker on a declaration.

The following conditions are not deferred to the first connection; they fail as configuration
errors **before the host starts**.

| Condition |
| --- |
| The node name is empty |
| The same node name was registered twice |
| There is no bind endpoint |
| The same session type was registered twice |
| More than one session was registered on one node |
| TLS is on but the certificate path is empty |
| TLS is on but the key path is empty |
| A client certificate is required without a TLS server configured |

With TLS on, the certificate and the key path are given together. Requiring a client certificate is
off by default; with it on, a connection that fails verification is refused **before a session is
created**.

## 2. Where an Error Goes

<iframe class="zlink-diagram" src="/common/diagrams/38-stream-dispatch-en.html" title="What one connection passes through" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/38-stream-dispatch-en.html" target="_blank">↗ View larger</a></p>

**The session error callback receives only transport errors that belong to that session.** The
rest take the paths below.

| Error | Where it goes |
| --- | --- |
| A transport error on that session | The session error callback |
| A handshake failure | Runtime observation. There is no session yet to call |
| A socket-level or node-level error | Runtime observation. It cannot be pinned to one session |
| An application handler exception | The handler exception path. **Not the session error callback** |

**Handler filters do not apply to session dispatch.** A filter registered for another dispatch does
not run ahead of a session callback —
[Handlers and Message Processing](31-handler-dispatch.en.md#2-filters--collecting-shared-processing-in-one-place)
covers the scope of filters. Work that must be screened on the session path, such as
authentication, is handled by the session's own handler registration.

**There is no surface that runs a receive loop directly.** The framework queues the packet and then
runs the session callback, applying dispatch, injection and logging consistently at that boundary.
The design removes the loop, the cancellation and the backpressure from the application's code —
[Backpressure](33-backpressure.en.md) covers what comes after.

## 3. The Lifetime of a Reply Token

Answering a request uses the **one-shot reply token** of the current dispatch. That token is valid
only for the current request and can be submitted once. It cannot be reused even when the send
fails through a timeout or a cancellation.

**A reply carries no packet name.** The connecting side finds the pending request by the request
sequence alone, and the type to read the reply as is decided by **the type named at the call**.
Since nothing is selected by name, there is no surface for putting a packet name on a reply. An
error reply comes back on the same sequence.

Sending to a peer that has no pending request uses a send rather than a reply.

## 4. The Execution Mode the Connecting Side Picks

The immediate mode runs callbacks on the connector's worker, which suits a client with no fixed
execution site. A client with a fixed site, such as a game loop or a UI thread, picks the manual
mode and drains what has queued up inside its own loop, at the moment the application calls for
it.

### 4.1 Turning Off the Observation Cost

The connector takes the same diagnostics-level option as the server runtime. The default keeps
errors only; lowering it to the bottom level stops the connector creating or attaching a flow
identifier on outbound frames, so the observation-only cost disappears.

The value that matches a request to its reply is protocol information rather than diagnostics, so it
keeps working at the bottom level.

## 5. Related Documents

- From accepting a connection to answering — [STREAM](23-stream.en.md)
- Binding one connection to an Actor — [Session and Actor](24-actor-session.en.md)
- The rules of that binding — [How Session Binding Works](39-session-binding.en.md)
- The scope filters apply to — [Handlers and Message Processing](31-handler-dispatch.en.md)
- The package a client installs — [Installation](../../../install.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
