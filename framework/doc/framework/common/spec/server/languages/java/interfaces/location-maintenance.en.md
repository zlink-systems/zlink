# Java Location/Relocation Public Interface

[Java exact interface list](README.en.md) · [Common Location Runtime](../../../../21-location-runtime.en.md) ·
[Common Redis Provider](../../../../22-location-store-redis.en.md)

This document only defines the Java public contract an application and
provider plugin author need to know. Authority, owner lease,
reservation, capacity, aggregate, and the relocation state machine are
composed by the framework as private records. A provider doesn't
interpret the meaning of a record — it only stores opaque key/value and
immutable blobs.

The Store primitives and interfaces are owned by the
`systems.zlink.framework.locationprovider` package in the opt-in
artifact `systems.zlink:zlink-framework-provider-abstractions`. A
provider implementation can implement the Store contract using only this
artifact, without depending on the Actor/Spot application package.

## Provider Registration And Lifetime

The application registers the two Stores separately with the existing
`ZLinkFrameworkOptions.addLocationStore(...)` and
`addRelocationStore(...)`. A separate helper that bundles both Stores or
registers Redis directly isn't provided.

Once registration succeeds, the Store instance's lifetime transfers to
the framework. If the Store implements `AutoCloseable`, the framework
ends the dependent runtime first and then closes it exactly once. When
two Stores share a connection, the provider manages the connection lease
each Store releases.

## Location Store

```java
public record ZLinkStoreKey(String value) {}
public record ZLinkStoreVersion(String value) {}
public record ZLinkStoreScanCursor(String value) {}

public record ZLinkStoreValue(
    byte[] bytes,
    ZLinkStoreVersion version,
    Instant expiresAt,
    Instant storeNow) {}

public sealed interface ZLinkStoreReadResult
    permits ZLinkStoreReadMissing, ZLinkStoreReadFound {}

public record ZLinkStoreReadMissing(Instant storeNow)
    implements ZLinkStoreReadResult {}

public record ZLinkStoreReadFound(ZLinkStoreValue value)
    implements ZLinkStoreReadResult {}

public sealed interface ZLinkStoreCondition
    permits ZLinkStoreMissingCondition, ZLinkStoreVersionCondition {}

public record ZLinkStoreMissingCondition(ZLinkStoreKey key)
    implements ZLinkStoreCondition {}

public record ZLinkStoreVersionCondition(
    ZLinkStoreKey key,
    ZLinkStoreVersion expected)
    implements ZLinkStoreCondition {}

public sealed interface ZLinkStoreMutation
    permits ZLinkStorePut, ZLinkStoreDelete {}

public record ZLinkStorePut(
    ZLinkStoreKey key,
    byte[] bytes,
    Duration retention)
    implements ZLinkStoreMutation {}

public record ZLinkStoreDelete(ZLinkStoreKey key)
    implements ZLinkStoreMutation {}

public record ZLinkStoreWriteRequest(
    List<ZLinkStoreCondition> conditions,
    List<ZLinkStoreMutation> mutations) {}

public sealed interface ZLinkStoreWriteResult
    permits ZLinkStoreWriteApplied, ZLinkStoreWriteConflict {}

public record ZLinkStoreWriteApplied(
    Map<ZLinkStoreKey, ZLinkStoreVersion> putVersions,
    Instant storeNow)
    implements ZLinkStoreWriteResult {}

public record ZLinkStoreWriteConflict(Instant storeNow)
    implements ZLinkStoreWriteResult {}

public record ZLinkStoreScanRequest(
    String prefix,
    ZLinkStoreScanCursor cursor,
    int limit) {}

public record ZLinkStoreScanItem(
    ZLinkStoreKey key,
    ZLinkStoreValue value) {}

public record ZLinkStoreScanPage(
    List<ZLinkStoreScanItem> items,
    ZLinkStoreScanCursor nextCursor,
    Instant storeNow) {}

public sealed interface ZLinkStoreScanResult
    permits ZLinkStoreScanPageResult, ZLinkStoreScanExpired {}

public record ZLinkStoreScanPageResult(ZLinkStoreScanPage value)
    implements ZLinkStoreScanResult {}

public record ZLinkStoreScanExpired()
    implements ZLinkStoreScanResult {}

public interface ZLinkLocationStore {
    CompletionStage<ZLinkStoreReadResult> read(
        ZLinkStoreKey key,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkStoreWriteResult> write(
        ZLinkStoreWriteRequest request,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkStoreScanResult> scan(
        ZLinkStoreScanRequest request,
        ZLinkStoreCancellation cancellation);
}
```

