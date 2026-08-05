# .NET Location/Relocation Provider Public Interface

[.NET exact interface table of contents](README.en.md) · [Location Runtime](../../../../21-location-runtime.en.md) ·
[Location Store Provider](../../../../22-location-store-redis.en.md) ·
[Relocation Store Provider](../../../../23-relocation-store-redis.en.md)

## 1. Scope

This document fixes the exact C# declaration of the minimal Store SPI an
external provider author implements. The provider only implements
conditional atomic batch on opaque key/value, and the operation that
stores an immutable blob at a framework-issued reference.

Authority, owner lease, descriptor, reservation, capacity, aggregate, and
relocation phase are framework-private records. This document doesn't
declare a public method/result/DTO for those domains.

The primitive types and the two Store interfaces are provided by a
separate `Zlink.Framework.Provider.Abstractions` package.

## 2. Location Store

```csharp
namespace Zlink.Framework.LocationProvider;

public readonly record struct ZLinkStoreKey(string Value);
public readonly record struct ZLinkStoreVersion(string Value);
public readonly record struct ZLinkStoreScanCursor(string Value);

public sealed record ZLinkStoreValue(
    ReadOnlyMemory<byte> Bytes,
    ZLinkStoreVersion Version,
    DateTimeOffset? ExpiresAt,
    DateTimeOffset StoreNow);

public abstract record ZLinkStoreReadResult
{
    private protected ZLinkStoreReadResult() { }

    public sealed record Missing(DateTimeOffset StoreNow)
        : ZLinkStoreReadResult;

    public sealed record Found(ZLinkStoreValue Value)
        : ZLinkStoreReadResult;
}

public abstract record ZLinkStoreCondition
{
    private protected ZLinkStoreCondition() { }

    public sealed record Missing(ZLinkStoreKey Key)
        : ZLinkStoreCondition;

    public sealed record Version(
        ZLinkStoreKey Key,
        ZLinkStoreVersion Expected)
        : ZLinkStoreCondition;
}

public abstract record ZLinkStoreMutation
{
    private protected ZLinkStoreMutation() { }

    public sealed record Put(
        ZLinkStoreKey Key,
        ReadOnlyMemory<byte> Bytes,
        TimeSpan? Retention)
        : ZLinkStoreMutation;

    public sealed record Delete(ZLinkStoreKey Key)
        : ZLinkStoreMutation;
}

public sealed record ZLinkStoreWriteRequest(
    IReadOnlyList<ZLinkStoreCondition> Conditions,
    IReadOnlyList<ZLinkStoreMutation> Mutations);

public abstract record ZLinkStoreWriteResult
{
    private protected ZLinkStoreWriteResult() { }

    public sealed record Applied(
        IReadOnlyDictionary<ZLinkStoreKey, ZLinkStoreVersion> PutVersions,
        DateTimeOffset StoreNow)
        : ZLinkStoreWriteResult;

    public sealed record Conflict(DateTimeOffset StoreNow)
        : ZLinkStoreWriteResult;
}

public sealed record ZLinkStoreScanRequest(
    string Prefix,
    ZLinkStoreScanCursor? Cursor,
    int Limit);

public sealed record ZLinkStoreScanPage(
    IReadOnlyList<KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>> Items,
    ZLinkStoreScanCursor? NextCursor,
    DateTimeOffset StoreNow);

public abstract record ZLinkStoreScanResult
{
    private protected ZLinkStoreScanResult() { }

    public sealed record Page(ZLinkStoreScanPage Value)
        : ZLinkStoreScanResult;

    public sealed record Expired : ZLinkStoreScanResult;
}

/// <summary>
/// Stores opaque Framework records and applies a bounded conditional batch
/// as one atomic commit.
/// </summary>
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

Key is opaque UTF-8 `1..1024` bytes, and version is provider-issued
opaque UTF-8 `1..4096` bytes. Value bytes are at most 1 MiB.
`ExpiresAt == null` is a durable value, and TTL is judged based on the
`StoreNow` included in the same result.

`WriteAsync(...)` checks every condition, and only if all are true does
it apply the whole mutation as one commit. The sum of unique keys across
conditions and mutations is at most 2,048, and the encoded request is at
most 4 MiB. A duplicate condition or duplicate mutation on the same key
is `ArgumentException`. If even one condition is false, it's `Conflict`,
and mutation and version increase are 0.

Scan limit is `1..1000`, and page encoded size is at most 4 MiB. A
subsequent page with the same cursor uses the snapshot created on the
first page. Cursor is opaque UTF-8 `1..4096` bytes. If the snapshot can't
be kept, it's `Expired`, and the framework discards the partial result
and reads from the beginning again.

## 3. Relocation Store

```csharp
namespace Zlink.Framework.LocationProvider;

