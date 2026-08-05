---
title: "Relocation Store Provider SPI And The Official Redis Implementation"
---

# Relocation Store Provider SPI And The Official Redis Implementation

[Spec table of contents](README.en.md) · [Previous: Location Store Provider SPI And The Official Redis Implementation](22-location-store-redis.en.md) · [Next: Runtime Status Query And Operational Diagnostics](24-runtime-monitoring.en.md)

> **What this chapter defines** — the public provider interface (SPI) of
> the Relocation Store, which holds the byte payload needed for
> relocation and recovery.


## 1. The Contract This Document Fixes

This document defines the public provider interface of the Relocation
Store, which holds the byte payload needed for the Framework's relocation
and recovery. An interface the framework calls and an external provider
implements, like this one, is called an SPI. A provider developer must
use the reference the framework created, unchanged, as the key to store
the payload, and must use that same reference to perform read, retention
extension, and delete.

The application doesn't call this SPI directly. A provider package
implements the SPI, and the framework uses the registered provider
instance.

The node currently responsible for running a
[Spot](01-glossary.en.md#spot) — a logical instance with an address and
state — is called the [owner](01-glossary.en.md#owner). The store that
holds this owner and lifecycle state and coordinates creation authority
is the [Location Store](01-glossary.en.md#location-store). The
Relocation Store doesn't manage this authority — it only holds, by
reference, the payload before it's published to the Location Store and
the payload that's already published. The order in which the two stores
are linked to carry out relocation and recovery is defined by
[Location Runtime](21-location-runtime.en.md).

The provider doesn't interpret the business meaning of the bytes it
stores. It stores, as uninterpreted bytes, application state, admission
records, timers, the move target and payload reference list, the
relocation phase, and even the
[activation envelope](01-glossary.en.md#activation-envelope) bundling the
first application message and creation information. The execution
procedure of [cold activation](01-glossary.en.md#cold-activation) — which
creates a new instance and prepares it to process the first message when
an [Instance Spot](01-glossary.en.md#entry-user-instance-spot) that could
be created at the needed moment, triggered by that first message, isn't
yet running — and of Actor Join, is also outside this document's scope.

## 2. Public SPI And Responsibility Boundary

The Relocation Store SPI provides only the following four operations and
provider responsibilities.

| Operation | Result the provider guarantees |
|---|---|
| `Put` | Stores the immutable payload at the framework-issued reference, or confirms it's byte-identical to an already-stored payload. |
| `Read` | Returns the immutable payload at the specified reference together with its expiry time and the provider's current time. |
| `Renew` | Recomputes the retention period based on the provider clock. |
| `Delete` | Removes the specified reference. Succeeds even if the reference doesn't exist. |

The SPI type and interface are owned by a provider abstraction package or
module separate from the base Framework API. The base Framework package
depends on this abstraction but doesn't expose Store operations as an
application API. An external provider must be able to implement only the
abstraction, without depending on the application/Actor/Spot package.

A separate public method or DTO isn't added per relocation phase,
manifest, participant, replay cursor, or completion. Redis key layout,
chunk storage structure, scripts, and cleanup index aren't exposed in the
public SPI either.

The following .NET excerpt shows the minimal shape of the common SPI. The
formal declaration is in the
[.NET Exact Interface](server/languages/dotnet/interfaces/08-authority-relocation.ko.md).

```csharp
public interface IZLinkRelocationStore
{
    // stores the payload without changing the framework-created reference.
    ValueTask<ZLinkBlobPutResult> PutAsync(
        ZLinkBlobReference reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    // reads the same reference's payload together with provider-based expiry information.
    ValueTask<ZLinkBlobReadResult> ReadAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default);

    // only extends the expiry time based on the provider clock, without changing the payload.
    ValueTask<ZLinkBlobRenewResult> RenewAsync(
        ZLinkBlobReference reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    // succeeds even if the reference doesn't exist, so it can be safely called again.
    ValueTask DeleteAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default);
}
```

The formal declaration for other languages follows the
[Java](server/languages/java/interfaces/location-maintenance.en.md),
[Kotlin](server/languages/kotlin/interfaces/location-maintenance.en.md),
[Node.js](server/languages/node/interfaces/08-location-maintenance.en.md),
and
[C++](server/languages/cpp/interfaces/07-location-store.en.md) exact
interfaces. A registration example for both Stores is kept only in
[Location Runtime](21-location-runtime.en.md#13-registration-conditions-and-lifetime).

## 3. Reference And Storage Size

The only values the provider needs to interpret are reference, payload,
and retention.

| Item | Contract |
|---|---|
| Reference | Opaque UTF-8 `1..4096` bytes the framework issues before `Put`. Compared case-sensitively for exact equality of the whole value. |
| Application data chunk | At most 64 MiB, measured in application bytes before the framework splits it. |
| Redis-encoded blob | The provider input combining the data chunk and the immutable envelope the framework attaches. The official Redis provider's maximum size is `64 MiB + 23 bytes`. |
| Payload split across multiple blobs | The whole payload the framework can compose from multiple blobs. Maximum size is 256 GiB. |
| Chunk count | The sum of data chunks the whole payload's leading index points to is at most 4,096. |
| Ordered stripe count | The contiguous segments splitting one relocation root for parallel storage and reading are at most 64. |
| `StoreNow` | The current time the provider includes in `Put`/`Read`/`Renew` results. Expiry is judged using this time and the provider clock. |

The provider doesn't create or change the reference. Even for the same
content, if the framework specifies different references, they're stored
as separate values. A deleted or expired reference must not be reused for
different bytes.

The framework splits a payload larger than 64 MiB into data chunks of at
most 64 MiB measured in application bytes. It attaches a checksum and a
23-byte immutable envelope needed for recovery to each chunk before
passing it to the Redis provider. So the application data limit and the
encoded blob limit the provider receives differ by 23 bytes. A separate
leading index records the format version, total length, checksum, chunk
order, and each chunk's reference/length/checksum. The provider also
stores this index as plain bytes. The framework confirms the index's
content and the chunk relationships.

The framework looks at participant count and payload size and evenly
splits the whole relocation root into at most 64 contiguous segments.
These segments are called ordered stripes. A stripe doesn't mean the
payload of a specific Actor or Spot. The Relocation Store doesn't
interpret a stripe's content — it stores it as opaque bytes.

If a stripe is larger than 64 MiB, it's split again into data chunks of
at most 64 MiB. The sum of data chunks pointed to by every stripe can't
exceed 4,096. Even if a SpotWide User Spot has 100 Actors, it doesn't
create one blob per participant — it processes up to 64 stripes in
parallel. The sum of encoded chunk bytes one process holds as a result of
parallel I/O doesn't exceed 256 MiB by default.

When storing, all data chunks are re-read to confirm bytes and checksum,
and only then is the leading index stored. If storing or confirming any
chunk fails, the leading index isn't stored. In this case, the remaining
chunks don't point to any Location Store record, so they're cleaned up
on retention expiry.

When restoring, data chunks are read in parallel and each checksum is
confirmed. Regardless of I/O completion order, they're combined in the
stripe and chunk order recorded in the leading index. The combined
bytes' overall checksum must also match before Spot state and Actor state
are restored. If even one stripe is missing or its checksum differs, the
whole relocation unit is treated as `DataLost`. Only some participants
aren't restored. When extending the retention period too, the existence
and checksum of each data chunk are confirmed in parallel, and only after
all succeed is the leading index's retention extended.

The bytes one application state adapter can return are also at most 64
MiB. The default 256 MiB limit applied when one process concurrently
processes relocation payloads is the Framework coordinator's in-flight
memory limit. This value doesn't change the storage size limit of one
blob or of the whole payload split across multiple blobs.

The default retention for each data chunk and the leading index is 24
hours, and the framework uses the point where 12 hours of retention
remain as the default renew threshold. The provider must compute expiry
using its own clock. The application host's wall clock isn't used to
judge expiry.

## 4. Result Per Operation

### 4.1 `Put`

`Put(reference, payload, retention)` returns only one of the following
results.

- `Stored(expiresAt, storeNow)`: the reference didn't exist, so the
  payload was newly stored.
- `AlreadyStored(expiresAt, storeNow)`: the same bytes are already stored
  at the same reference.
- `Conflict(storeNow)`: different bytes are stored at the same reference.

The provider compares the whole payload byte-for-byte. It doesn't provide
an API that issues a new reference for the same content or returns a
provider-chosen reference.

### 4.2 `Read`

`Read(reference)` returns `Found(bytes, expiresAt, storeNow)` if an
unexpired payload exists, and `Missing(storeNow)` if the reference
doesn't exist or has expired. `Found`'s bytes must not be changed while
the consumer is using them, or reused as the buffer for a different read
result.

### 4.3 `Renew`

`Renew(reference, retention)` computes a new expiry based on the provider
clock. If the payload exists, it returns `Renewed(expiresAt, storeNow)`,
and if the reference doesn't exist or has already expired, it returns
`Missing(storeNow)`. Repeating the same request doesn't change the
payload bytes.

### 4.4 `Delete`

`Delete(reference)` is an idempotent operation that succeeds even when
the reference doesn't exist. Running the same request multiple times
still ends in the same final state of the reference not existing.

## 5. Cancellation, Errors, And Result Reconstruction

If cancellation is requested before an operation starts, the provider
doesn't start I/O or write. If cancellation, timeout, or a transport
error occurs after an operation has started, whether the store or delete
was applied may be unknown. The provider must not assume this case is a
success or a normal result.

The official Redis provider's `OperationTimeout` applies to the whole
operation, combining the time to obtain a connection and the time for the
Redis command to finish. Once the time limit passes, the provider waiter
completes with the language-specific timeout, and the Framework public
operation converts this to `DeadlineExceeded`. A write already delivered
to Redis may still be applied after the timeout, so it isn't assumed to
have failed.

A framework that didn't receive a `Put` result must be able to
reconstruct whether it was stored, by running `Read` with the reference
it issued, or by running `Put` again with the same reference and the same
bytes. `Delete` reaches the same state even if run again, and `Renew`
also doesn't change the payload.

Violating an input contract such as reference length, payload size, or
retention returns a language-specific argument validation error.
`Missing`, `AlreadyStored`, and `Conflict` aren't provider failures — they
are normal results the caller can handle. Every other provider-specific
failure must be classifiable by the framework as a Store failure. Redis
command, key layout, and script information aren't exposed in the
application public API.

Input bytes passed by the caller must not change until the asynchronous
operation finishes. If the provider needs to reference the same memory
after the operation completes, it must copy the bytes first.

## 6. Payload Publication And Cleanup

A payload the Location Store authority doesn't yet point to is called an
orphan. If work is interrupted before Location Store publication, the
provider or Framework cleanup must remove that orphan after retention
expiry.

A published reference the Location Store authority points to may still
be needed for recovery. The framework must first commit the end of that
reference's use in the Location Store before deleting the payload. The
provider doesn't arbitrarily delete a published payload that still has
retention remaining. This publish/release order, and the `DataLost`
handling when a payload is missing, are defined by
[Location Runtime's Result Reconstruction Rule](21-location-runtime.en.md#8-when-a-store-response-isnt-received).

## 7. Registration And Provider Instance Lifetime

The registration conditions of a provider instance and the ownership of
the Framework root follow
[Location Runtime's Store Registration](21-location-runtime.en.md#13-registration-conditions-and-lifetime).
In a configuration where the framework owns the instance lifetime, it
disposes the instance exactly once after ending all runtime and
background operations that use the Store.

Multiple Store instances can share one physical connection. The
responsibility for deciding when to release the connection when each
instance is disposed, and for preventing a duplicate release, belongs to
the provider implementation.

## 8. The Official Redis Provider

The official Redis extension package provides a `RedisRelocationStore`
implementation matching each language's naming convention. Its public
options are limited to the connection, key namespace, and operation
timeout needed to create an instance.

The following items are internal to the Redis provider's implementation
and aren't the public contract.

- Redis key layout and chunk storage data structure
- Scripts and private serialization records
- Connection lease and cleanup index
- The internal method that runs retry and cleanup

A Redis-specific Framework registration helper, or a combined class that
implements Location Store and Relocation Store together, isn't provided.

Location Store and Relocation Store can use different key namespaces on
the same Redis deployment, or they can be placed on different
deployments. The correctness of the public contract doesn't depend on
connection sharing or a Redis transaction spanning both Stores.

## 9. Contract Test Requirements

The implementation and each language's contract tests must confirm the
following results.

- Re-running `Put` with the same reference and same bytes returns
  `AlreadyStored`; storing different bytes returns `Conflict`.
- Supports the encoded blob contract of 64 MiB application data plus a
  23-byte envelope, at most 4,096 data chunks, and a 256 GiB whole
  payload.
- Regardless of participant count, evenly splits the relocation root into
  at most 64 ordered opaque stripes, preserving original order and
  overall checksum.
- After not receiving a `Put` result, storage state can be reconstructed
  by an exact `Read` or by `Put` with the same input.
- The Redis operation timeout limits both connection acquisition and the
  actual command, and a write that completes after the timeout can be
  confirmed as `AlreadyStored` by retrying with the same reference and
  bytes.
- The bytes `Read` returns don't change while the consumer is using them.
- Re-running `Renew` and `Delete` doesn't change the payload, and expiry
  is computed with the provider clock.
- The payload isn't deleted before the Location Store commits the end of
  use for the published reference.
- A payload that failed before Location Store publication becomes an
  orphan-cleanup target after retention expiry.
- Location Store and Relocation Store can each be registered on the same
  Redis and on different Redis configurations.
- The Redis provider's public declarations don't include relocation
  phase/manifest DTOs, or script and key layout types.
