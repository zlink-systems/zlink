---
title: "Location Store Provider SPI And The Official Redis Implementation"
---

# Location Store Provider SPI And The Official Redis Implementation

[Spec table of contents](README.en.md) · [Previous: Location Runtime](21-location-runtime.en.md) · [Next: Relocation Store Provider SPI And The Official Redis Implementation](23-relocation-store-redis.ko.md)

> **What this chapter defines** — the public SPI a Location Store provider must
> follow (conditional commit, page-bounded snapshot).


## 1. Scope And Audience

This document defines the public SPI a developer implementing a Location Store
provider must follow. The provider stores opaque keys and bytes the framework
builds. Condition checks and changes across several keys apply as one commit.
A recovery snapshot is provided with a bounded page size.

The provider doesn't need to know the meaning of Actor/Spot authority, owner
lease, placement reservation, aggregate commit, or relocation phase. How the
framework uses this SPI to build that state, and the Store registration
conditions, are owned by [Location Runtime](21-location-runtime.en.md). This
document doesn't repeat that behavior.

The application doesn't call this SPI's operations directly. Only a provider
package implements the SPI, and the framework uses the registered instance.

## 2. Responsibilities Of The Public SPI

The Location Store SPI only provides the following three operation groups.

| Operation group | Result the provider guarantees |
|---|---|
| Exact read | Returns the current bytes, provider version, optional expiry, and `StoreNow` for one opaque key, as a single observation. |
| Conditional atomic batch | Applies every mutation as one commit, only if every condition is true. |
| Snapshot scan | Continues reading a snapshot fixed at the first page, with a fixed page size and opaque cursor. |

The SPI type and interface are owned by a provider abstraction package or
module, separate from the base framework API. The base framework package
depends on the abstraction but doesn't re-expose Store operations as an
application API. An external provider must be able to implement only the
abstraction, without depending on the application/Actor/Spot package.

Per-descriptor, per-authority, per-reservation, per-capacity, per-aggregate,
per-lease, and per-change-stamp public methods or DTOs aren't added. Redis
commands, key layout, scripts, and private record encoding also aren't exposed
in the public SPI.

The following .NET excerpt shows the minimal shape of the common SPI. The
formal declaration is in the
[.NET exact interface](server/languages/dotnet/interfaces/08-authority-relocation.ko.md).

```csharp
public interface IZLinkLocationStore
{
    ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default);
}
```

