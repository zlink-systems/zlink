using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkRelocationTreeStored(
    ZLinkRelocationStored Root,
    long LogicalLength,
    uint LogicalChecksumCrc32c,
    int ChunkCount);

internal sealed record ZLinkRelocationTreeRead(
    ZLinkRelocationEnvelope Envelope,
    long LogicalLength,
    uint LogicalChecksumCrc32c,
    int ChunkCount);

/// <summary>
/// Persists the logical relocation stream as canonical ZLTC chunks and a
/// canonical ZLTM manifest. A temporary file keeps memory proportional to the
/// active chunk instead of the complete aggregate.
/// </summary>
internal static partial class ZLinkRelocationTreeStore
{
    internal const int ChunkBytes = 64 * 1024 * 1024;
    internal const int MaxChunks = 4096;
    internal const ulong MaxLogicalBytes = 256UL * 1024 * 1024 * 1024;
    // Component concurrency and its encoded byte budget stay private so
    // applications and providers do not need to coordinate I/O scheduling.
    internal const int MaxConcurrentComponentIo = 64;
    internal const long MaxComponentIoBytes = 256L * 1024 * 1024;
    internal const int MaxOrderedStripes = 64;

    private const int FrameHeaderBytes = 11;
    private const int FrameChecksumBytes = 4;
    private const int ChunkBodyPrefixBytes = 8;
    private const int InventoryDigestBytes = 32;
    private const int MaxManifestBytes = 64 * 1024 * 1024;
    private const int MaxReferenceBytes = 4096;
    private static readonly TimeSpan RenewThreshold = TimeSpan.FromHours(12);
    private static readonly byte[] ChunkMagic = "ZLTC"u8.ToArray();
    private static readonly byte[] ManifestMagic = "ZLTM"u8.ToArray();

    internal static async ValueTask<ZLinkRelocationTreeStored> PutAsync(
        IZLinkRelocationRepository store,
        ZLinkRelocationEnvelope envelope,
        TimeSpan retention,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(store);
        ArgumentNullException.ThrowIfNull(envelope);
        if (envelope.InventoryDigest.Length != InventoryDigestBytes)
            throw new InvalidOperationException(
                "Relocation inventory digest must be SHA-256.");
        if (CanUseParticipantComponents(envelope))
            return await PutParticipantComponentsAsync(
                    store,
                    envelope,
                    retention,
                    cancellationToken)
                .ConfigureAwait(false);

        var path = Path.GetTempFileName();
        try
        {
            await using (var output = new FileStream(
                             path,
                             FileMode.Truncate,
                             FileAccess.Write,
                             FileShare.None,
                             1024 * 1024,
                             FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                ZLinkRelocationEnvelopeCodec.EncodeTo(output, envelope);
                await output.FlushAsync(cancellationToken).ConfigureAwait(false);
            }

            var logicalLength = new FileInfo(path).Length;
            if (logicalLength <= 0 || (ulong)logicalLength > MaxLogicalBytes)
                throw new InvalidOperationException(
                    "Relocation logical stream exceeds its 256 GiB bound.");
            var chunkCount = CalculateChunkCount(
                checked((ulong)logicalLength));

            var chunks = new ChunkEntry[chunkCount];
            var logicalCrcState = uint.MaxValue;
            await using var input = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                1024 * 1024,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            for (var firstOrder = 0;
                 firstOrder < chunkCount;)
            {
                var batch = new List<EncodedChunk>(
                    MaxConcurrentComponentIo);
                long batchBytes = 0;
                while (firstOrder + batch.Count < chunkCount
                       && batch.Count < MaxConcurrentComponentIo)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    var order = firstOrder + batch.Count;
                    var dataLength = checked((int)Math.Min(
                        ChunkBytes,
                        logicalLength - input.Position));
                    var encoded = new byte[
                        FrameHeaderBytes + ChunkBodyPrefixBytes
                        + dataLength + FrameChecksumBytes];
                    if (batch.Count != 0
                        && batchBytes
                        > MaxComponentIoBytes - encoded.LongLength)
                        break;
                    WriteFrameHeader(
                        encoded,
                        ChunkMagic,
                        checked((uint)(ChunkBodyPrefixBytes + dataLength)));
                    BinaryPrimitives.WriteUInt32BigEndian(
                        encoded.AsSpan(FrameHeaderBytes, 4),
                        checked((uint)order));
                    BinaryPrimitives.WriteUInt32BigEndian(
                        encoded.AsSpan(FrameHeaderBytes + 4, 4),
                        checked((uint)dataLength));
                    await input.ReadExactlyAsync(
                            encoded.AsMemory(
                                FrameHeaderBytes + ChunkBodyPrefixBytes,
                                dataLength),
                            cancellationToken)
                        .ConfigureAwait(false);
                    var dataOffset =
                        FrameHeaderBytes + ChunkBodyPrefixBytes;
                    var dataChecksum = ZLinkCrc32C.Compute(
                        encoded.AsSpan(dataOffset, dataLength));
                    ZLinkCrc32C.Append(
                        ref logicalCrcState,
                        encoded.AsSpan(dataOffset, dataLength));
                    WriteFrameChecksum(encoded);
                    batch.Add(new EncodedChunk(
                        order,
                        dataLength,
                        dataChecksum,
                        encoded));
                    batchBytes = checked(
                        batchBytes + encoded.LongLength);
                }

                var storedBatch = await Task.WhenAll(
                        batch.Select(chunk => PutVerifiedAsync(
                                store,
                                chunk.Encoded,
                                retention,
                                cancellationToken)
                            .AsTask()))
                    .ConfigureAwait(false);
                for (var index = 0; index < batch.Count; index++)
                {
                    var chunk = batch[index];
                    var stored = storedBatch[index];
                    chunks[chunk.Order] = new ChunkEntry(
                        checked((uint)chunk.Order),
                        stored.Reference,
                        checked((ulong)chunk.DataLength),
                        chunk.DataChecksumCrc32c);
                }
                firstOrder += batch.Count;
            }

            var logicalChecksum = ~logicalCrcState;
            var manifest = EncodeManifest(
                checked((ulong)logicalLength),
                logicalChecksum,
                envelope.InventoryDigest.Span,
                chunks);
            var root = await PutVerifiedAsync(
                    store,
                    manifest,
                    retention,
                    cancellationToken)
                .ConfigureAwait(false);
            return new ZLinkRelocationTreeStored(
                root,
                logicalLength,
                logicalChecksum,
                chunks.Length);
        }
        finally
        {
            File.Delete(path);
        }
    }


