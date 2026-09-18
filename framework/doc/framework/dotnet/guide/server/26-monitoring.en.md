---
title: "Monitoring · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/26-monitoring.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Monitoring

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: Location](25-location.en.md) | [Next: The Execution Model](32-execution-model.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/server/26-monitoring.en.md) · **C#/.NET** · [Java](../../../java/guide/server/26-monitoring.en.md) · [Kotlin](../../../kotlin/guide/server/26-monitoring.en.md) · [Node/TypeScript](../../../node/guide/server/26-monitoring.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You can read what is ready right now, receive that state whenever it changes, and record
    where one message ended up. The code here is the minimal call on each language's
    observation surface.

The earlier chapters covered registering and calling. Once it is running, something else is
needed — whether the connections are ready, which peer dropped out, and where a message failed.
No amount of reading handlers answers that. **The framework exposes them on public
surfaces.**

There is no surface that delivers runtime events to a handler. Observation goes through the
surfaces below.

## 1. The Kinds of Observation Surface

| What you see | How | Where it is covered |
| --- | --- | --- |
| Whether it is ready now, who dropped out | Status lookups and status subscriptions | [Reading the Current State](#2-reading-the-current-state) · [Subscribing to Changes](#3-subscribing-to-changes) |
| Where and how one message ended | Diagnostic records (traces, logs) | [Setting the Diagnostics Level](#4-setting-the-diagnostics-level) |
| Numbers such as concurrent users and queue depth | Meters | [Operations and Lifecycle](12-operations.en.md#1-runtime-metrics) |

<iframe class="zlink-diagram" src="/common/diagrams/26-observation-paths-en.html" title="The kinds of observation surface" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/26-observation-paths-en.html" target="_blank">↗ View larger</a></p>

They are consumed differently. **The status surface** is for reading the current value or
receiving changes in order, **diagnostics** for tracing an individual message, and **meters** for
gathering the numbers a dashboard shows.

## 2. Reading the Current State

A lookup returns **one value as of the call**. Use it to answer an operational endpoint or to check
something once.

```csharp
var fanout = app.Services.GetRequiredService<IZLinkFanoutRuntime>();

var status = fanout.GetStatus("user.events");
if (!status.IsReady)
    logger.LogWarning("fanout not ready: {Channel} {State}", status.ChannelName, status.State);
```

**Read the readiness and the state value together.** Readiness alone does not say what to do, and
the reason for not being ready comes separately — no eligible peer, a stalled record store and a
drain in progress each call for a different response.

The value carries only the peer's identifier, its current state and why it is unusable. Reconnect
attempt counts and internal socket state are not part of the public contract.

## 3. Subscribing to Changes

A subscription gives **the complete value after each change**, not an event carrying only the
fields that changed. If a comparison with the previous value is needed, the subscriber keeps it.

```csharp
await foreach (var observed in fanout.ObserveAsync("user.events", cancellationToken: ct))
{
    var update = observed.Status;
    logger.LogInformation("publishers={Count} state={State} seq={Seq}",
        update.ReadyPublisherCount, update.State, update.Sequence);

    if (observed.Loss.DiscardedTerminalCount > 0)
        logger.LogWarning("lost terminal statuses: {Count}", observed.Loss.DiscardedTerminalCount);
}
```

**Values can be missed.** A slow receiver skips intermediate values past the retention limit. The
missed count comes with each item and splits into those coalesced away and those that can no longer
be received past the limit. The first means the latest value did arrive; the second does not.

The order of two values is decided by the sequence number carried on the item. The time that comes
with it is for display.

A lookup is for reading once; a subscription is for recording or reacting to state transitions.
Only a subscription skips intermediate values.

A subscription stays open until it is cancelled, so **run it somewhere tied to the host's
lifetime.** That place differs by language — a background service, the observation object's
lifetime, a cancellation signal.

## 4. Setting the Diagnostics Level

Where and how one message ended is what diagnostics record. The levels are as follows.

```csharp
builder.Services.AddZLinkFramework(options =>
{
    options.ConfigureDispatch().Diagnostics
        .SetLevel(ZLinkDiagnosticsLevel.Normal)
        .SetSampleRate(0.1)
        .IncludeMessageSizes(false);
});
```

| Level | What it records |
| --- | --- |
| Off | Nothing. No trace item and no string is created |
| Errors (default) | Dispatch failures and backpressure |
| Normal | The above plus major transitions such as receive, dispatch and completion |
| Detailed | The above plus byte sizes and elapsed time |

**Keep it at the errors step in production and raise it only when needed.** The detailed step
records per message, so in a high-throughput stretch it becomes load in itself. A logging filter
that only discards the output is not the same as the off step — the cost of producing it remains.

The value that ties one request to its reply, and the value that ties that request to the calls it
started, are both carried on the record. Use them to see pieces from several nodes as one.

Where the records are stored and where they are exported **is owned by the application's existing
configuration.** The framework exposes neither an observation callback nor a file path; it writes
to the standard providers the application has configured. A failing provider call does not change
the result of the original message processing.

## 5. Common Problems

- **I want runtime events delivered to a handler** — there is no such surface. State changes come
  from [Subscribing to Changes](#3-subscribing-to-changes), and per-message results from
  [Setting the Diagnostics Level](#4-setting-the-diagnostics-level).
- **The subscription gives nothing** — it is quiet when that name has not changed. If the current
  value is needed, read [Reading the Current State](#2-reading-the-current-state) first and then
  start the subscription.
- **I expect a health endpoint** — the framework creates no HTTP endpoint. Wire the readiness into
  the application's existing endpoint —
  [Operations and Lifecycle](12-operations.en.md#4-wiring-operational-calls-and-readiness) is that
  place.
- **I want to see what lives on which node** — use the lookup in [Location](25-location.en.md) and
  the topology query in
  [Operations and Lifecycle](12-operations.en.md#5-location-readiness-and-operational-queries).
- **I want to know about messages with no handler** — at the errors step or above they are recorded
  as dispatch failures. A request comes back as an error reply while a send is dropped quietly, so
  the send side shows up only in diagnostics —
  [How Channels Work](30-channel-patterns.en.md#7-what-it-means-for-a-call-to-be-finished) covers
  that difference.

## 6. Related Documents

- Numbers and operational calls — [Operations and Lifecycle](12-operations.en.md)
- The reasons for not being ready — [How Channels Work](30-channel-patterns.en.md#6-connection-and-discovery)
- When arrival outruns processing — [Backpressure](33-backpressure.en.md)
- The per-language surface names — [Key Type Index](13-interface-catalog.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
