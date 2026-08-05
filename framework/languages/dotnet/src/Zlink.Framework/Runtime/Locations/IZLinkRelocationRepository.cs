namespace Zlink.Framework.Contracts.Locations;

internal interface IZLinkRelocationRepository
{
    ValueTask<ZLinkRelocationStored> PutRelocationAsync(
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
        string reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
        string reference,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
        string reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
        string reference,
        CancellationToken cancellationToken = default);
}
