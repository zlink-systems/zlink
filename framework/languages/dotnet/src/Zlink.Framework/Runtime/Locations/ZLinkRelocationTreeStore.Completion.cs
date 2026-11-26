using System.Globalization;
using System.Security.Cryptography;

namespace Zlink.Framework.Runtime.Locations;

internal static partial class ZLinkRelocationTreeStore
{
    private const string CompletionReferenceSuffix = ":completion";
    private static readonly TimeSpan CompletionRetention = TimeSpan.FromHours(24);

    internal static async ValueTask<(string Reference, string Sha256)> PutCompletionAsync(
        IZLinkRelocationRepository store,
        ZLinkRelocationCompletionRecord record,
        ReadOnlyMemory<byte> inventoryDigest,
        CancellationToken cancellationToken
    )
    {
        await using var payload = OpenCompletionFile();
        using var sha256 = SHA256.Create();
        using (
            var hashing = new CryptoStream(payload, sha256, CryptoStreamMode.Write, leaveOpen: true)
        )
        {
            await record.EncodeAsync(hashing, cancellationToken).ConfigureAwait(false);
            await hashing.FlushFinalBlockAsync(cancellationToken).ConfigureAwait(false);
        }
        payload.Position = 0;
        var tree = await PutStreamAsync(
                store,
                payload,
                inventoryDigest,
                CompletionRetention,
                cancellationToken,
                Guid.NewGuid().ToString("N") + CompletionReferenceSuffix
            )
            .ConfigureAwait(false);
        return (tree.Root.Reference, Convert.ToHexString(sha256.Hash!).ToLowerInvariant());
    }

    internal static async ValueTask<ZLinkRelocationCompletionRecord> ReadCompletionAsync(
        IZLinkRelocationRepository store,
        string reference,
        string expectedSha256,
        ReadOnlyMemory<byte> inventoryDigest,
        bool renew,
        CancellationToken cancellationToken
    )
    {
        var root = await store
            .GetRelocationAsync(reference, cancellationToken)
            .ConfigureAwait(false);
        if (root is not ZLinkRelocationReadResult.Found found)
            throw DataLost("Relocation completion manifest is missing.");
        var manifest = DecodeManifest(found.Payload.Span);
        if (!manifest.InventoryDigest.Span.SequenceEqual(inventoryDigest.Span))
            throw DataLost("Relocation completion manifest is inconsistent.");
        await using var payload = OpenCompletionFile();
        var crc = uint.MaxValue;
        using var sha256 = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var entry in manifest.Chunks)
        {
            var chunk = await ReadChunkAsync(
                    store,
                    entry,
                    cancellationToken,
                    renew ? CompletionRetention : null
                )
                .ConfigureAwait(false);
            ZLinkCrc32C.Append(ref crc, chunk.Data.Span);
            sha256.AppendData(chunk.Data.Span);
            await payload.WriteAsync(chunk.Data, cancellationToken).ConfigureAwait(false);
        }
        if (
            (ulong)payload.Length != manifest.LogicalLength
            || ~crc != manifest.LogicalChecksumCrc32c
        )
            throw DataLost("Relocation completion payload checksum differs.");
        var hash = sha256.GetHashAndReset();
        if (Convert.ToHexString(hash).ToLowerInvariant() != expectedSha256)
            throw DataLost("Relocation completion payload SHA-256 differs from authority.");
        payload.Position = 0;
        var record = await ZLinkRelocationCompletionRecord
            .DecodeAsync(payload, hash, cancellationToken)
            .ConfigureAwait(false);
        if (renew && found.ExpiresAt - found.StoreNow <= RenewThreshold)
            await RenewComponentAsync(store, reference, CompletionRetention, cancellationToken)
                .ConfigureAwait(false);
        return record;
    }

    private static string CompletionChunkReference(string manifest, int order) =>
        string.Concat(manifest, ":", order.ToString(CultureInfo.InvariantCulture));

    private static async ValueTask DeleteCompletionChunksAsync(
        IZLinkRelocationRepository store,
        string reference,
        CancellationToken cancellationToken
    )
    {
        var root = await store
            .GetRelocationAsync(reference, cancellationToken)
            .ConfigureAwait(false);
        if (root is not ZLinkRelocationReadResult.Found found)
            return;

        var manifest = DecodeManifest(found.Payload.Span);
        foreach (var chunk in manifest.Chunks)
            if (
                !StringComparer.Ordinal.Equals(
                    chunk.Reference,
                    CompletionChunkReference(reference, (int)chunk.Order)
                )
            )
                throw DataLost("Relocation completion manifest has an unrelated chunk reference.");

        foreach (var chunk in manifest.Chunks)
            _ = await store
                .DeleteRelocationAsync(chunk.Reference, cancellationToken)
                .ConfigureAwait(false);
    }

    private static FileStream OpenCompletionFile()
    {
        var options = new FileStreamOptions
        {
            Mode = FileMode.CreateNew,
            Access = FileAccess.ReadWrite,
            Share = FileShare.None,
            BufferSize = StreamBufferBytes,
            Options =
                FileOptions.Asynchronous | FileOptions.SequentialScan | FileOptions.DeleteOnClose,
        };
        if (!OperatingSystem.IsWindows())
            options.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;
        return new FileStream(Path.Combine(Path.GetTempPath(), Path.GetRandomFileName()), options);
    }
}