If `ZLinkStorePut.retention == null`, it's a durable value that doesn't
expire. `ZLinkStoreScanRequest.cursor == null` is the first page, and
`ZLinkStoreScanPage.nextCursor == null` is the last page. The returned
`byte[]` isn't changed by the provider while the caller uses the result,
and isn't reused as a different result's buffer.

### Values And Time

- Key is opaque UTF-8 `1..1024` bytes the framework issues, compared as
  case-sensitive exact match.
- Version is provider-issued opaque UTF-8 `1..4096` bytes. The
  framework and provider don't interpret its internal structure or
  numeric magnitude.
- Value is at most 1 MiB. Expiry time is judged based on the provider
  clock.
- `storeNow` is the provider clock value obtained from the same read,
  commit, or scan page. The framework's local clock isn't used for TTL
  correctness.

### Atomic Write

`write(...)` first checks every condition, and only if all are true does
it apply every mutation as one commit. If even one is false, it's
`ZLinkStoreWriteConflict`, and mutation and version increase are 0. A
different caller can't observe an intermediate state of the commit.

- A missing condition is only true when the key doesn't exist or has
  expired.
- A version condition is only true when the current version is an exact
  match.
- The sum of unique keys across conditions and mutations is at most
  2,048.
- The request's encoded size is at most 4 MiB.
- A duplicate condition or duplicate mutation on the same key isn't
  allowed.
- The applied result returns the new version the provider issued for
  each put.
- The conflict result doesn't disclose the failed condition or current
  value. The framework re-confirms the needed key with an exact read.

### Snapshot Scan

`scan(...)` is the required operation recovery and maintenance use to
find a bounded key set.

- Prefix is UTF-8 `0..1024` bytes, using the same exact comparison as
  key.
- A subsequent cursor page also uses the snapshot the first page fixed.
- Limit is `1..1000`. If the encoded page reaches 4 MiB first, fewer
  items can be returned.
- Cursor is opaque UTF-8 `1..4096` bytes.
- If the provider can't keep the snapshot any longer, it returns
  `ZLinkStoreScanExpired`. The framework discards the partial result and
  reads from the first page again.

## Relocation Store

```java
public record ZLinkBlobReference(String value) {}

public sealed interface ZLinkBlobPutResult
    permits ZLinkBlobStored, ZLinkBlobAlreadyStored, ZLinkBlobConflict {}

public record ZLinkBlobStored(
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkBlobPutResult {}

public record ZLinkBlobAlreadyStored(
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkBlobPutResult {}

public record ZLinkBlobConflict(Instant storeNow)
    implements ZLinkBlobPutResult {}

public sealed interface ZLinkBlobReadResult
    permits ZLinkBlobMissing, ZLinkBlobFound {}

public record ZLinkBlobMissing(Instant storeNow)
    implements ZLinkBlobReadResult {}

public record ZLinkBlobFound(
    byte[] bytes,
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkBlobReadResult {}

public sealed interface ZLinkBlobRenewResult
    permits ZLinkBlobRenewMissing, ZLinkBlobRenewed {}

public record ZLinkBlobRenewMissing(Instant storeNow)
    implements ZLinkBlobRenewResult {}

public record ZLinkBlobRenewed(
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkBlobRenewResult {}

public interface ZLinkRelocationStore {
    CompletionStage<ZLinkBlobPutResult> put(
        ZLinkBlobReference reference,
        byte[] payload,
        Duration retention,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkBlobReadResult> read(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkBlobRenewResult> renew(
        ZLinkBlobReference reference,
        Duration retention,
        ZLinkStoreCancellation cancellation);

    CompletionStage<Void> delete(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation);
}
```

