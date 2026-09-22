---
title: "Options and Defaults · C++"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/server/16-options.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Options and Defaults

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Previous: 12. Operations — Runtime Metrics · Graceful Drain · Readiness](12-operations.en.md) | [Next: 14. Picking a Sample — Start with the Example Closest to Your Problem](14-samples.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — **C++** · [C#/.NET](../../../dotnet/guide/server/16-options.en.md) · [Java](../../../java/guide/server/16-options.en.md) · [Kotlin](../../../kotlin/guide/server/16-options.en.md) · [Node/TypeScript](../../../node/guide/server/16-options.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "What you get from this chapter"

    You learn what you can set, what value applies when you do not set it, and which values can
    still be changed after the host has started.

Option names and defaults are the same in all five languages. What differs is the spelling and
the way you set them, and the tabs in each section show that. **Most options work without being
set.** Check the default on the line that matters when you have a reason to change it, and use
it as it is until then.

## 1. Where Settings Are Made and What They Cover

The same value has a different scope and a different window for change depending on where you
set it.

| Place | Scope | When it can change |
| --- | --- | --- |
| Root options | The default for this whole process | Before the host starts |
| Node builder | That one MeshNode, channel, or STREAM node | Before the host starts |
| Runtime option | Values that can change while running | While running ([§9](#9-values-that-can-change-while-running)) |

```cpp
app.add_zlink_framework ([] (zlink_framework_options_t &options) {
    options.configure_network ().set_bind_host ("0.0.0.0");   // root option
    auto mesh = options.add_route_mesh ("play");              // node builder
    mesh.listen ("tcp://0.0.0.0:5555").set_placement_weight (100);
    mesh.channel_name ("room").server ();
});
```

There is no surface that calls a builder again after the host has started. An invalid
combination is not deferred to the first call — it **ends as a configuration error during
startup.**

## 2. Root Options

| Option | What it sets | Default |
| --- | --- | --- |
| Codec registration | Payload serialization format | Built-in JSON |
| `bind_host` | The address a listener binds | `127.0.0.1` |
| `advertise_host` | The address given to peers | Not set — the non-wildcard bind address or wildcard loopback |
| `default_request_timeout` | How long a request waits for its reply | 30 seconds |
| `session_replacement_callback_timeout` | How long a session replacement callback may run | 30 seconds |
| Stream compression | STREAM payload compression | LZ4 in use |
| Worker `MinThreads` · `max_threads` | Thread count of the CPU worker pool | 0 · twice the processor count |
| Worker `idle_timeout` | How long an idle thread is kept | 30 seconds |
| `application_version` · `maintenance_wave` | Version and maintenance group a rolling update compares | 0 · not set |
| Handler discovery, filters, metadata policy | What is registered and which keys may pass | Only what is registered |
| Location store · relocation store | Stores for placement and state transfer | Single-node setup when absent |

- **The bind address defaults to loopback.** Set the bind address and the advertised address
  separately when a node or client on another host has to connect.
- The `listen` host is where the current process binds its socket, and `0.0.0.0` accepts on every
  local interface. `advertise_host` is the address peers actually dial and the Location Store publishes. Use
  `127.0.0.1` for a wildcard bind in a single-machine example. For multiple hosts, containers,
  NAT, or Kubernetes, set a reachable IP address or DNS name for that node, using a Pod IP or
  per-Pod DNS in Kubernetes. When omitted,
  [Network Listener Identity §2.1](../../../common/spec/server/02-channel-transport/04-network-listener-identity.en.md#21-defaults)
  advertises a non-wildcard bind host or the same-family loopback for a wildcard; an advertised
  host cannot be a wildcard. [Channel Messaging](20-channel-messaging.en.md#32-the-receiving-side--the-node-serving-the-channel)
  shows each language's actual option surface in its mesh registration block.
- **STREAM compression starts enabled.** Turn it off explicitly in the compression settings.
- **The CPU worker pool has no queue limit.** The Application job queue is what limits intake
  (§3). `default_request_timeout` rejects values of `0` or below.

## 3. Core HWM and Application Job Queue Limits

Core HWM limits the bytes held by the ordinary queues, and the Application job queue limits the
number of jobs waiting for a handler to start across the whole host. How both behave is covered
by [Backpressure](33-backpressure.en.md#1-core-hwm-and-the-application-job-queue).

| Option | What it sets | Default |
| --- | --- | --- |
| `core_hwm_memory_limit_bytes` | Memory limit hint passed to the Core budget calculation | Not set |
| `core_hwm_budget_bytes` | Manual Core budget that takes precedence over the profile | Not set |
| `CoreHwmProfile` | Core auto-budget profile | `Balanced` |
| `ApplicationJobQueueProfile` | Profile used to compute the job limit | `Balanced` |
| `max_queued_application_jobs` | Exact job limit that replaces the profile calculation | Not set |
| `set_application_job_queue_pause_threshold_percent` | Usage at which intake pauses | 80 |
| `set_application_job_queue_resume_threshold_percent` | Usage at which intake resumes | 60 |

The memory limit and the Core budget accept positive values only. The manual job limit ranges
over `1..2,147,483,647`, and `0` is not unlimited but a startup configuration error. The two
percentages range over `1..100` and `0..99`, and the resume value must be smaller than the pause
value. Both profiles use the same labels but are independent values, and `Balanced` means 128
jobs per processor.

## 4. Diagnostic Recording

| Option | What it sets | Default |
| --- | --- | --- |
| Recording level | `off` · `errors` · `Normal` · `Detailed` | `errors` |
| `TraceSampleRate` | Share of normal flows recorded, ranging over `0.0..1.0` | 1.0 |
| `include_message_sizes` | Whether payload byte sizes are recorded too | Not recorded |

What happens to a packet that arrives with no handler is set in the same place. A request gets
an error reply, while a send and a publish are recorded and dropped. The error-reply action
cannot be chosen for a send or a publish because they have no reply path. This setting does not
exist in C++, where only the default behavior applies. What each level records is covered by
[Monitoring](26-monitoring.en.md#4-setting-the-diagnostics-level).

!!! warning "The default for message size recording differs only on the JVM"

    Java and Kotlin start with `include_message_sizes` enabled, while the other languages start
    with it disabled. State the value explicitly to keep the volume of records aligned across a
    mixed-language deployment.

## 5. MeshNode Options

| Option | What it sets | Default |
| --- | --- | --- |
| `listen` | This node's own address for peers to connect to | Must be set |
| `bind_host` · `advertise_host` | Bind and advertised address for this node alone | The root value |
| `RoutingId` · `RoutingIdPrefix` | This node's identifier | Generated |
| `object_role` | Whether the node takes part in placement | See the note below |
| `placement_weight` | Share of new placements, ranging over `0..10000` | 100 |
| `actor_limit` · `spot_limit` | How many this node may hold at once | `0` — no limit |
| `ActivationConcurrency` | Cold activations that may proceed at once | 128 |
| `instance_spot_idle_timeout` | How long an idle Instance Spot is kept | `0` — never removed |
| `default_request_timeout` | Limit for requests leaving this node | The root value (30 seconds) |
| Peer connection | Peers to connect to manually | None — the location store finds them |

For both limits, `0` means no limit and a positive value ranges over `1..2,147,483,647`.
`ActivationConcurrency` rejects `0` instead, because it limits the activations in progress
rather than the number of objects.

!!! warning "The placement default differs only in C++"

    C++ starts as a `Server` that receives placements when `object_role` is left unset, while the
    other languages take no part in placement. State the role explicitly in C++ for a node that
    is to hold no Spot or Actor.

## 6. Send Waiting and Socket Limits

| Option | What it sets | Default |
| --- | --- | --- |
| `send_timeout` | How long a send waits for room | 1 second |
| `receive_timeout` | Wait limit in the receive direction | Not set |
| `SendHighWaterMark` · `ReceiveHighWaterMark` | Bytes held per peer. `0` is unlimited | Not set — the Core computes it |

Once a limit is reached, the sender waits up to `send_timeout`, and the call ends as a deadline
overrun if no room ever appears. Nothing is sent again automatically, so the application decides
whether to retry. **Connections between MeshNodes have no message size limit setting** — that
limit belongs to the STREAM node and the ClientServer listener
([Backpressure](33-backpressure.en.md)).

!!! warning "Where a manual HWM is set differs by language"

    The surface for setting the per-direction byte limit directly exists only in some languages
    and not in C++. When it is not set, the Core computes it from the physical queues, which
    makes leaving the value unset the ordinary configuration. A setting that limits one execution
    unit's mailbox by message count or bytes has no effect in any language.

## 7. Location Options

| Option | What it sets | Default |
| --- | --- | --- |
| `owner_lease_renew_interval` | How often ownership is renewed | 5 seconds |
| `owner_lease_ttl` | When ownership without renewal expires | 15 seconds |
| `owner_lease_renew_timeout` | Limit for one renewal attempt | 3 seconds |
| `owner_lease_fencing_margin` | Margin for releasing authority before expiry | 5 seconds |
| `polling_interval` | How often a store without change notification is re-read | 1 second |
| `store_failure_grace` | How long a store failure is tolerated | 30 seconds |
| `route_cache_max_age` | How long a resolved location is reused | 15 seconds |
| `message_follow_duration` | How long the former owner forwards messages to the new owner | 30 seconds |
| `session_relocation_seal_timeout` | Limit for waiting on a session route update | 3 seconds |
| `RelocationPayloadChunkLimit` | Size limit of one relocation payload chunk | 256 KiB |
| `RelocationInFlightPayloadBudget` | Chunk bytes in flight on one connection | 16 MiB |
| `RelocationNodeInFlightPayloadBudget` | The same limit across the whole node | `0` — not applied |
| `relocation_cutover_wait_timeout` | How long the cutover is awaited | 1 second |

**The lease values move together.** `OwnerLeaseRenewInterval + OwnerLeaseRenewTimeout` must be
smaller than `OwnerLeaseTtl - OwnerLeaseFencingMargin`. The defaults satisfy this at 8 seconds
against 10 seconds, so ownership survives one failed renewal. `route_cache_max_age` must be at
least five seconds shorter than `message_follow_duration`, and `0` for either turns off the route
cache and message forwarding respectively. Placement and transfer behavior are covered by
[Location](25-location.en.md) and [Relocation](37-relocation.en.md).

## 8. STREAM Node Options

| Option | What it sets | Default |
| --- | --- | --- |
| `bind` | The address clients connect to | Must be set |
| `bind_host` · `advertise_host` | Bind and advertised address | The root value |
| Session registration | The session type created per connection | Must be set |
| `max_message_size` | Byte limit of one message sent by a client | 64 KiB |
| TLS settings | Server certificate and whether a client certificate is required | Plaintext, no client certificate required |
| Actor dispatch | Forwarding an incoming packet to the bound Actor | Off |

`max_message_size` applies only in the client-to-server direction, and `0` means no limit is
imposed. A message over the limit does not reach the handler even in part, and the server closes
the connection. Actor dispatch is enabled once per STREAM node, and a second call is an error.
Which mesh the Actor is found in is decided by the global ActorId rather than an argument, so no
mesh name is given alongside. The registration code is covered by [STREAM](23-stream.en.md) and
[Session and Actor](24-actor-session.en.md).

## 9. Values That Can Change While Running

The values that can change after startup are the selection weights. The rest are fixed when the
host starts.

| Value | What it is for |
| --- | --- |
| Channel weight | Share of new requests and sends this node is selected for |
| Placement weight | Share of new Spots and Actors placed on this node |

```cpp
runtime_options.placement_weight (0);
runtime_options.channel ("room").weight (0);
```

Both values range over `0..10000` and default to 100. Setting `0` **stops new assignments only**
— existing objects and connections are kept. In a zero-downtime rollout, use it to keep new
traffic away from this node before starting a relocation
([Operations and Lifecycle](12-operations.en.md#4-wiring-operational-calls-and-readiness)).

## 10. Values That Must Be Set

These have no default, and leaving them out either fails startup or produces behavior other than
the one intended.

| Value | When it is missing |
| --- | --- |
| The MeshNode `listen` address | Configuration error at startup |
| At least one channel role or object role on the MeshNode | Configuration error at startup |
| The STREAM node `bind` address and session type | Configuration error at startup |
| Exactly one relocation policy per Spot or Actor factory | Configuration error at startup |
| A relocation store when a state-carrying factory or an Instance Spot exists | Configuration error at startup |
| A location store when several nodes are used | Peers cannot be found |
| Metadata keys passed between a connection and an Actor | The value is dropped without an error |

## 11. Common Problems

- **A setting was changed but has no effect** — most options are fixed when the host starts. The
  values that can change while running are listed in
  [§9](#9-values-that-can-change-while-running).
- **A node on another host cannot connect** — the bind address defaults to loopback. Set the bind
  address and the advertised address separately.
- **Memory keeps growing after a value was set to `0`** — `0` on a byte limit is unlimited. Leave
  the value unset to let the Core compute it.
- **Ownership keeps being lost** — the renewal interval plus the renewal timeout is larger than
  the lifetime minus the margin.
- **The connection drops on a large message from a client** — the STREAM size limit is 64 KiB.
  Raise it on a node that receives large payloads.
- **A weight of `0` was expected to drop existing connections** — a weight stops new assignments
  only, and existing objects and connections are kept.

## 12. Related Documents

- What the limits change — [Backpressure](33-backpressure.en.md)
- How to read the records — [Monitoring](26-monitoring.en.md)
- How to drain traffic with a weight — [Operations and Lifecycle](12-operations.en.md)
- Surface names per language — [Interface Catalog](13-interface-catalog.en.md)
