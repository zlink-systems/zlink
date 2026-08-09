using System.Security.Cryptography;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

/// <summary>
/// Canonical in-memory <see cref="IZLinkRelocationRepository"/> test double.
/// References are content-addressed (SHA-256 hex) so repeated puts of the
/// same payload stay idempotent, matching provider store semantics.
/// </summary>
internal sealed class InMemoryRelocationStore : IZLinkRelocationRepository
{
    internal Dictionary<string, byte[]> Payloads { get; } =
        new(StringComparer.Ordinal);

    internal bool Contains(string reference) => Payloads.ContainsKey(reference);

    internal void Remove(string reference) => Payloads.Remove(reference);

    public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var bytes = payload.ToArray();
        var reference = Convert.ToHexString(SHA256.HashData(bytes));
        return ValueTask.FromResult(Store(reference, bytes, retention));
    }

    public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
        string reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var bytes = payload.ToArray();
        if (Payloads.TryGetValue(reference, out var current)
            && !current.AsSpan().SequenceEqual(bytes))
            throw new InvalidDataException("Relocation reference collision.");
        return ValueTask.FromResult(Store(reference, bytes, retention));
    }

    public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
        string reference,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult<ZLinkRelocationReadResult>(
            Payloads.TryGetValue(reference, out var payload)
                ? new ZLinkRelocationReadResult.Found(payload)
                : new ZLinkRelocationReadResult.Missing());

    public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
        string reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        var now = DateTimeOffset.UtcNow;
        return ValueTask.FromResult<ZLinkRelocationRenewResult>(
            Payloads.ContainsKey(reference)
                ? new ZLinkRelocationRenewResult.Renewed(now + retention, now)
                : new ZLinkRelocationRenewResult.Missing());
    }

    public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
        string reference,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult(
            Payloads.Remove(reference)
                ? ZLinkRelocationDeleteResult.Deleted
                : ZLinkRelocationDeleteResult.Missing);

    private ZLinkRelocationStored Store(
        string reference, byte[] bytes, TimeSpan retention)
    {
        Payloads[reference] = bytes;
        var now = DateTimeOffset.UtcNow;
        return new ZLinkRelocationStored(
            reference,
            ZLinkCrc32C.Compute(bytes),
            now + retention,
            now);
    }
}
