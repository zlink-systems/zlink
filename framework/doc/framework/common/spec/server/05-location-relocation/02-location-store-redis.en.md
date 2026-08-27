---
title: "Location Store Provider SPI And The Official Redis Implementation"
---

# Location Store Provider SPI And The Official Redis Implementation

[Location And Relocation topic index](README.en.md) · [Spec index](../README.en.md) · [Previous: 01. Location Runtime](01-location-runtime.en.md) · [Next: 03. Relocation Store (Redis)](03-relocation-store-redis.en.md)

> Defines the public SPI a provider must follow (conditional
> commit, page-bounded snapshot), and the key/byte format the official Redis
> implementation must follow for cross-language interoperability.

## 1. Scope And Audience

The storage that holds the current owner and state of a
[Spot](../00-foundation/02-glossary.en.md#spot) — a logical target that
receives messages — and an Actor, so multiple nodes can check them together,
is the [Location Store](../00-foundation/02-glossary.en.md#location-store).
This document defines the public SPI a developer implementing that provider
must follow. The provider stores opaque keys and bytes the
framework builds. Condition checks and changes across several keys apply as
one commit. A recovery snapshot is provided with a bounded page size.

The provider doesn't need to know the meaning of Actor/Spot authority, owner
lease, placement reservation, aggregate commit, or relocation phase. How the
framework uses this SPI to build that state, and the Store registration
conditions, are owned by [Location Runtime](01-location-runtime.en.md). This
document doesn't repeat that behavior.

The application doesn't call this SPI's operations directly. Only a provider
package implements the SPI, and the framework uses the registered instance.

**This document carries two layers of contract together.** §2~§6 are the SPI
contract every provider implementing a Location Store — including a
non-Redis provider — must satisfy; if the code disagrees, the code is fixed.
§8~§9 are the implementation contract that fixes the Redis key layout and
data type the official Redis provider itself uses. Within §8~§9, the part
that Redis providers in different languages must be able to read from each
other — the counter-issuance key, and the opaque storage format for the five
records — is a **MUST-level public contract**, and if the code disagrees, the
code is fixed; a non-Redis provider doesn't need to follow this part. The
rest — implementation detail the Redis provider chooses internally, such as
Lua script splitting or connection management — is called out explicitly at
the end of §9, and that part is updated in the document, not the code, if the
implementation changes.

## 2. Responsibilities Of The Public SPI

The Location Store SPI only provides the following three operation groups.

| Operation group | Result the provider guarantees |
|---|---|
| Direct read | Returns the current bytes, provider version, optional expiry, and `StoreNow` for one opaque key, as a single observation. |
| Conditional atomic batch | Applies every mutation as one commit, only if every condition is true. |
| Snapshot scan | Continues reading a [snapshot](../00-foundation/02-glossary.en.md#snapshot) fixed at the first page, with a fixed page size and opaque cursor. |

The provider abstraction package boundary that the SPI type and interface
follow is defined by
[Location Runtime §2.1](01-location-runtime.en.md#21-package-principles-shared-by-the-two-spis).
Per-descriptor, per-authority, per-reservation, per-capacity, per-aggregate,
per-lease, and per-change-stamp public methods or DTOs aren't added. Redis
commands, key layout, scripts, and private record encoding also aren't
exposed in the public SPI.

The following .NET excerpt shows the minimal shape of the common SPI. The
formal declaration is in the
[.NET per-language interface](../languages/dotnet/interfaces/08-authority-relocation.en.md).

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
[Java](../languages/java/interfaces/location-maintenance.en.md),
[Kotlin](../languages/kotlin/interfaces/location-maintenance.en.md),
[Node.js](../languages/node/interfaces/08-location-maintenance.en.md), and
[C++](../languages/cpp/interfaces/07-location-store.en.md) per-language interfaces.
Registration examples for both Stores are kept only in
[Location Runtime §2](01-location-runtime.en.md#2-roles-and-responsibilities--provider-vs-framework).

## 3. Key, Value, Version, And Clock

| Item | Contract |
|---|---|
| Key | Opaque UTF-8 `1..1024` bytes the framework issues. Compared byte-for-byte, case-sensitive — no normalization or case folding is applied. |
| Value | Bytes, at most 1 MiB. Unchanged after commit until that version is replaced or deleted. With no expiry, kept until explicit delete. |
| Version | Opaque UTF-8 `1..4096` bytes the provider issues. The framework doesn't interpret the value's size or internal makeup. |
| `StoreNow` | The provider wall clock that read, commit, and scan pages use as reference. TTL and expiry correctness only use this time. |

A direct read returns `Missing(StoreNow)` or
`Found(bytes, version, optional expiry, StoreNow)`. An expired value is
returned as `Missing`. The provider doesn't change bytes, or reuse them in a
different result buffer, while a consumer is using a read result.

Framework domain generation is different from provider version. The provider
doesn't interpret a domain counter or provide a separate generation API.

## 4. Conditional Atomic Batch

A write request is made of a condition set and a mutation set.

- `Missing(key)` is true only if the key doesn't exist or has expired.
- `Version(key, expected)` is true only if the current version equals
  `expected`.
- `Put(key, bytes, optional retention)` issues a new opaque version.
- `Delete(key)` removes the key.

**The provider checks every condition first. Only if all are true does it
apply every mutation as one commit.** A different caller can't observe the
commit's intermediate state. If even one condition is false, it returns
`Conflict`, and mutation and version increments are zero. `Conflict` doesn't
return the failed condition or the current value.

The following bounds apply to a batch.

- The total unique keys appearing across conditions and mutations is at most
  2,048.
- An encoded request is at most 4 MiB.
- The same key isn't used twice within conditions or within mutations.
- `Applied` returns each `Put`'s opaque version and the single `StoreNow`
  observed at commit.

A whole User Spot participant set isn't put into a single batch. The
framework pre-stores immutable inventory chunks bounded to at most 1,024
entries per relocation-target-list page and 1 MiB encoded — this page bound
is a separate figure from the CAS batch's unique-key-total bound above, and
applies to a different target. The final batch only puts in small records
that must change together at publish time — aggregate authority, inventory
root/count/digest, and capacity counter.

So the total number of Actors that can belong to one User Spot isn't set by
the batch's 2,048-key limit. The provider doesn't interpret the meaning of
inventory chunk, participant, or aggregate.

## 5. Size-Bounded Snapshot Scan

Snapshot scan lets recovery and maintenance read framework records at a
bounded size.

- Prefix is UTF-8 `0..1024` bytes and is compared byte-for-byte, case-sensitive,
  the same way as a key.
- The first page request has no cursor. The provider builds a bounded
  snapshot and returns an opaque cursor if there's a next page.
- The next page for the same cursor only reads the snapshot fixed initially.
- Page limit is `1..1000`, and encoded page size is at most 4 MiB.
- Cursor is opaque UTF-8 `1..4096` bytes.
- If the snapshot no longer exists or the cursor is invalid, `Expired` is
  returned.

On receiving `Expired`, the framework discards the previous page result and
re-reads from the first page. Since a scan item is only a recovery candidate,
it's re-checked with a direct read and expected-version condition before
mutation.

Cursor encoding, snapshot retention structure, and whether Redis `SCAN` is
used are provider implementation details — whatever the Redis provider
chooses is irrelevant to this contract (§9).

## 6. Cancellation, Result Loss, And Errors

Cancellation before an operation starts blocks I/O and commit from starting.
If cancellation, timeout, or a transport error occurs after an operation
starts, whether it committed can be unclear. The provider doesn't assume
success or `Conflict` in this case. The framework must be able to reconstruct
the result via a direct read and expected version.

An input bound violation, and a caller error specifying the same key twice
within conditions or mutations, are per-language argument validation errors.
`Missing`, `Conflict`, and `Expired` are normal closed results. A
provider-specific failure must be classifiable by the framework as a Store
failure, but Redis command, key layout, or script information isn't exposed
in the application public API.

Input bytes must not change until the async operation finishes. If the
provider needs to keep them afterward, it copies them. A success result's
bytes must stay stable while the consumer uses them.

## 7. Registration And Provider Instance Lifetime

The provider instance's registration conditions and the framework root's
ownership follow
[Location Runtime §2](01-location-runtime.en.md#2-roles-and-responsibilities--provider-vs-framework).
In a configuration where the framework owns instance lifetime, it's disposed
exactly once after every runtime and background operation using the Store
finishes. Preventing duplicate dispose when several Stores share a physical
connection is the provider implementation's responsibility.

This registration condition is an SPI contract independent of provider kind.
From here on, this document defines the key and data type the official
Redis provider actually uses to satisfy that contract.

## 8. Official Redis Provider — Counter Issuance

The official Redis extension package provides a `RedisLocationStore`
implementation matching each language's naming convention. Public options
are limited to the connection, key namespace, and operation timeout needed
to build the instance.

**The Redis key and data type described from this section through §9 are
not an internal implementation choice left to the provider — they are a
MUST-level public contract that lets official Redis providers in different
languages read the same record.** If the code disagrees, the code is fixed.
This contract applies only to the official Redis extension package; a
non-Redis provider, or a provider that uses Redis but isn't the official
extension, only needs to satisfy the SPI in §2~§6 and doesn't need to follow
this key format.

The following logical keys are one cross-language contract; they are not
provider versions or Redis implementation counters. Each counter issues an
[OwnerLeaseGeneration](../00-foundation/02-glossary.en.md#owner-lease-generation)
value, which distinguishes the host process lifecycle the current owner
belongs to, an
[ObjectGeneration](../00-foundation/02-glossary.en.md#objectgeneration)
value, which distinguishes different logical incarnations of the same Actor
or Spot identity, or an
[AuthorityOwnerGeneration](../00-foundation/02-glossary.en.md#authority-owner-generation)
value, which marks the order in which the authority owner changed within
the same object incarnation.

| Logical key | Issued value |
|---|---|
| `zlink:v11:owner-counter` | `OwnerLeaseGeneration` (unchanged) |
| `zlink:v11:object-counter` | `ObjectGeneration` |
| `zlink:v11:authority-owner-counter` | `AuthorityOwnerGeneration` |

Each value is bare UTF-8 canonical decimal: no sign, leading zero, JSON
envelope, or `recordVersion`. A missing row means the next value is `1`;
issuing `v` CAS-puts `v + 1`. A block of `n` issues `v..v+n-1` and puts
`v+n`. The valid stored counter range is `1..2^63-1`; `0` is never stored or
issued. A row storing `2^63-1` is exhausted and returns the typed
`GenerationExhausted` result without changing either record or counter.
Therefore, the maximum issued value is `2^63-2`.

**The counter mutation must be in the same conditional write batch as the
record it gates (one `EVAL`).** The provider mapping
`{prefix}:{zlink-location-v3}:opaque:{sha256hex(logicalKey)}` gives every
logical counter the same `{zlink-location-v3}` hash slot automatically.
Counter logical keys remain outside the `authority\0` and descriptor
scan-preimage prefixes.

**Operations clean break:** flush once per Store the physical opaque keys
computed as SHA-256 of these retired logical literals:
`zlink:v11:counter:object`, `zlink:v11:counter:authority-owner`,
`zlink:v11:authority:object-generation-counter`,
`zlink:v11:authority:owner-generation-counter`, and
`zlink:v11:authority-generations`. Recompute each physical key from its
literal with the mapping above; do not guess it from a record or scan
prefix.

## 9. The Official Redis Provider — 5-Record Opaque Storage Format

A [MeshNode descriptor](../00-foundation/02-glossary.en.md#meshnode-descriptor) —
the registration a [MeshNode](../00-foundation/02-glossary.en.md#meshnode)
(a runtime node that sends or receives messages)
publishes for automatic discovery, so other nodes
can find its identity and connection information — an owner lease,
ClientServer server descriptor, fanout
publisher descriptor, and authority record must use the same opaque record
representation regardless of language — otherwise a record one language
writes can't be read by another. These five records **must** follow this
storage scheme.

The Redis key is `{prefix}:{zlink-location-v3}:opaque:{sha256hex(preimage)}`,
where `{prefix}` is the key namespace the provider specifies at registration
and `preimage` is the per-record logical key preimage defined by
[Location Runtime §3.4](01-location-runtime.en.md#34-how-different-languages-read-and-write-the-same-redis-record)
(note — this `sha256hex(preimage)` uses different input than §8's
`sha256hex(logicalKey)`: the counter hashes the short literal logical key
as-is, while these five records hash a preimage string built per record).

The braces around `{zlink-location-v3}` are a Redis Cluster hashtag —
because `Put` changes the record, sequence counter, and index together in
one script (§4), the whole domain must be pinned to a single hash slot for
that multi-key `EVAL` to stay atomic under Cluster; without the braces,
Cluster could scatter the keys across different slots and break that
atomicity.

The data structure is a Redis `ZSET`; each `Put` appends to the log with the
provider's monotonically increasing `INCR` counter as the score — the member
with the highest score is the current value. The member value is a cmsgpack
array holding `{originalKey, rawBytes(value), version, expiresAtMs,
tombstone}`, prefixed with a 1-byte format tag `0x01`. `rawBytes` carries the
original bytes as-is, without re-encoding to base64. The provider fails
explicitly on an unrecognized format tag rather than guessing how to read
the value.

`cmsgpack` means the standard MessagePack encoding Redis's Lua `cmsgpack`
library produces, and the array's five members use these MessagePack types —
pinned explicitly because a generic encoder's defaults (for example a `bin`
family for byte strings) don't match Lua `cmsgpack`'s output byte-for-byte.

| Member | MessagePack type |
|---|---|
| `originalKey` | `str` family (fixstr/str8/str16/str32 by length) — never `bin` |
| `rawBytes` | `str` family, same rule — Lua strings carry raw bytes without a separate binary type |
| `version` | `str` family — an opaque `StoreVersion` string, not a number |
| `expiresAtMs` | unsigned `int` family (positive fixint/uint8/uint16/uint32/uint64 by magnitude); `0` means no expiry |
| `tombstone` | `bool` (`0xc2` false / `0xc3` true) |

The outer array itself uses the `array` family (fixarray for 5 elements).

There's no backward-compatible path that reads state already stored in Redis
under the old key/value format and converts it to the new opaque record. A
deployment upgrading to this format is a **clean break** — existing Redis
state must be drained or its loss accepted, and a mix of format tags or
`recordVersion` values fails explicitly instead of silently picking one to
read.

The Location Store, and the
[Relocation Store](../00-foundation/02-glossary.en.md#relocation-store) that
keeps the activation envelope needed for cold activation and the reply
payload returned after relocation completes as opaque bytes, can use
different key namespaces on the same Redis deployment, or be physically
separated. Correctness doesn't
depend on connection sharing or a cross-store Redis transaction.

**This is the end of the MUST-level public contract.** Outside these five
records and their opaque record representation, the following items are
implementation details the Redis provider chooses freely, and aren't part
of the public contract — if the document disagrees with the code, the
document is updated to match the code.

- Lua script and transaction-splitting method
- Connection lease, retry, and snapshot cursor implementation
- Change stamp and polling optimization

The Redis provider must also support §4's generic atomic batch and §5's
snapshot scan as-is. It doesn't expose an authority DTO or change-stamp
capability interface.

## 10. Verification Requirements

Public surface — the return values of the Location Store SPI's three
operations, and the key/value bytes the official Redis provider verifies
with the store record golden fixture — alone confirm the following. Each
item maps to one test.

**SPI (Common To Every Provider)**

- A direct read returns bytes, version, optional expiry, and `StoreNow` as one
  observation.
- An expired value is `Missing` by the provider clock, and a durable value is
  kept until explicit delete.
- If one condition fails, every mutation and version increment is zero.
- Up to 2,048 unique keys and a 4 MiB encoded request apply as one atomic
  commit.
- Scan pages use the same snapshot, and `Expired` is returned if the
  snapshot or cursor is invalid.
- The cursor round-trips opaquely up to 4,096 bytes, and pages respect the
  1,000-item/4 MiB bound.
- After cancellation or result loss, whether it committed can be
  reconstructed via a direct read and version.
- The Redis provider's public declaration has no authority/reservation/
  aggregate DTO, script, or key-layout type.
- The Location Store and Relocation Store can each be registered and used
  on the same Redis or on separate Redis configurations.

**Official Redis Provider's Key/Byte Format**

- The MeshNode descriptor, owner lease, ClientServer server descriptor, and
  fanout publisher descriptor are verified by a conformance test that
  consumes the store record golden fixture's
  (`framework/runtime/protocol/golden/store-record-v1.json`) key-derivation
  vectors (preimage → SHA-256 → full key string) and value byte vectors
  (including tombstone and expired variants) as-is.
- An unrecognized format tag or `recordVersion` fails explicitly, and
  there's no backward-compatible path that reads the old key/value format
  and converts it to the new opaque record.

---

[Location And Relocation topic index](README.en.md) · [Spec index](../README.en.md) · [Previous: 01. Location Runtime](01-location-runtime.en.md) · [Next: 03. Relocation Store (Redis)](03-relocation-store-redis.en.md)