Reference is opaque UTF-8 `1..4096` bytes the framework issues before
put, compared as case-sensitive exact match. Re-putting the same
reference with the same bytes returns `ZLinkBlobAlreadyStored`; with
different bytes, `ZLinkBlobConflict`. A deleted or expired reference also
isn't reused for different content.

One blob is at most 64 MiB. The framework splits a logical relocation
stream of at most 256 GiB into at most 4,096 64-MiB chunks and an
immutable root manifest. The framework computes and verifies checksum
and the root/chunk relationship, and the provider doesn't interpret the
manifest.

Read returns the original bytes and expiry based on the provider clock.
Renew and delete retries are idempotent, and delete is a successful
no-op even when the reference doesn't exist. Since the framework issues
the reference in advance, storage state can be reconciled after a
timeout or lost result by reading the same reference exactly.

## Cancellation And Errors

`ZLinkStoreCancellation` only expresses cancellation of a provider I/O
operation. If cancellation is requested before the call, the provider
doesn't start I/O or commit. If cancellation, timeout, or a transport
error occurs after the call has started, whether the commit was applied
may be uncertain, and the framework reconciles the result with an exact
read and version, or a caller-issued blob reference.

An input range violation and a duplicate on the same key are
`IllegalArgumentException`. Conflict, Missing, Expired, and
AlreadyStored are closed normal results. Every other Store call
exception is classified by the framework as a provider failure.

## Operational Query

```java
public interface ZLinkLocationRuntimeQuery {
    CompletionStage<ZLinkLocationRuntimeStatus> getStatus();
    CompletionStage<ZLinkLocationPage<ZLinkLocationTopologyEntry>> listTopology(
        ZLinkLocationTopologyFilter filter,
        ZLinkPageRequest page);
    CompletionStage<ZLinkLocationPage<ZLinkLocationServiceSummary>> listServiceSummaries(
        ZLinkLocationServiceSummaryFilter filter,
        ZLinkPageRequest page);
}

public interface ZLinkLocationReadiness {
    CompletionStage<Boolean> isPeerReady(
        String meshName,
        ZLinkLocationRole role,
        RoutingId nodeRid);
}
```

An operational query only returns a bounded page. Raw Spot/Actor
authority rows, Store keys, scan cursors, and provider version aren't
included in the application query contract.

## Redis Extension

```java
public final class ZLinkRedisLocationStore
    implements ZLinkLocationStore, AutoCloseable {
    public ZLinkRedisLocationStore(ZLinkRedisLocationOptions options);
}

public final class ZLinkRedisRelocationStore
    implements ZLinkRelocationStore, AutoCloseable {
    public ZLinkRedisRelocationStore(ZLinkRedisRelocationOptions options);
}

public final class ZLinkRedisLocationOptions {
    public String connectionString();
    public ZLinkRedisLocationOptions setConnectionString(String value);
    public String keyPrefix();
    public ZLinkRedisLocationOptions setKeyPrefix(String value);
    public Duration operationTimeout();
    public ZLinkRedisLocationOptions setOperationTimeout(Duration value);
}

public final class ZLinkRedisRelocationOptions {
    public String connectionString();
    public ZLinkRedisRelocationOptions setConnectionString(String value);
    public String keyPrefix();
    public ZLinkRedisRelocationOptions setKeyPrefix(String value);
    public Duration operationTimeout();
    public ZLinkRedisRelocationOptions setOperationTimeout(Duration value);
}
```

The Redis public surface is limited to the two Store classes' minimal
constructor, and connection/key namespace/operation timeout options.
Key layout, Lua script, private record encoding, retry, and shared
connection reference count are implementation details.

## Contract Not Made Public

The following items aren't an interface a provider or application
implements or calls.

- Store capability per Authority, owner lease, reservation, capacity, and
  aggregate
- Runtime publisher, resolver, cache, retry coordinator, and recovery
  state machine
- Redis script client, key codec, row serializer, and connection lease
- Watch publisher, change-stamp event, and raw peer/Spot/Actor/route
  Store
- Routing-ID slot, allocation group, and allocated-RID provider

The provider's public declarations must not show Authority, Reservation,
Aggregate, Capacity, Fence, or relocation phase types.