    internal static int CalculateChunkCount(ulong logicalLength)
    {
        if (logicalLength is 0 or > MaxLogicalBytes)
            throw new InvalidOperationException(
                "Relocation logical stream exceeds its 256 GiB bound.");
        var count = checked((int)(
            (logicalLength + (ulong)ChunkBytes - 1) / (ulong)ChunkBytes));
        if (count > MaxChunks)
            throw new InvalidOperationException(
                "Relocation logical stream exceeds 4096 chunks.");
        return count;
    }

    internal static async ValueTask<ZLinkRelocationEnvelope> GetAsync(
        IZLinkRelocationRepository store,
        string rootReference,
        uint? expectedRootChecksum,
        CancellationToken cancellationToken)
    {
        var read = await ReadAsync(
                store,
                rootReference,
                expectedRootChecksum,
                cancellationToken)
            .ConfigureAwait(false);
        return read.Envelope;
    }

    internal static async ValueTask<ZLinkRelocationTreeRead> ReadAsync(
        IZLinkRelocationRepository store,
        string rootReference,
        uint? expectedRootChecksum,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(store);
        ArgumentException.ThrowIfNullOrWhiteSpace(rootReference);
        var root = await store.GetRelocationAsync(
                rootReference,
                cancellationToken)
            .ConfigureAwait(false);
        if (root is not ZLinkRelocationReadResult.Found found)
            throw DataLost($"Relocation manifest '{rootReference}' is unavailable.");
        if (expectedRootChecksum is { } checksum
            && ZLinkCrc32C.Compute(found.Payload.Span) != checksum)
            throw DataLost($"Relocation manifest '{rootReference}' checksum is invalid.");
        if (IsParticipantManifest(found.Payload.Span))
            return await ReadParticipantComponentsAsync(
                    store,
                    found.Payload,
                    cancellationToken)
                .ConfigureAwait(false);
        var manifest = DecodeManifest(found.Payload.Span);

        var path = Path.GetTempFileName();
        try
        {
            var crcState = uint.MaxValue;
            ulong length = 0;
            await using (var output = new FileStream(
                             path,
                             FileMode.Truncate,
                             FileAccess.Write,
                             FileShare.None,
                             1024 * 1024,
                             FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                for (var firstIndex = 0;
                     firstIndex < manifest.Chunks.Count;)
                {
                    var count = SelectBatchCount(
                        manifest.Chunks,
                        firstIndex);
                    var batch = await Task.WhenAll(
                            manifest.Chunks
                                .Skip(firstIndex)
                                .Take(count)
                                .Select(entry => ReadChunkAsync(
                                        store,
                                        entry,
                                        cancellationToken)
                                    .AsTask()))
                        .ConfigureAwait(false);
                    for (var index = 0; index < batch.Length; index++)
                    {
                        var chunk = batch[index];
                        if (chunk.Order
                            != manifest.Chunks[firstIndex + index].Order)
                            throw DataLost(
                                "Relocation chunk completion order is invalid.");
                        ZLinkCrc32C.Append(
                            ref crcState,
                            chunk.Data.Span);
                        length = checked(
                            length + (uint)chunk.Data.Length);
                        await output.WriteAsync(
                                chunk.Data,
                                cancellationToken)
                            .ConfigureAwait(false);
                    }
                    firstIndex += count;
                }
                await output.FlushAsync(cancellationToken).ConfigureAwait(false);
            }
            if (length != manifest.LogicalLength
                || ~crcState != manifest.LogicalChecksumCrc32c)
                throw DataLost("Relocation logical stream checksum is invalid.");

            ZLinkRelocationEnvelope envelope;
            await using (var input = new FileStream(
                             path,
                             FileMode.Open,
                             FileAccess.Read,
                             FileShare.Read,
                             1024 * 1024,
                             FileOptions.SequentialScan))
                envelope = ZLinkRelocationEnvelopeCodec.Decode(
                    input,
                    manifest.InventoryDigest);
            if (!envelope.InventoryDigest.Span.SequenceEqual(
                    manifest.InventoryDigest.Span))
                throw DataLost(
                    "Relocation manifest inventory digest does not match the logical root.");
            return new ZLinkRelocationTreeRead(
                envelope,
                checked((long)manifest.LogicalLength),
                manifest.LogicalChecksumCrc32c,
                manifest.Chunks.Count);
        }
        finally
        {
            File.Delete(path);
        }
    }

    internal static async ValueTask RenewTreeAsync(
        IZLinkRelocationRepository store,
        string rootReference,
        uint expectedRootChecksum,
        TimeSpan retention,
        CancellationToken cancellationToken)
    {
        var root = await store.GetRelocationAsync(
                rootReference,
                cancellationToken)
            .ConfigureAwait(false);
        if (root is not ZLinkRelocationReadResult.Found found
            || ZLinkCrc32C.Compute(found.Payload.Span) != expectedRootChecksum)
            throw DataLost(
                $"Relocation manifest '{rootReference}' cannot be renewed.");
        if (IsParticipantManifest(found.Payload.Span))
        {
            await RenewParticipantComponentsAsync(
                    store,
                    found.Payload,
                    retention,
                    cancellationToken)
                .ConfigureAwait(false);
            await RenewComponentAsync(
                    store,
                    rootReference,
                    retention,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        var manifest = DecodeManifest(found.Payload.Span);
        for (var firstIndex = 0;
             firstIndex < manifest.Chunks.Count;)
        {
            var count = SelectBatchCount(
                manifest.Chunks,
                firstIndex);
            await Task.WhenAll(
                    manifest.Chunks
                        .Skip(firstIndex)
                        .Take(count)
                        .Select(entry => VerifyAndRenewChunkAsync(
                                store,
                                entry,
                                retention,
                                cancellationToken)
                            .AsTask()))
                .ConfigureAwait(false);
            firstIndex += count;
        }
        await RenewComponentAsync(
                store,
                rootReference,
                retention,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static async ValueTask VerifyAndRenewChunkAsync(
        IZLinkRelocationRepository store,
        ChunkEntry entry,
        TimeSpan retention,
        CancellationToken cancellationToken)
    {
        _ = await ReadChunkAsync(
                store,
                entry,
                cancellationToken)
            .ConfigureAwait(false);
        await RenewComponentAsync(
                store,
                entry.Reference,
                retention,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal static async ValueTask DeleteTreeAsync(
        IZLinkRelocationRepository store,
        string rootReference,
        CancellationToken cancellationToken)
    {
        // Chunks are content-addressed and can be shared by another immutable
        // root. Without a Store-level reference count, eagerly deleting them
        // can corrupt a published successor. Removing the manifest makes this
        // tree unreachable; component TTL performs bounded orphan cleanup.
        _ = await store.DeleteRelocationAsync(
                rootReference,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static async ValueTask RenewComponentAsync(
        IZLinkRelocationRepository store,
        string reference,
        TimeSpan retention,
        CancellationToken cancellationToken)
    {
        var result = await store.RenewRelocationAsync(
                reference,
                retention,
                cancellationToken)
            .ConfigureAwait(false);
        if (result is not ZLinkRelocationRenewResult.Renewed renewed
            || renewed.ExpiresAt - renewed.StoreNow <= RenewThreshold)
            throw DataLost(
                $"Relocation tree component '{reference}' could not be renewed.");
    }

    private static async ValueTask<ZLinkRelocationStored> PutVerifiedAsync(
        IZLinkRelocationRepository store,
        ReadOnlyMemory<byte> encoded,
        TimeSpan retention,
        CancellationToken cancellationToken)
    {
        var stored = await store.PutRelocationAsync(
                encoded,
                retention,
                cancellationToken)
            .ConfigureAwait(false);
        var checksum = ZLinkCrc32C.Compute(encoded.Span);
        if (string.IsNullOrWhiteSpace(stored.Reference)
            || stored.ChecksumCrc32c != checksum
            || stored.ExpiresAt - stored.StoreNow <= RenewThreshold)
            throw DataLost("Relocation Store returned an invalid immutable reference.");
        var read = await store.GetRelocationAsync(
                stored.Reference,
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkRelocationReadResult.Found found
            || !found.Payload.Span.SequenceEqual(encoded.Span))
            throw DataLost(
                $"Relocation Store did not preserve '{stored.Reference}'.");
        return stored;
    }

    private static int SelectBatchCount(
        IReadOnlyList<ChunkEntry> chunks,
        int firstIndex) =>
        CalculateComponentBatchCountCore(
            chunks.Count,
            firstIndex,
            index => chunks[index].Length);

    internal static int CalculateComponentBatchCount(
        IReadOnlyList<ulong> dataLengths,
        int firstIndex) =>
        CalculateComponentBatchCountCore(
            dataLengths?.Count
            ?? throw new ArgumentNullException(nameof(dataLengths)),
            firstIndex,
            index => dataLengths[index]);

    private static int CalculateComponentBatchCountCore(
        int componentCount,
        int firstIndex,
        Func<int, ulong> dataLengthAt)
    {
        if (firstIndex < 0 || firstIndex >= componentCount)
            throw new ArgumentOutOfRangeException(nameof(firstIndex));
        var count = 0;
        long bytes = 0;
        while (firstIndex + count < componentCount
               && count < MaxConcurrentComponentIo)
        {
            var dataLength = checked(
                (long)dataLengthAt(firstIndex + count));
            if (dataLength is < 1 or > ChunkBytes)
                throw new ArgumentOutOfRangeException(nameof(dataLengthAt));
            var encodedLength = checked(
                dataLength
                + FrameHeaderBytes
                + ChunkBodyPrefixBytes
                + FrameChecksumBytes);
            if (count != 0
                && bytes > MaxComponentIoBytes - encodedLength)
                break;
            bytes = checked(bytes + encodedLength);
            count++;
        }
        return count;
    }

    private static async ValueTask<DecodedChunk> ReadChunkAsync(
        IZLinkRelocationRepository store,
        ChunkEntry entry,
        CancellationToken cancellationToken)
    {
        var read = await store.GetRelocationAsync(
                entry.Reference,
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkRelocationReadResult.Found chunk)
            throw DataLost(
                $"Relocation chunk '{entry.Reference}' is unavailable.");
        var dataLength = DecodeChunk(chunk.Payload.Span, entry);
        return new DecodedChunk(
            entry.Order,
            chunk.Payload.Slice(
                FrameHeaderBytes + ChunkBodyPrefixBytes,
                dataLength));
    }

    private static byte[] EncodeManifest(
        ulong logicalLength,
        uint logicalChecksum,
        ReadOnlySpan<byte> inventoryDigest,
        IReadOnlyList<ChunkEntry> chunks)
    {
        var references = chunks
            .Select(static entry => Encoding.UTF8.GetBytes(entry.Reference))
            .ToArray();
        if (references.Any(static value =>
                value.Length is < 1 or > MaxReferenceBytes))
            throw new InvalidOperationException(
                "A relocation reference exceeds its UTF-8 bound.");
        var bodyLength = checked(
            1 + 8 + 4 + 1 + InventoryDigestBytes + 4
            + chunks.Select((entry, index) =>
                    4 + 2 + references[index].Length + 8 + 4)
                .Sum());
        var encoded = new byte[
            FrameHeaderBytes + bodyLength + FrameChecksumBytes];
        WriteFrameHeader(encoded, ManifestMagic, checked((uint)bodyLength));
        var offset = FrameHeaderBytes;
        encoded[offset++] = 1;
        BinaryPrimitives.WriteUInt64BigEndian(
            encoded.AsSpan(offset, 8), logicalLength);
        offset += 8;
        BinaryPrimitives.WriteUInt32BigEndian(
            encoded.AsSpan(offset, 4), logicalChecksum);
        offset += 4;
        encoded[offset++] = InventoryDigestBytes;
        inventoryDigest.CopyTo(encoded.AsSpan(offset, InventoryDigestBytes));
        offset += InventoryDigestBytes;
        BinaryPrimitives.WriteUInt32BigEndian(
            encoded.AsSpan(offset, 4), checked((uint)chunks.Count));
        offset += 4;
        for (var index = 0; index < chunks.Count; index++)
        {
            var entry = chunks[index];
            BinaryPrimitives.WriteUInt32BigEndian(
                encoded.AsSpan(offset, 4), entry.Order);
            offset += 4;
            BinaryPrimitives.WriteUInt16BigEndian(
                encoded.AsSpan(offset, 2),
                checked((ushort)references[index].Length));
            offset += 2;
            references[index].CopyTo(encoded.AsSpan(offset));
            offset += references[index].Length;
            BinaryPrimitives.WriteUInt64BigEndian(
                encoded.AsSpan(offset, 8), entry.Length);
            offset += 8;
            BinaryPrimitives.WriteUInt32BigEndian(
                encoded.AsSpan(offset, 4), entry.ChecksumCrc32c);
            offset += 4;
        }
        WriteFrameChecksum(encoded);
        if (encoded.Length > MaxManifestBytes)
            throw new InvalidOperationException(
                "Relocation manifest exceeds 64 MiB.");
        return encoded;
    }

    private static Manifest DecodeManifest(ReadOnlySpan<byte> encoded)
    {
        var body = DecodeFrame(encoded, ManifestMagic);
        var offset = 0;
        if (ReadByte(body, ref offset) != 1)
            throw DataLost("Relocation manifest logical version is invalid.");
        var logicalLength = ReadUInt64(body, ref offset);
        var logicalChecksum = ReadUInt32(body, ref offset);
        if (ReadByte(body, ref offset) != InventoryDigestBytes)
            throw DataLost("Relocation manifest inventory digest is invalid.");
        var digest = ReadBytes(body, ref offset, InventoryDigestBytes).ToArray();
        var count = ReadUInt32(body, ref offset);
        if (logicalLength is 0 or > MaxLogicalBytes
            || count is 0 or > MaxChunks)
            throw DataLost("Relocation manifest bounds are invalid.");
        var chunks = new ChunkEntry[checked((int)count)];
        ulong total = 0;
        for (var index = 0; index < chunks.Length; index++)
        {
            var order = ReadUInt32(body, ref offset);
            var referenceLength = ReadUInt16(body, ref offset);
            if (order != (uint)index
                || referenceLength is 0 or > MaxReferenceBytes)
                throw DataLost("Relocation manifest chunk order is invalid.");
            var reference = new UTF8Encoding(false, true).GetString(
                ReadBytes(body, ref offset, referenceLength));
            var length = ReadUInt64(body, ref offset);
            var checksum = ReadUInt32(body, ref offset);
            if (length is 0 or > ChunkBytes
                || index < chunks.Length - 1 && length != ChunkBytes)
                throw DataLost("Relocation manifest chunk length is invalid.");
            total = checked(total + length);
            chunks[index] = new ChunkEntry(order, reference, length, checksum);
        }
        if (offset != body.Length || total != logicalLength)
            throw DataLost("Relocation manifest length is invalid.");
        return new Manifest(logicalLength, logicalChecksum, digest, chunks);
    }

    private static int DecodeChunk(
        ReadOnlySpan<byte> encoded,
        ChunkEntry expected)
    {
        var body = DecodeFrame(encoded, ChunkMagic);
        var offset = 0;
        var order = ReadUInt32(body, ref offset);
        var length = ReadUInt32(body, ref offset);
        if (order != expected.Order || length != expected.Length
            || length is 0 or > ChunkBytes
            || body.Length - offset != checked((int)length))
            throw DataLost("Relocation chunk envelope is invalid.");
        var data = body[offset..];
        if (ZLinkCrc32C.Compute(data) != expected.ChecksumCrc32c)
            throw DataLost("Relocation chunk data checksum is invalid.");
        return data.Length;
    }

    private static void WriteFrameHeader(
        Span<byte> encoded,
        ReadOnlySpan<byte> magic,
        uint bodyLength)
    {
        magic.CopyTo(encoded);
        encoded[4] = 1;
        BinaryPrimitives.WriteUInt16BigEndian(encoded[5..7], 0);
        BinaryPrimitives.WriteUInt32BigEndian(encoded[7..11], bodyLength);
    }

    private static void WriteFrameChecksum(Span<byte> encoded) =>
        BinaryPrimitives.WriteUInt32BigEndian(
            encoded[^FrameChecksumBytes..],
            ZLinkCrc32C.Compute(encoded[..^FrameChecksumBytes]));

    private static ReadOnlySpan<byte> DecodeFrame(
        ReadOnlySpan<byte> encoded,
        ReadOnlySpan<byte> magic)
    {
        if (encoded.Length < FrameHeaderBytes + FrameChecksumBytes
            || !encoded[..4].SequenceEqual(magic)
            || encoded[4] != 1
            || BinaryPrimitives.ReadUInt16BigEndian(encoded[5..7]) != 0)
            throw DataLost("Relocation frame header is invalid.");
        var bodyLength = BinaryPrimitives.ReadUInt32BigEndian(encoded[7..11]);
        if (encoded.Length != FrameHeaderBytes
            + checked((int)bodyLength) + FrameChecksumBytes
            || ZLinkCrc32C.Compute(encoded[..^FrameChecksumBytes])
               != BinaryPrimitives.ReadUInt32BigEndian(
                   encoded[^FrameChecksumBytes..]))
            throw DataLost("Relocation frame length or checksum is invalid.");
        return encoded.Slice(FrameHeaderBytes, checked((int)bodyLength));
    }

    private static byte ReadByte(ReadOnlySpan<byte> value, ref int offset)
    {
        if ((uint)offset >= (uint)value.Length)
            throw DataLost("Relocation manifest is truncated.");
        return value[offset++];
    }

    private static ushort ReadUInt16(ReadOnlySpan<byte> value, ref int offset)
    {
        var bytes = ReadBytes(value, ref offset, 2);
        return BinaryPrimitives.ReadUInt16BigEndian(bytes);
    }

    private static uint ReadUInt32(ReadOnlySpan<byte> value, ref int offset)
    {
        var bytes = ReadBytes(value, ref offset, 4);
        return BinaryPrimitives.ReadUInt32BigEndian(bytes);
    }

    private static ulong ReadUInt64(ReadOnlySpan<byte> value, ref int offset)
    {
        var bytes = ReadBytes(value, ref offset, 8);
        return BinaryPrimitives.ReadUInt64BigEndian(bytes);
    }

    private static ReadOnlySpan<byte> ReadBytes(
        ReadOnlySpan<byte> value,
        ref int offset,
        int length)
    {
        if (length < 0 || offset > value.Length - length)
            throw DataLost("Relocation manifest is truncated.");
        var result = value.Slice(offset, length);
        offset += length;
        return result;
    }

    private static ZLinkRelocationDataLostException DataLost(string message) =>
        new(message);

    private sealed record Manifest(
        ulong LogicalLength,
        uint LogicalChecksumCrc32c,
        ReadOnlyMemory<byte> InventoryDigest,
        IReadOnlyList<ChunkEntry> Chunks);

    private sealed record ChunkEntry(
        uint Order,
        string Reference,
        ulong Length,
        uint ChecksumCrc32c);

    private sealed record EncodedChunk(
        int Order,
        int DataLength,
        uint DataChecksumCrc32c,
        ReadOnlyMemory<byte> Encoded);

    private sealed record DecodedChunk(
        uint Order,
        ReadOnlyMemory<byte> Data);
}