public readonly record struct ZLinkBlobReference(string Value);

public abstract record ZLinkBlobPutResult
{
    private protected ZLinkBlobPutResult() { }

    public sealed record Stored(
        DateTimeOffset ExpiresAt,
        DateTimeOffset StoreNow)
        : ZLinkBlobPutResult;

    public sealed record AlreadyStored(
        DateTimeOffset ExpiresAt,
        DateTimeOffset StoreNow)
        : ZLinkBlobPutResult;

    public sealed record Conflict(DateTimeOffset StoreNow)
        : ZLinkBlobPutResult;
}

public abstract record ZLinkBlobReadResult
{
    private protected ZLinkBlobReadResult() { }

    public sealed record Missing(DateTimeOffset StoreNow)
        : ZLinkBlobReadResult;

    public sealed record Found(
        ReadOnlyMemory<byte> Bytes,
        DateTimeOffset ExpiresAt,
        DateTimeOffset StoreNow)
        : ZLinkBlobReadResult;
}

public abstract record ZLinkBlobRenewResult
{
    private protected ZLinkBlobRenewResult() { }

    public sealed record Missing(DateTimeOffset StoreNow)
        : ZLinkBlobRenewResult;

    public sealed record Renewed(
        DateTimeOffset ExpiresAt,
        DateTimeOffset StoreNow)
        : ZLinkBlobRenewResult;
}

/// <summary>
/// Stores immutable relocation payload under a reference issued by Framework.
/// It does not interpret relocation phases, manifests, or participants.
/// </summary>
public interface IZLinkRelocationStore
{
    ValueTask<ZLinkBlobPutResult> PutAsync(
        ZLinkBlobReference reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkBlobReadResult> ReadAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkBlobRenewResult> RenewAsync(
        ZLinkBlobReference reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    ValueTask DeleteAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default);
}
```

Reference is opaque UTF-8 `1..4096` bytes the framework issues before
put. Retrying with the same reference and same bytes returns
`AlreadyStored`; different bytes returns `Conflict`. A deleted or expired
reference also isn't reused for different content.

A data chunk splitting application state is at most 64 MiB. The
framework attaches a 23-byte immutable envelope in front of each chunk.
So the encoded blob `IZLinkRelocationStore.PutAsync(...)` receives is at
most `64 MiB + 23 bytes`. The framework composes a logical stream of at
most 256 GiB from at most 4,096 data chunks and an immutable root
manifest. The framework computes and verifies the payload checksum and
the root/chunk relationship.

Read result bytes aren't changed while the consumer is using them. Renew
and delete are idempotent, and delete is a successful no-op even when the
reference doesn't exist.

## 4. Cancellation And Result Reconciliation

If cancellation is requested before the call, the provider doesn't start
I/O or commit. If cancellation, timeout, or transport failure occurs
after the call has started, whether the commit was applied may be
uncertain.

The framework reconciles the result using the Location Store's exact
read and version, or the Relocation Store's caller-issued reference.
`Conflict`, `Missing`, `Expired`, and `AlreadyStored` are closed normal
results. A Store call exception that isn't `ArgumentException` or
`OperationCanceledException` is classified by the framework as a
provider failure.

## 5. Lifetime

Once registration succeeds, the Store instance's lifetime is owned by the
framework. If the Store implements `IAsyncDisposable` or `IDisposable`,
the framework ends the dependent runtime first and then disposes it
exactly once.

When two Stores share a connection, the provider manages the connection
lease each Store's dispose releases. After registration, the application
doesn't call Store operations directly or replace/dispose the instance.