The formal shape in other languages follows the
[Java](server/languages/java/interfaces/location-maintenance.en.md),
[Kotlin](server/languages/kotlin/interfaces/location-maintenance.en.md),
[Node.js](server/languages/node/interfaces/08-location-maintenance.en.md), and
[C++](server/languages/cpp/interfaces/07-location-store.en.md) exact
interfaces.
Registration examples for both Stores are kept only in
[Location Runtime](21-location-runtime.en.md#13-registration-conditions-and-lifetime).

## 3. Key, Value, Version, And Clock

| Item | Contract |
|---|---|
| Key | Opaque UTF-8 `1..1024` bytes the framework issues. Uses case-sensitive exact match — no normalization or case folding is applied. |
| Value | Bytes, at most 1 MiB. Unchanged after commit until that version is replaced or deleted. With no expiry, kept until explicit delete. |
| Version | Opaque UTF-8 `1..4096` bytes the provider issues. The framework doesn't interpret the value's size or internal makeup. |
| `StoreNow` | The provider wall clock that read, commit, and scan pages use as reference. TTL and expiry correctness only use this time. |

An exact read returns `Missing(StoreNow)` or
`Found(bytes, version, optional expiry, StoreNow)`. An expired value is
returned as `Missing`. The provider doesn't change bytes, or reuse them in a
different result buffer, while a consumer is using a read result.

Framework domain generation is different from provider version. The provider
doesn't interpret a domain counter or provide a separate generation API.

## 4. Conditional Atomic Batch

A write request is made of a condition set and a mutation set.

- `Missing(key)` is true only if the key doesn't exist or has expired.
- `Version(key, expected)` is true only if the current version is an exact
  match.
- `Put(key, bytes, optional retention)` issues a new opaque version.
- `Delete(key)` removes the key.

The provider checks every condition first. Only if all are true does it apply
every mutation as one commit. A different caller can't observe the commit's
intermediate state. If even one condition is false, it returns `Conflict`, and
mutation and version increments are zero. `Conflict` doesn't return the failed
condition or the current value.

The following bounds apply to a batch.

- The total unique keys appearing across conditions and mutations is at most
  2,048.
- An encoded request is at most 4 MiB.
- The same key isn't used twice within conditions or within mutations.
- `Applied` returns each `Put`'s opaque version and the single `StoreNow`
  observed at commit.

A whole User Spot participant set isn't put into a single batch. The framework
pre-stores immutable inventory chunks bounded to at most 1,024 entries and
1 MiB encoded. The final batch only puts in small records that must change
together at publish time — aggregate authority, inventory root/count/digest,
and capacity counter.

So the total number of Actors that can belong to one User Spot isn't set by
the batch's 2,048-key limit. The provider doesn't interpret the meaning of
inventory chunk, participant, or aggregate.

## 5. Size-Bounded Snapshot Scan

Snapshot scan lets recovery and maintenance read framework records at a
bounded size.

- Prefix is UTF-8 `0..1024` bytes and uses the same exact comparison as a key.
- The first page request has no cursor. The provider builds a bounded
  snapshot and returns an opaque cursor if there's a next page.
- The next page for the same cursor only reads the snapshot fixed initially.
- Page limit is `1..1000`, and encoded page size is at most 4 MiB.
- Cursor is opaque UTF-8 `1..4096` bytes.
- If the snapshot no longer exists or the cursor is invalid, `Expired` is
  returned.

On receiving `Expired`, the framework discards the previous page result and
re-reads from the first page. Since a scan item is only a recovery candidate,
it's re-checked with an exact read and expected-version condition before
mutation.

Cursor encoding, snapshot retention structure, and whether Redis `SCAN` is
used are provider implementation details.

## 6. Cancellation, Result Loss, And Errors

Cancellation before an operation starts blocks I/O and commit from starting.
If cancellation, timeout, or a transport error occurs after an operation
starts, whether it committed can be unclear. The provider doesn't assume
success or `Conflict` in this case. The framework must be able to reconstruct
the result via exact read and expected version.

An input bound violation, and a caller error specifying the same key twice
within conditions or mutations, are per-language argument validation errors.
`Missing`, `Conflict`, and `Expired` are normal closed results. A
provider-specific failure must be classifiable by the framework as a Store
failure, but Redis command, key layout, or script information isn't exposed
in the application public API.

Input bytes must not change until the async operation finishes. If the
provider needs to keep them afterward, it copies them. A success result's
bytes must stay stable while the consumer uses them.

## 7. Registration, Lifetime, And The Official Redis Provider

The provider instance's registration conditions and the framework root's
ownership follow
[Location Runtime's Store Registration](21-location-runtime.en.md#1-scope-and-responsibility).
In a configuration where the framework owns instance lifetime, it's disposed
exactly once after every runtime and background operation using the Store
finishes. Preventing duplicate dispose when several Stores share a physical
connection is the provider implementation's responsibility.

The official Redis extension package provides a `RedisLocationStore`
implementation matching each language's naming convention. Public options are
limited to the connection, key namespace, and operation timeout needed to
build the instance.

The following items are Redis provider implementation details, not part of
the public contract.

- Redis key and hash-tag layout
- Choice of HASH/SET/ZSET
- Lua script and transaction-splitting method
- Private record encoding and schema markers
- Connection lease, retry, and snapshot cursor implementation
- Change stamp and polling optimization

The Redis provider must also support §4's generic atomic batch and §5's
snapshot scan as-is. It doesn't expose a domain-specific Redis method,
descriptor/authority DTO, or change-stamp capability interface.

The Location Store and Relocation Store can use different key namespaces on
the same Redis deployment, or be physically separated. Correctness doesn't
depend on connection sharing or a cross-store Redis transaction.

## 8. Contract Test

- Exact read returns bytes, version, optional expiry, and `StoreNow` as one
  observation.
- An expired value is `Missing` by the provider clock, and a durable value is
  kept until explicit delete.
- If one condition fails, every mutation and version increment is zero.
- Up to 2,048 unique keys and a 4 MiB encoded request apply as one atomic
  commit.
- Scan pages use the same snapshot, and `Expired` is returned if the snapshot
  or cursor is invalid.
- The cursor round-trips opaquely up to 4,096 bytes, and pages respect the
  1,000-item/4 MiB bound.
- After cancellation or result loss, whether it committed can be
  reconstructed via exact read and version.
- The Redis provider's public declaration has no authority/reservation/
  aggregate DTO, script, or key-layout type.
- The Location Store and Relocation Store can each be registered and used on
  the same Redis or on separate Redis configurations.
