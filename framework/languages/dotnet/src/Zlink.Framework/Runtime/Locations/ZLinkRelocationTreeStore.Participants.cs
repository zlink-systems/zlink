using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Locations;

internal static partial class ZLinkRelocationTreeStore
{
    private const int ParticipantManifestFixedBodyBytes =
        1 + 1 + 8 + 4 + 1 + InventoryDigestBytes + 16 + 8 + 4;
    private const int MinimumParticipantManifestBytes = 39;
    private const int MinimumParticipantChunkBytes = 19;
    private static readonly byte[] ParticipantManifestMagic =
        "ZLPM"u8.ToArray();

    private static bool CanUseParticipantComponents(
        ZLinkRelocationEnvelope envelope) =>
        envelope.Participants.Count > 1;

    private static bool IsParticipantManifest(ReadOnlySpan<byte> encoded) =>
        encoded.Length >= ParticipantManifestMagic.Length
        && encoded[..ParticipantManifestMagic.Length]
            .SequenceEqual(ParticipantManifestMagic);

    private static async ValueTask<ZLinkRelocationTreeStored>
        PutParticipantComponentsAsync(
            IZLinkRelocationRepository store,
            ZLinkRelocationEnvelope envelope,
            TimeSpan retention,
            CancellationToken cancellationToken)
    {
        string? logicalPath = null;
        try
        {
            long sourceLength;
            if (envelope.CanonicalLogicalStream.IsEmpty)
            {
                logicalPath = Path.GetTempFileName();
                await using (var output = new FileStream(
                                 logicalPath,
                                 FileMode.Truncate,
                                 FileAccess.Write,
                                 FileShare.None,
                                 1024 * 1024,
                                 FileOptions.Asynchronous
                                 | FileOptions.SequentialScan))
                {
                    ZLinkRelocationEnvelopeCodec.EncodeTo(output, envelope);
                    await output.FlushAsync(cancellationToken)
                        .ConfigureAwait(false);
                }
                sourceLength = new FileInfo(logicalPath).Length;
            }
            else
            {
                sourceLength = envelope.CanonicalLogicalStream.Length;
            }
            var componentCount = CalculateOrderedStripeCount(
                sourceLength,
                envelope.Participants.Count);
            var components =
                new ParticipantComponentBuilder[componentCount];
            var measuredLengths = new long[componentCount];
            for (var index = 0; index < componentCount; index++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var length = MeasureCanonicalSlice(
                    sourceLength,
                    componentCount,
                    index);
                measuredLengths[index] = length;
                components[index] = new ParticipantComponentBuilder(
                    index,
                    length);
            }
            var preflight = ValidateParticipantComponentBounds(
                measuredLengths);
            var preflightLogicalLength = preflight.LogicalLength;
            var preflightChunkCount = preflight.ChunkCount;

            var pending = new List<ParticipantEncodedChunk>(
                MaxConcurrentComponentIo);
            long pendingBytes = 0;
            var globalOrder = 0;
            var logicalLength = 0UL;
            var logicalCrcState = uint.MaxValue;

            async ValueTask FlushAsync()
            {
                if (pending.Count == 0)
                    return;
                var stored = await Task.WhenAll(
                        pending.Select(chunk => PutVerifiedAsync(
                                store,
                                chunk.Encoded,
                                retention,
                                cancellationToken)
                            .AsTask()))
                    .ConfigureAwait(false);
                for (var index = 0; index < pending.Count; index++)
                {
                    var chunk = pending[index];
                    components[chunk.ParticipantOrder].Chunks.Add(
                        new ChunkEntry(
                            checked((uint)chunk.GlobalOrder),
                            stored[index].Reference,
                            checked((ulong)chunk.DataLength),
                            chunk.DataChecksumCrc32c));
                }
                pending.Clear();
                pendingBytes = 0;
            }

            for (var componentIndex = 0;
                 componentIndex < componentCount;
                 componentIndex++)
            {
                using var source = logicalPath is null
                    ? CreateMemorySliceSource(
                        envelope.CanonicalLogicalStream,
                        sourceLength,
                        componentCount,
                        componentIndex)
                    : CreateFileSliceSource(
                        logicalPath,
                        sourceLength,
                        componentCount,
                        componentIndex);
                var componentCrcState = uint.MaxValue;
                await using var input = source.OpenRead();
                var localChunkOrder = 0;
                while (input.Position < input.Length)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    var dataLength = checked((int)Math.Min(
                        ChunkBytes,
                        input.Length - input.Position));
                    var encodedLength = checked(
                        FrameHeaderBytes + ChunkBodyPrefixBytes
                        + dataLength + FrameChecksumBytes);
                    if (pending.Count == MaxConcurrentComponentIo
                        || pending.Count != 0
                        && pendingBytes > MaxComponentIoBytes - encodedLength)
                        await FlushAsync().ConfigureAwait(false);

                    var encoded = new byte[encodedLength];
                    WriteFrameHeader(
                        encoded,
                        ChunkMagic,
                        checked((uint)(ChunkBodyPrefixBytes + dataLength)));
                    BinaryPrimitives.WriteUInt32BigEndian(
                        encoded.AsSpan(FrameHeaderBytes, 4),
                        checked((uint)globalOrder));
                    BinaryPrimitives.WriteUInt32BigEndian(
                        encoded.AsSpan(FrameHeaderBytes + 4, 4),
                        checked((uint)dataLength));
                    var data = encoded.AsMemory(
                        FrameHeaderBytes + ChunkBodyPrefixBytes,
                        dataLength);
                    await input.ReadExactlyAsync(data, cancellationToken)
                        .ConfigureAwait(false);
                    var dataChecksum = ZLinkCrc32C.Compute(data.Span);
                    ZLinkCrc32C.Append(
                        ref componentCrcState,
                        data.Span);
                    ZLinkCrc32C.Append(ref logicalCrcState, data.Span);
                    WriteFrameChecksum(encoded);
                    pending.Add(new ParticipantEncodedChunk(
                        source.Order,
                        localChunkOrder++,
                        globalOrder++,
                        dataLength,
                        dataChecksum,
                        encoded));
                    pendingBytes = checked(pendingBytes + encodedLength);
                    logicalLength = checked(
                        logicalLength + (uint)dataLength);
                }
                components[source.Order].LogicalChecksumCrc32c =
                    ~componentCrcState;
            }
            await FlushAsync().ConfigureAwait(false);
            if (logicalLength != preflightLogicalLength
                || globalOrder != preflightChunkCount)
                throw new InvalidOperationException(
                    "Relocation ordered-stripe preflight changed during encoding.");

            var manifest = EncodeParticipantManifest(
                envelope,
                logicalLength,
                ~logicalCrcState,
                components,
                canonicalSlices: true);
            var root = await PutVerifiedAsync(
                    store,
                    manifest,
                    retention,
                    cancellationToken)
                .ConfigureAwait(false);
            return new ZLinkRelocationTreeStored(
                root,
                checked((long)logicalLength),
                ~logicalCrcState,
                globalOrder);
        }
        finally
        {
            if (logicalPath is not null)
                File.Delete(logicalPath);
        }
    }

    internal static (
        ulong LogicalLength,
        int ChunkCount,
        int MinimumManifestBytes)
        ValidateParticipantComponentBounds(
            IReadOnlyList<long> componentLengths)
    {
        ArgumentNullException.ThrowIfNull(componentLengths);
        if (componentLengths.Count < 2)
            throw new ArgumentOutOfRangeException(
                nameof(componentLengths));
        ulong logicalLength = 0;
        long chunkCount = 0;
        foreach (var length in componentLengths)
        {
            if (length <= 0 || (ulong)length > MaxLogicalBytes)
                throw new InvalidOperationException(
                    "A relocation participant component exceeds its bound.");
            logicalLength = checked(
                logicalLength + (ulong)length);
            if (logicalLength > MaxLogicalBytes)
                throw new InvalidOperationException(
                    "Relocation participant components exceed the 256 GiB aggregate bound.");
            chunkCount = checked(
                chunkCount
                + CalculateChunkCount(checked((ulong)length)));
        }
        if (chunkCount > MaxChunks)
            throw new InvalidOperationException(
                "Relocation participant components exceed the 4,096 chunk aggregate bound.");
        var minimumManifestBytes = checked(
            (ulong)FrameHeaderBytes
            + ParticipantManifestFixedBodyBytes
            + (ulong)componentLengths.Count * 20
            + (ulong)chunkCount * MinimumParticipantChunkBytes
            + FrameChecksumBytes);
        if (minimumManifestBytes > MaxManifestBytes)
            throw new InvalidOperationException(
                "Relocation participant manifest exceeds its 64 MiB bound.");
        return (
            logicalLength,
            checked((int)chunkCount),
            checked((int)minimumManifestBytes));
    }

    private static long MeasureCanonicalSlice(
        long logicalLength,
        int stripeCount,
        int order)
    {
        if (logicalLength < stripeCount)
            throw new InvalidOperationException(
                "A relocation root cannot be divided into non-empty ordered stripes.");
        var quotient = logicalLength / stripeCount;
        var remainder = logicalLength % stripeCount;
        return quotient + (order < remainder ? 1 : 0);
    }

    private static int CalculateOrderedStripeCount(
        long logicalLength,
        int participantCount)
    {
        if (logicalLength <= 0)
            throw new InvalidOperationException(
                "A canonical relocation stream must not be empty.");
        return checked((int)Math.Min(
            logicalLength,
            Math.Min(participantCount, MaxOrderedStripes)));
    }

    private static ParticipantComponentSource CreateFileSliceSource(
        string path,
        long logicalLength,
        int stripeCount,
        int order)
    {
        var length = MeasureCanonicalSlice(
            logicalLength,
            stripeCount,
            order);
        var quotient = logicalLength / stripeCount;
        var remainder = logicalLength % stripeCount;
        var offset = checked(
            order * quotient + Math.Min(order, remainder));
        return ParticipantComponentSource.FromFileSlice(
            order,
            path,
            offset,
            length);
    }

    private static ParticipantComponentSource CreateMemorySliceSource(
        ReadOnlyMemory<byte> bytes,
        long logicalLength,
        int stripeCount,
        int order)
    {
        var length = MeasureCanonicalSlice(
            logicalLength,
            stripeCount,
            order);
        var quotient = logicalLength / stripeCount;
        var remainder = logicalLength % stripeCount;
        var offset = checked(
            order * quotient + Math.Min(order, remainder));
        return ParticipantComponentSource.FromMemorySlice(
            order,
            bytes.Slice(
                checked((int)offset),
                checked((int)length)));
    }

    private static async ValueTask<ZLinkRelocationTreeRead>
        ReadParticipantComponentsAsync(
            IZLinkRelocationRepository store,
            ReadOnlyMemory<byte> encodedManifest,
            CancellationToken cancellationToken)
    {
        var manifest = DecodeParticipantManifest(encodedManifest.Span);
        var path = Path.GetTempFileName();
        try
        {
            var chunks = manifest.Components
                .SelectMany(component => component.Chunks.Select(
                    chunk => new ManifestParticipantChunk(
                        component.Order,
                        chunk)))
                .ToArray();
            var logicalCrcState = uint.MaxValue;
            var componentCrcStates = Enumerable.Repeat(
                    uint.MaxValue,
                    manifest.Components.Count)
                .ToArray();
            ulong logicalLength = 0;
            await using (var output = new FileStream(
                             path,
                             FileMode.Truncate,
                             FileAccess.Write,
                             FileShare.None,
                             1024 * 1024,
                             FileOptions.Asynchronous
                             | FileOptions.SequentialScan))
            {
                for (var firstIndex = 0;
                     firstIndex < chunks.Length;)
                {
                    var count = CalculateParticipantBatchCount(
                        chunks,
                        firstIndex);
                    var batch = await Task.WhenAll(
                            chunks
                                .Skip(firstIndex)
                                .Take(count)
                                .Select(item => ReadChunkAsync(
                                        store,
                                        item.Chunk,
                                        cancellationToken)
                                    .AsTask()))
                        .ConfigureAwait(false);
                    for (var index = 0; index < batch.Length; index++)
                    {
                        var item = chunks[firstIndex + index];
                        var chunk = batch[index];
                        if (chunk.Order != item.Chunk.Order)
                            throw DataLost(
                                "Relocation component completion order is invalid.");
                        await output.WriteAsync(
                                chunk.Data,
                                cancellationToken)
                            .ConfigureAwait(false);
                        ZLinkCrc32C.Append(
                            ref componentCrcStates[
                                item.ParticipantOrder],
                            chunk.Data.Span);
                        ZLinkCrc32C.Append(
                            ref logicalCrcState,
                            chunk.Data.Span);
                        logicalLength = checked(
                            logicalLength + (uint)chunk.Data.Length);
                    }
                    firstIndex += count;
                }
                await output.FlushAsync(cancellationToken)
                    .ConfigureAwait(false);
            }

            if (logicalLength != manifest.LogicalLength
                || ~logicalCrcState != manifest.LogicalChecksumCrc32c)
                throw DataLost(
                    "Relocation participant component stream is invalid.");

            var participants =
                new ZLinkRelocationParticipantEnvelope[
                    manifest.Components.Count];
            await using var input = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                1024 * 1024,
                FileOptions.SequentialScan);
            long componentOffset = 0;
            for (var index = 0;
                 index < manifest.Components.Count;
                 index++)
            {
                if (~componentCrcStates[index]
                    != manifest.Components[index].LogicalChecksumCrc32c)
                    throw DataLost(
                        "Relocation participant component checksum is invalid.");
            }
            if (manifest.CanonicalSlices)
            {
                var decoded = ZLinkRelocationEnvelopeCodec.Decode(
                    input,
                    manifest.InventoryDigest);
                if (decoded.AggregateId != manifest.AggregateId
                    || !decoded.InventoryDigest.Span.SequenceEqual(
                        manifest.InventoryDigest.Span))
                    throw DataLost(
                        "Canonical relocation component identity is invalid.");
                return new ZLinkRelocationTreeRead(
                    decoded,
                    checked((long)logicalLength),
                    ~logicalCrcState,
                    chunks.Length);
            }
            for (var index = 0;
                 index < manifest.Components.Count;
                 index++)
            {
                var component = manifest.Components[index];
                using var componentInput = new BoundedReadStream(
                    input,
                    componentOffset,
                    component.LogicalLength);
                var decoded = ZLinkRelocationEnvelopeCodec.Decode(
                    componentInput,
                    manifest.InventoryDigest);
                if (decoded.AggregateId != manifest.AggregateId
                    || decoded.AggregateGeneration
                       != manifest.AggregateGeneration
                    || !decoded.InventoryDigest.Span.SequenceEqual(
                        manifest.InventoryDigest.Span)
                    || decoded.Participants.Count != 1)
                    throw DataLost(
                        "Relocation participant component identity is invalid.");
                participants[index] = decoded.Participants[0];
                componentOffset = checked(
                    componentOffset + component.LogicalLength);
            }
            var envelope = new ZLinkRelocationEnvelope(
                manifest.AggregateId,
                manifest.AggregateGeneration,
                manifest.InventoryDigest,
                participants);
            return new ZLinkRelocationTreeRead(
                envelope,
                checked((long)logicalLength),
                ~logicalCrcState,
                chunks.Length);
        }
        finally
        {
            File.Delete(path);
        }
    }

    private static async ValueTask RenewParticipantComponentsAsync(
        IZLinkRelocationRepository store,
        ReadOnlyMemory<byte> encodedManifest,
        TimeSpan retention,
        CancellationToken cancellationToken)
    {
        var manifest = DecodeParticipantManifest(encodedManifest.Span);
        var chunks = manifest.Components
            .SelectMany(static component => component.Chunks)
            .ToArray();
        for (var firstIndex = 0;
             firstIndex < chunks.Length;)
        {
            var count = CalculateComponentBatchCountCore(
                chunks.Length,
                firstIndex,
                index => chunks[index].Length);
            await Task.WhenAll(
                    chunks
                        .Skip(firstIndex)
                        .Take(count)
                        .Select(chunk => VerifyAndRenewChunkAsync(
                                store,
                                chunk,
                                retention,
                                cancellationToken)
                            .AsTask()))
                .ConfigureAwait(false);
            firstIndex += count;
        }
    }

    private static int CalculateParticipantBatchCount(
        IReadOnlyList<ManifestParticipantChunk> chunks,
        int firstIndex) =>
        CalculateComponentBatchCountCore(
            chunks.Count,
            firstIndex,
            index => chunks[index].Chunk.Length);

    private static byte[] EncodeParticipantManifest(
        ZLinkRelocationEnvelope envelope,
        ulong logicalLength,
        uint logicalChecksum,
        IReadOnlyList<ParticipantComponentBuilder> components,
        bool canonicalSlices)
    {
        var references = components
            .SelectMany(static component => component.Chunks)
            .Select(static chunk => Encoding.UTF8.GetBytes(chunk.Reference))
            .ToArray();
        if (references.Any(static value =>
                value.Length is < 1 or > MaxReferenceBytes))
            throw new InvalidOperationException(
                "A relocation reference exceeds its UTF-8 bound.");

        var referenceIndex = 0;
        var bodyLength = checked(
            1 + 1 + 8 + 4 + 1 + InventoryDigestBytes + 16 + 8 + 4
            + components.Sum(component =>
                4 + 8 + 4 + 4
                + component.Chunks.Sum(_ =>
                {
                    var length = references[referenceIndex++].Length;
                    return 4 + 2 + length + 8 + 4;
                })));
        if (bodyLength
            > MaxManifestBytes
              - FrameHeaderBytes
              - FrameChecksumBytes)
            throw new InvalidOperationException(
                "Relocation participant manifest exceeds 64 MiB.");
        var encoded = new byte[
            FrameHeaderBytes + bodyLength + FrameChecksumBytes];
        WriteFrameHeader(
            encoded,
            ParticipantManifestMagic,
            checked((uint)bodyLength));
        var offset = FrameHeaderBytes;
        encoded[offset++] = 2;
        encoded[offset++] = canonicalSlices ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt64BigEndian(
            encoded.AsSpan(offset, 8), logicalLength);
        offset += 8;
        BinaryPrimitives.WriteUInt32BigEndian(
            encoded.AsSpan(offset, 4), logicalChecksum);
        offset += 4;
        encoded[offset++] = InventoryDigestBytes;
        envelope.InventoryDigest.Span.CopyTo(
            encoded.AsSpan(offset, InventoryDigestBytes));
        offset += InventoryDigestBytes;
        envelope.AggregateId.TryWriteBytes(encoded.AsSpan(offset, 16));
        offset += 16;
        BinaryPrimitives.WriteUInt64BigEndian(
            encoded.AsSpan(offset, 8),
            envelope.AggregateGeneration);
        offset += 8;
        BinaryPrimitives.WriteUInt32BigEndian(
            encoded.AsSpan(offset, 4),
            checked((uint)components.Count));
        offset += 4;

        referenceIndex = 0;
        foreach (var component in components)
        {
            BinaryPrimitives.WriteUInt32BigEndian(
                encoded.AsSpan(offset, 4),
                checked((uint)component.Order));
            offset += 4;
            BinaryPrimitives.WriteUInt64BigEndian(
                encoded.AsSpan(offset, 8),
                checked((ulong)component.LogicalLength));
            offset += 8;
            BinaryPrimitives.WriteUInt32BigEndian(
                encoded.AsSpan(offset, 4),
                component.LogicalChecksumCrc32c);
            offset += 4;
            BinaryPrimitives.WriteUInt32BigEndian(
                encoded.AsSpan(offset, 4),
                checked((uint)component.Chunks.Count));
            offset += 4;
            foreach (var chunk in component.Chunks)
            {
                var reference = references[referenceIndex++];
                BinaryPrimitives.WriteUInt32BigEndian(
                    encoded.AsSpan(offset, 4), chunk.Order);
                offset += 4;
                BinaryPrimitives.WriteUInt16BigEndian(
                    encoded.AsSpan(offset, 2),
                    checked((ushort)reference.Length));
                offset += 2;
                reference.CopyTo(encoded.AsSpan(offset));
                offset += reference.Length;
                BinaryPrimitives.WriteUInt64BigEndian(
                    encoded.AsSpan(offset, 8), chunk.Length);
                offset += 8;
                BinaryPrimitives.WriteUInt32BigEndian(
                    encoded.AsSpan(offset, 4),
                    chunk.ChecksumCrc32c);
                offset += 4;
            }
        }
        if (offset != encoded.Length - FrameChecksumBytes)
            throw new InvalidOperationException(
                "Relocation participant manifest length is invalid.");
        WriteFrameChecksum(encoded);
        if (encoded.Length > MaxManifestBytes)
            throw new InvalidOperationException(
                "Relocation participant manifest exceeds 64 MiB.");
        return encoded;
    }

    private static ParticipantManifest DecodeParticipantManifest(
        ReadOnlySpan<byte> encoded)
    {
        if (encoded.Length > MaxManifestBytes)
            throw DataLost(
                "Relocation participant manifest exceeds its 64 MiB bound.");
        var body = DecodeFrame(encoded, ParticipantManifestMagic);
        var offset = 0;
        var version = ReadByte(body, ref offset);
        if (version is not (1 or 2))
            throw DataLost(
                "Relocation participant manifest version is invalid.");
        var canonicalSlices = version == 2
            && ReadByte(body, ref offset) switch
            {
                0 => false,
                1 => true,
                _ => throw DataLost(
                    "Relocation participant manifest component mode is invalid.")
            };
        var logicalLength = ReadUInt64(body, ref offset);
        var logicalChecksum = ReadUInt32(body, ref offset);
        if (ReadByte(body, ref offset) != InventoryDigestBytes)
            throw DataLost(
                "Relocation participant manifest digest is invalid.");
        var digest = ReadBytes(
                body,
                ref offset,
                InventoryDigestBytes)
            .ToArray();
        var aggregateId = new Guid(ReadBytes(body, ref offset, 16));
        var aggregateGeneration = ReadUInt64(body, ref offset);
        var participantCount = ReadUInt32(body, ref offset);
        var remaining = body.Length - offset;
        if (aggregateId == Guid.Empty
            || aggregateGeneration is 0 or > long.MaxValue
            || logicalLength is 0 or > MaxLogicalBytes
            || participantCount < 2
            || participantCount
               > (uint)Math.Min(
                   int.MaxValue,
                   remaining / MinimumParticipantManifestBytes))
            throw DataLost(
                "Relocation participant manifest bounds are invalid.");

        var components =
            new ParticipantComponentManifest[
                checked((int)participantCount)];
        var expectedGlobalOrder = 0U;
        ulong totalLength = 0;
        for (var index = 0; index < components.Length; index++)
        {
            var order = ReadUInt32(body, ref offset);
            var componentLength = ReadUInt64(body, ref offset);
            var componentChecksum = ReadUInt32(body, ref offset);
            var chunkCount = ReadUInt32(body, ref offset);
            if (order != (uint)index
                || componentLength is 0 or > MaxLogicalBytes
                || chunkCount is 0 or > MaxChunks
                || chunkCount
                   > (uint)((body.Length - offset)
                       / MinimumParticipantChunkBytes))
                throw DataLost(
                    "Relocation participant component bounds are invalid.");
            var chunks = new ChunkEntry[checked((int)chunkCount)];
            ulong componentTotal = 0;
            for (var chunkIndex = 0;
                 chunkIndex < chunks.Length;
                 chunkIndex++)
            {
                var globalOrder = ReadUInt32(body, ref offset);
                var referenceLength = ReadUInt16(body, ref offset);
                if (globalOrder != expectedGlobalOrder++
                    || referenceLength is 0 or > MaxReferenceBytes)
                    throw DataLost(
                        "Relocation participant chunk order is invalid.");
                var reference = new UTF8Encoding(false, true).GetString(
                    ReadBytes(body, ref offset, referenceLength));
                var length = ReadUInt64(body, ref offset);
                var checksum = ReadUInt32(body, ref offset);
                if (length is 0 or > ChunkBytes
                    || chunkIndex < chunks.Length - 1
                    && length != ChunkBytes)
                    throw DataLost(
                        "Relocation participant chunk length is invalid.");
                componentTotal = checked(componentTotal + length);
                chunks[chunkIndex] = new ChunkEntry(
                    globalOrder,
                    reference,
                    length,
                    checksum);
            }
            if (componentTotal != componentLength)
                throw DataLost(
                    "Relocation participant component length is invalid.");
            totalLength = checked(totalLength + componentLength);
            components[index] = new ParticipantComponentManifest(
                index,
                checked((long)componentLength),
                componentChecksum,
                chunks);
        }
        if (offset != body.Length
            || totalLength != logicalLength
            || expectedGlobalOrder is 0 or > MaxChunks)
            throw DataLost(
                "Relocation participant manifest length is invalid.");
        return new ParticipantManifest(
            aggregateId,
            aggregateGeneration,
            logicalLength,
            logicalChecksum,
            digest,
            canonicalSlices,
            components);
    }

    private sealed class ParticipantComponentSource : IDisposable
    {
        private readonly string? _path;
        private readonly ReadOnlyMemory<byte> _memory;
        private readonly long _offset;

        private ParticipantComponentSource(
            int order,
            string? path,
            ReadOnlyMemory<byte> memory,
            long offset,
            long length)
        {
            Order = order;
            Length = length;
            _path = path;
            _memory = memory;
            _offset = offset;
        }

        internal int Order { get; }
        internal long Length { get; }

        internal static ParticipantComponentSource FromFileSlice(
            int order,
            string path,
            long offset,
            long length) =>
            new(order, path, default, offset, length);

        internal static ParticipantComponentSource FromMemorySlice(
            int order,
            ReadOnlyMemory<byte> memory) =>
            new(order, null, memory, 0, memory.Length);

        internal Stream OpenRead()
        {
            if (_path is null)
                return new MemoryStream(
                    _memory.ToArray(),
                    writable: false);
            var input = new FileStream(
                _path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                1024 * 1024,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            input.Position = _offset;
            return new BoundedReadStream(
                input,
                _offset,
                Length,
                leaveOpen: false);
        }

        public void Dispose() { }
    }

    private sealed class BoundedReadStream(
        Stream inner,
        long start,
        long length,
        bool leaveOpen = true) : Stream
    {
        public override bool CanRead => inner.CanRead;
        public override bool CanSeek => inner.CanSeek;
        public override bool CanWrite => false;
        public override long Length => length;
        public override long Position
        {
            get => inner.Position - start;
            set
            {
                if (value < 0 || value > length)
                    throw new ArgumentOutOfRangeException(nameof(value));
                inner.Position = checked(start + value);
            }
        }
        public override int Read(byte[] buffer, int offset, int count)
        {
            var bounded = checked((int)Math.Min(
                count,
                length - Position));
            return bounded == 0
                ? 0
                : inner.Read(buffer, offset, bounded);
        }
        public override int Read(Span<byte> buffer)
        {
            var bounded = checked((int)Math.Min(
                buffer.Length,
                length - Position));
            return bounded == 0
                ? 0
                : inner.Read(buffer[..bounded]);
        }
        public override long Seek(long offset, SeekOrigin origin)
        {
            var target = origin switch
            {
                SeekOrigin.Begin => offset,
                SeekOrigin.Current => checked(Position + offset),
                SeekOrigin.End => checked(length + offset),
                _ => throw new ArgumentOutOfRangeException(nameof(origin))
            };
            Position = target;
            return target;
        }
        public override void Flush() { }
        public override void SetLength(long value) =>
            throw new NotSupportedException();
        public override void Write(
            byte[] buffer,
            int offset,
            int count) =>
            throw new NotSupportedException();
        protected override void Dispose(bool disposing)
        {
            if (disposing && !leaveOpen)
                inner.Dispose();
            base.Dispose(disposing);
        }

        public override async ValueTask DisposeAsync()
        {
            if (!leaveOpen)
                await inner.DisposeAsync().ConfigureAwait(false);
            GC.SuppressFinalize(this);
        }
    }

    private sealed record ParticipantEncodedChunk(
        int ParticipantOrder,
        int LocalOrder,
        int GlobalOrder,
        int DataLength,
        uint DataChecksumCrc32c,
        ReadOnlyMemory<byte> Encoded);

    private sealed class ParticipantComponentBuilder(
        int order,
        long logicalLength)
    {
        internal int Order { get; } = order;
        internal long LogicalLength { get; } = logicalLength;
        internal uint LogicalChecksumCrc32c { get; set; }
        internal List<ChunkEntry> Chunks { get; } = [];
    }

    private sealed record ParticipantComponentManifest(
        int Order,
        long LogicalLength,
        uint LogicalChecksumCrc32c,
        IReadOnlyList<ChunkEntry> Chunks);

    private sealed record ParticipantManifest(
        Guid AggregateId,
        ulong AggregateGeneration,
        ulong LogicalLength,
        uint LogicalChecksumCrc32c,
        ReadOnlyMemory<byte> InventoryDigest,
        bool CanonicalSlices,
        IReadOnlyList<ParticipantComponentManifest> Components);

    private sealed record ManifestParticipantChunk(
        int ParticipantOrder,
        ChunkEntry Chunk);
}
