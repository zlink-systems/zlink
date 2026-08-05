namespace Zlink.Framework.LocationProvider;

public readonly record struct ZLinkBlobReference(string Value);

public abstract record ZLinkBlobPutResult
{
    private protected ZLinkBlobPutResult()
    {
    }

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
    private protected ZLinkBlobReadResult()
    {
    }

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
    private protected ZLinkBlobRenewResult()
    {
    }

    public sealed record Missing(DateTimeOffset StoreNow)
        : ZLinkBlobRenewResult;

    public sealed record Renewed(
        DateTimeOffset ExpiresAt,
        DateTimeOffset StoreNow)
        : ZLinkBlobRenewResult;
}

/// <summary>
/// Stores immutable relocation payload under a reference issued by Framework.
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
