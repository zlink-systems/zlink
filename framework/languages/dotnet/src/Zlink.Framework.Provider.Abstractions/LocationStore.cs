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
    private protected ZLinkStoreReadResult()
    {
    }

    public sealed record Missing(DateTimeOffset StoreNow)
        : ZLinkStoreReadResult;

    public sealed record Found(ZLinkStoreValue Value)
        : ZLinkStoreReadResult;
}

public abstract record ZLinkStoreCondition
{
    private protected ZLinkStoreCondition()
    {
    }

    public sealed record Missing(ZLinkStoreKey Key)
        : ZLinkStoreCondition;

    public sealed record Version(
        ZLinkStoreKey Key,
        ZLinkStoreVersion Expected)
        : ZLinkStoreCondition;
}

public abstract record ZLinkStoreMutation
{
    private protected ZLinkStoreMutation()
    {
    }

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
    private protected ZLinkStoreWriteResult()
    {
    }

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
    private protected ZLinkStoreScanResult()
    {
    }

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
