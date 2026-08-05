using System.Collections.Concurrent;
using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class RelocationTreeParallelIoTests
{
    [Fact]
    public async Task SpotWideTree_UsesBoundedParallelIo_AndPublishesRootLast()
    {
        var envelope = CreateSpotWideEnvelope();
        var store = new ConcurrentRelocationStore();

        var stored = await ZLinkRelocationTreeStore.PutAsync(
            store,
            envelope,
            TimeSpan.FromHours(24),
            CancellationToken.None);

        Assert.Equal(
            ZLinkRelocationTreeStore.MaxOrderedStripes,
            stored.ChunkCount);
        Assert.InRange(
            store.MaxConcurrentChunkPuts,
            2,
            ZLinkRelocationTreeStore.MaxConcurrentComponentIo);
        Assert.Equal("manifest", store.CompletedPutKinds[^1]);
        Assert.Equal(
            ZLinkRelocationTreeStore.MaxOrderedStripes,
            store.CompletedPutKinds.Count(
                static kind => kind == "chunk"));
        Assert.True(
            store.MaxChunkPayloadBytesInFlight
            <= ZLinkRelocationTreeStore.MaxComponentIoBytes);

        store.ResetReadConcurrency();
        var restored = await ZLinkRelocationTreeStore.GetAsync(
            store,
            stored.Root.Reference,
            stored.Root.ChecksumCrc32c,
            CancellationToken.None);

        Assert.InRange(
            store.MaxConcurrentChunkReads,
            2,
            ZLinkRelocationTreeStore.MaxConcurrentComponentIo);
        Assert.Equal(
            envelope.Participants.Select(
                static participant => participant.AuthorityKey),
            restored.Participants.Select(
                static participant => participant.AuthorityKey));
        Assert.Equal(
            ZLinkRelocationEnvelopeCodec.ComputeEncodedSha256(envelope),
            ZLinkRelocationEnvelopeCodec.ComputeEncodedSha256(restored));

        store.ResetRenewConcurrency();
        await ZLinkRelocationTreeStore.RenewTreeAsync(
            store,
            stored.Root.Reference,
            stored.Root.ChecksumCrc32c,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        Assert.InRange(
            store.MaxConcurrentChunkRenews,
            2,
            ZLinkRelocationTreeStore.MaxConcurrentComponentIo);

        var corruptReference = store.ChunkReferences[0];
        store.Corrupt(corruptReference);
        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            () => ZLinkRelocationTreeStore.GetAsync(
                    store,
                    stored.Root.Reference,
                    stored.Root.ChecksumCrc32c,
                    CancellationToken.None)
                .AsTask());
        store.UndoCorruption(corruptReference);

        var missingReference = store.ChunkReferences[1];
        store.Hide(missingReference);
        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            () => ZLinkRelocationTreeStore.GetAsync(
                    store,
                    stored.Root.Reference,
                    stored.Root.ChecksumCrc32c,
                    CancellationToken.None)
                .AsTask());
        store.Show(missingReference);

        var failingStore = new ConcurrentRelocationStore
        {
            FailChunkPutOrdinal = 2
        };
        await Assert.ThrowsAsync<IOException>(
            () => ZLinkRelocationTreeStore.PutAsync(
                    failingStore,
                    envelope,
                    TimeSpan.FromHours(24),
                    CancellationToken.None)
                .AsTask());
        Assert.Equal(0, failingStore.ManifestPutCount);
    }

    [Fact]
    public async Task SingleParticipantTree_KeepsLargeComponentChunking()
    {
        var fourFullChunks = Enumerable.Repeat(
                (ulong)ZLinkRelocationTreeStore.ChunkBytes,
                4)
            .ToArray();
        Assert.Equal(
            3,
            ZLinkRelocationTreeStore.CalculateComponentBatchCount(
                fourFullChunks,
                0));
        const int encodedFrameBytes = 23;
        Assert.True(
            3L * (ZLinkRelocationTreeStore.ChunkBytes + encodedFrameBytes)
            <= ZLinkRelocationTreeStore.MaxComponentIoBytes);
        Assert.True(
            4L * (ZLinkRelocationTreeStore.ChunkBytes + encodedFrameBytes)
            > ZLinkRelocationTreeStore.MaxComponentIoBytes);
        var envelope = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            SHA256.HashData("small-spot-wide"u8),
            [
                new ZLinkRelocationParticipantEnvelope(
                    new ZLinkAuthorityKey("actor:mesh:small"),
                    ZLinkPlacementObjectKind.Actor,
                    1,
                    1,
                    new byte[] { 4, 5, 6 },
                    [],
                    [])
            ]);
        var store = new ConcurrentRelocationStore();

        var stored = await ZLinkRelocationTreeStore.PutAsync(
            store,
            envelope,
            TimeSpan.FromHours(24),
            CancellationToken.None);

        Assert.Equal(1, stored.ChunkCount);
        Assert.Equal(
            new[] { "chunk", "manifest" },
            store.CompletedPutKinds);
    }

    [Fact]
    public void ParticipantPreflight_BoundsTheAggregateChunkCount()
    {
        var manySmallParticipants = new RepeatedLengthList(
            count: ZLinkRelocationTreeStore.MaxChunks,
            value: 64 * 1024);

        var accepted = ZLinkRelocationTreeStore
            .ValidateParticipantComponentBounds(
                manySmallParticipants);

        Assert.Equal(
            ZLinkRelocationTreeStore.MaxChunks,
            accepted.ChunkCount);
        Assert.Equal<ulong>(
            (ulong)ZLinkRelocationTreeStore.MaxChunks * 64 * 1024,
            accepted.LogicalLength);
        Assert.Throws<InvalidOperationException>(
            () => ZLinkRelocationTreeStore
                .ValidateParticipantComponentBounds(
                    new RepeatedLengthList(
                        count: ZLinkRelocationTreeStore.MaxChunks + 1,
                        value: 1)));
        Assert.Throws<InvalidOperationException>(
            () => ZLinkRelocationTreeStore
                .ValidateParticipantComponentBounds(
                    new long[]
                    {
                        checked((long)ZLinkRelocationTreeStore
                            .MaxLogicalBytes),
                        1
                    }));
        Assert.Throws<InvalidOperationException>(
            () => ZLinkRelocationTreeStore
                .ValidateParticipantComponentBounds(
                    new RepeatedLengthList(
                        count: 2_000_000,
                        value: 1)));
    }

    [Theory]
    [InlineData(1)]
    [InlineData(4)]
    public void VersionedEnvelopeDecodeKeepsOneOwnedPayloadCopy(
        int participantCount)
    {
        var state = GC.AllocateUninitializedArray<byte>(
            ZLinkRelocationTreeStore.ChunkBytes);
        Array.Fill(state, (byte)0x6d);
        var envelope = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            SHA256.HashData("bounded-envelope-decode"u8),
            Enumerable.Range(0, participantCount)
                .Select(index =>
                    new ZLinkRelocationParticipantEnvelope(
                        new ZLinkAuthorityKey($"actor:decode:{index}"),
                        ZLinkPlacementObjectKind.Actor,
                        1,
                        1,
                        state,
                        [],
                        []))
                .ToArray());
        var path = Path.GetTempFileName();
        try
        {
            using (var output = new FileStream(
                       path,
                       FileMode.Truncate,
                       FileAccess.Write,
                       FileShare.None))
                ZLinkRelocationEnvelopeCodec.EncodeTo(output, envelope);

            using var input = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                1024 * 1024,
                FileOptions.SequentialScan);
            var before = GC.GetAllocatedBytesForCurrentThread();
            var restored = ZLinkRelocationEnvelopeCodec.Decode(input);
            var allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            var logicalPayloadBytes =
                (long)participantCount * ZLinkRelocationTreeStore.ChunkBytes;

            Assert.Equal(participantCount, restored.Participants.Count);
            Assert.All(
                restored.Participants,
                participant =>
                    Assert.Equal(
                        ZLinkRelocationTreeStore.ChunkBytes,
                        participant.ApplicationState.Length));
            Assert.InRange(
                allocated,
                logicalPayloadBytes,
                logicalPayloadBytes + 32L * 1024 * 1024);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void NonSeekableVersionedEnvelopeSharesOneOwnedPayloadBuffer()
    {
        var state = GC.AllocateUninitializedArray<byte>(
            ZLinkRelocationTreeStore.ChunkBytes);
        Array.Fill(state, (byte)0x4d);
        var encoded = ZLinkRelocationEnvelopeCodec.Encode(
            new ZLinkRelocationEnvelope(
                Guid.NewGuid(),
                1,
                SHA256.HashData("nonseek-versioned"u8),
                [
                    new ZLinkRelocationParticipantEnvelope(
                        new ZLinkAuthorityKey("actor:nonseek"),
                        ZLinkPlacementObjectKind.Actor,
                        1,
                        1,
                        state,
                        [],
                        [])
                ]));
        using var input = new NonSeekableReadStream(encoded);

        var before = GC.GetAllocatedBytesForCurrentThread();
        var restored = ZLinkRelocationEnvelopeCodec.Decode(input);
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        Assert.Equal(state.Length, restored.Participants[0].ApplicationState.Length);
        Assert.True(MemoryMarshal.TryGetArray(
            restored.Participants[0].ApplicationState,
            out var stateSegment));
        Assert.NotSame(encoded, stateSegment.Array);
        Assert.InRange(
            allocated,
            encoded.Length,
            2L * encoded.Length + 16L * 1024 * 1024);
    }

    [Fact]
    public void VersionedSpanDecodeProjectsFromItsSingleOwnedCopy()
    {
        var state = GC.AllocateUninitializedArray<byte>(
            ZLinkRelocationTreeStore.ChunkBytes);
        var encoded = ZLinkRelocationEnvelopeCodec.Encode(
            new ZLinkRelocationEnvelope(
                Guid.NewGuid(),
                1,
                SHA256.HashData("span-versioned"u8),
                [
                    new ZLinkRelocationParticipantEnvelope(
                        new ZLinkAuthorityKey("actor:span"),
                        ZLinkPlacementObjectKind.Actor,
                        1,
                        1,
                        state,
                        [],
                        [])
                ]));

        var before = GC.GetAllocatedBytesForCurrentThread();
        var restored = ZLinkRelocationEnvelopeCodec.Decode(encoded.AsSpan());
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        Assert.True(MemoryMarshal.TryGetArray(
            restored.Participants[0].ApplicationState,
            out var stateSegment));
        Assert.NotSame(encoded, stateSegment.Array);
        Assert.InRange(
            allocated,
            encoded.Length,
            encoded.Length + 16L * 1024 * 1024);
    }

    [Fact]
    public void NonSeekableCanonicalEnvelopeProjectsSlicesFromOwnedBuffer()
    {
        var encoded = CreateCanonicalEnvelopeWithState(
            ZLinkRelocationTreeStore.ChunkBytes);
        using var input = new NonSeekableReadStream(encoded);

        var before = GC.GetAllocatedBytesForCurrentThread();
        var restored = ZLinkRelocationEnvelopeCodec.Decode(input);
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        var state = restored.Participants[0].ApplicationState;
        Assert.Equal(ZLinkRelocationTreeStore.ChunkBytes, state.Length);
        Assert.True(MemoryMarshal.TryGetArray(
            restored.CanonicalLogicalStream,
            out var logicalSegment));
        Assert.True(MemoryMarshal.TryGetArray(state, out var stateSegment));
        Assert.Same(logicalSegment.Array, stateSegment.Array);
        Assert.NotSame(encoded, logicalSegment.Array);
        Assert.InRange(
            allocated,
            encoded.Length,
            2L * encoded.Length + 16L * 1024 * 1024);
    }

    [Fact]
    public void NonSeekableCanonicalAcceptedPayloadSharesOwnedBuffer()
    {
        var golden = ReadCanonicalRelocationGolden();
        var projected = ZLinkRelocationEnvelopeCodec.Decode(golden);
        var accepted = Assert.IsType<ZLinkCanonicalAcceptedRequest>(
            projected.Participants
                .SelectMany(static participant => participant.AcceptedJobs)
                .Select(static job => job.CanonicalRequest)
                .First(static request => request is not null));
        var originalPayload = accepted.ApplicationPayload.Payload.ToArray();
        var payloadStart = golden.AsSpan().IndexOf(originalPayload);
        Assert.True(payloadStart > 0);
        var packetBytes = Encoding.UTF8.GetByteCount(
            accepted.ApplicationPayload.PacketName);
        var contentBytes = Encoding.UTF8.GetByteCount(
            accepted.ApplicationPayload.ContentType);
        var bodyStart = payloadStart
                        - sizeof(uint)
                        - (1 + contentBytes)
                        - (1 + packetBytes);
        var bodyLengthOffset = bodyStart - sizeof(uint);
        Assert.Equal(1, golden[bodyLengthOffset - 1]);
        var replacementBytes = ZLinkRelocationTreeStore.ChunkBytes;
        var expanded = GC.AllocateUninitializedArray<byte>(
            checked(golden.Length - originalPayload.Length
                    + replacementBytes));
        golden.AsSpan(0, payloadStart).CopyTo(expanded);
        expanded.AsSpan(payloadStart, replacementBytes).Fill(0x5e);
        golden.AsSpan(payloadStart + originalPayload.Length).CopyTo(
            expanded.AsSpan(payloadStart + replacementBytes));
        BinaryPrimitives.WriteUInt32BigEndian(
            expanded.AsSpan(payloadStart - sizeof(uint)),
            checked((uint)replacementBytes));
        var oldBodyLength = BinaryPrimitives.ReadUInt32BigEndian(
            golden.AsSpan(bodyLengthOffset));
        BinaryPrimitives.WriteUInt32BigEndian(
            expanded.AsSpan(bodyLengthOffset),
            checked(oldBodyLength
                    + (uint)(replacementBytes - originalPayload.Length)));
        using var input = new NonSeekableReadStream(expanded);

        var before = GC.GetAllocatedBytesForCurrentThread();
        var restored = ZLinkRelocationEnvelopeCodec.Decode(input);
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        var restoredRequest = Assert.IsType<ZLinkCanonicalAcceptedRequest>(
            restored.Participants
                .SelectMany(static participant => participant.AcceptedJobs)
                .Select(static job => job.CanonicalRequest)
                .First(static request => request is not null));
        Assert.Equal(
            replacementBytes,
            restoredRequest.ApplicationPayload.Payload.Length);
        Assert.True(MemoryMarshal.TryGetArray(
            restored.CanonicalLogicalStream,
            out var logicalSegment));
        Assert.True(MemoryMarshal.TryGetArray(
            restoredRequest.ApplicationPayload.Payload,
            out var payloadSegment));
        Assert.Same(logicalSegment.Array, payloadSegment.Array);
        Assert.InRange(
            allocated,
            expanded.Length,
            2L * expanded.Length + 16L * 1024 * 1024);
    }

    [Fact]
    public void NonSeekableCanonicalTerminalPayloadIsNotJoinedOrCopied()
    {
        var golden = ReadCanonicalRelocationGolden();
        var projected = ZLinkRelocationEnvelopeCodec.Decode(golden);
        var completion = projected.Participants
            .SelectMany(static participant => participant.TerminalCompletions)
            .First(static item => item.Payload is not null);
        var applicationPayload = Assert.IsType<ZLinkCanonicalApplicationPayload>(
            completion.Payload);
        var originalPayload = applicationPayload.Payload.ToArray();
        var payloadStart = golden.AsSpan().LastIndexOf(originalPayload);
        Assert.True(payloadStart > 0);
        var packetBytes = Encoding.UTF8.GetByteCount(
            applicationPayload.PacketName);
        var contentBytes = Encoding.UTF8.GetByteCount(
            applicationPayload.ContentType);
        var bodyStart = payloadStart
                        - sizeof(uint)
                        - (1 + contentBytes)
                        - (1 + packetBytes);
        var bodyLengthOffset = bodyStart - sizeof(uint);
        Assert.Equal(1, golden[bodyLengthOffset - 1]);
        var replacementBytes = ZLinkRelocationTreeStore.ChunkBytes;
        var expanded = GC.AllocateUninitializedArray<byte>(
            checked(golden.Length - originalPayload.Length
                    + replacementBytes));
        golden.AsSpan(0, payloadStart).CopyTo(expanded);
        expanded.AsSpan(payloadStart, replacementBytes).Fill(0x3e);
        golden.AsSpan(payloadStart + originalPayload.Length).CopyTo(
            expanded.AsSpan(payloadStart + replacementBytes));
        BinaryPrimitives.WriteUInt32BigEndian(
            expanded.AsSpan(payloadStart - sizeof(uint)),
            checked((uint)replacementBytes));
        var oldBodyLength = BinaryPrimitives.ReadUInt32BigEndian(
            golden.AsSpan(bodyLengthOffset));
        BinaryPrimitives.WriteUInt32BigEndian(
            expanded.AsSpan(bodyLengthOffset),
            checked(oldBodyLength
                    + (uint)(replacementBytes - originalPayload.Length)));
        using var input = new NonSeekableReadStream(expanded);

        var before = GC.GetAllocatedBytesForCurrentThread();
        var restored = ZLinkRelocationEnvelopeCodec.Decode(input);
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        var restoredCompletion = restored.Participants
            .SelectMany(static participant => participant.TerminalCompletions)
            .First(static item => item.Payload is not null);
        var restoredPayload =
            Assert.IsType<ZLinkCanonicalApplicationPayload>(
                restoredCompletion.Payload).Payload;
        Assert.Equal(replacementBytes, restoredPayload.Length);
        Assert.All(
            restored.Participants,
            static participant =>
                Assert.True(participant.CompletionPayload.IsEmpty));
        Assert.True(MemoryMarshal.TryGetArray(
            restored.CanonicalLogicalStream,
            out var logicalSegment));
        Assert.True(MemoryMarshal.TryGetArray(
            restoredPayload,
            out var payloadSegment));
        Assert.Same(logicalSegment.Array, payloadSegment.Array);
        Assert.InRange(
            allocated,
            expanded.Length,
            2L * expanded.Length + 16L * 1024 * 1024);
    }

    private static byte[] ReadCanonicalRelocationGolden()
    {
        var frameworkRoot =
            Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var path = Path.GetFullPath(
            "../../runtime/protocol/golden/relocation-envelope-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        return Convert.FromHexString(
            document.RootElement.GetProperty("logicalHex").GetString()!);
    }

    private static byte[] CreateCanonicalEnvelopeWithState(int stateBytes)
    {
        using var stream = new MemoryStream(
            checked(stateBytes + 256));
        stream.Write(Enumerable.Range(1, 16)
            .Select(static value => (byte)value)
            .ToArray());
        stream.WriteByte(1); // Actor.
        using (var objectBody = new MemoryStream())
        {
            objectBody.WriteByte(5);
            objectBody.Write("actor"u8);
            WriteUInt64BigEndian(objectBody, 1);
            WriteUInt64BigEndian(objectBody, 1);
            WriteUInt16BigEndian(
                stream,
                checked((ushort)objectBody.Length));
            objectBody.Position = 0;
            objectBody.CopyTo(stream);
        }
        WriteUInt64BigEndian(stream, 1); // Application version.
        WriteUInt32BigEndian(stream, 1); // State count.
        WriteUInt64BigEndian(stream, 1); // Participant id.
        stream.WriteByte(1); // Application state only.
        WriteUInt64BigEndian(
            stream,
            checked((ulong)stateBytes + sizeof(ulong)));
        WriteUInt64BigEndian(stream, checked((ulong)stateBytes));
        stream.Write(new byte[stateBytes]);
        WriteUInt32BigEndian(stream, 1); // Progress count.
        WriteUInt64BigEndian(stream, 1); // Participant id.
        WriteUInt64BigEndian(stream, 0); // Accepted boundary.
        WriteUInt64BigEndian(stream, 0); // Replay cursor.
        WriteUInt32BigEndian(stream, 0); // Journal count.
        WriteUInt32BigEndian(stream, 0); // Timer count.
        WriteUInt32BigEndian(stream, 0); // Pending tick count.
        WriteUInt32BigEndian(stream, 0); // Completion count.
        return stream.ToArray();
    }

    private static void WriteUInt16BigEndian(Stream stream, ushort value)
    {
        Span<byte> encoded = stackalloc byte[sizeof(ushort)];
        BinaryPrimitives.WriteUInt16BigEndian(encoded, value);
        stream.Write(encoded);
    }

    private static void WriteUInt32BigEndian(Stream stream, uint value)
    {
        Span<byte> encoded = stackalloc byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32BigEndian(encoded, value);
        stream.Write(encoded);
    }

    private static void WriteUInt64BigEndian(Stream stream, ulong value)
    {
        Span<byte> encoded = stackalloc byte[sizeof(ulong)];
        BinaryPrimitives.WriteUInt64BigEndian(encoded, value);
        stream.Write(encoded);
    }

    private static ZLinkRelocationEnvelope CreateSpotWideEnvelope()
    {
        const int stateBytes = 64 * 1024;
        var participants =
            new List<ZLinkRelocationParticipantEnvelope>(101);
        var spotState = GC.AllocateUninitializedArray<byte>(stateBytes);
        Array.Fill(spotState, (byte)0x51);
        participants.Add(
            new ZLinkRelocationParticipantEnvelope(
                new ZLinkAuthorityKey("spot:mesh:room"),
                ZLinkPlacementObjectKind.UserSpot,
                1,
                1,
                spotState,
                [],
                []));
        for (var index = 0; index < 100; index++)
        {
            var actorState =
                GC.AllocateUninitializedArray<byte>(stateBytes);
            Array.Fill(actorState, checked((byte)index));
            participants.Add(
                new ZLinkRelocationParticipantEnvelope(
                    new ZLinkAuthorityKey(
                        $"actor:mesh:player-{index:D3}"),
                    ZLinkPlacementObjectKind.Actor,
                    1,
                    1,
                    actorState,
                    [],
                    []));
        }
        return new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            SHA256.HashData("spot-wide-parallel-io"u8),
            participants);
    }

    private sealed class ConcurrentRelocationStore
        : IZLinkRelocationRepository
    {
        private readonly ConcurrentDictionary<string, byte[]> _payloads =
            new(StringComparer.Ordinal);
        private readonly ConcurrentDictionary<string, byte[]> _hidden =
            new(StringComparer.Ordinal);
        private readonly ConcurrentDictionary<string, byte> _corrupted =
            new(StringComparer.Ordinal);
        private readonly ConcurrentQueue<string> _completedPutKinds = new();
        private int _nextReference;
        private int _chunkPutOrdinal;
        private int _activeChunkPuts;
        private int _maxConcurrentChunkPuts;
        private long _chunkPayloadBytesInFlight;
        private long _maxChunkPayloadBytesInFlight;
        private int _activeChunkReads;
        private int _maxConcurrentChunkReads;
        private int _activeChunkRenews;
        private int _maxConcurrentChunkRenews;
        private int _manifestPutCount;
        private TaskCompletionSource _secondChunkRenewEntered = NewSignal();

        internal int FailChunkPutOrdinal { get; init; }
        internal int MaxConcurrentChunkPuts =>
            Volatile.Read(ref _maxConcurrentChunkPuts);
        internal long MaxChunkPayloadBytesInFlight =>
            Volatile.Read(ref _maxChunkPayloadBytesInFlight);
        internal int MaxConcurrentChunkReads =>
            Volatile.Read(ref _maxConcurrentChunkReads);
        internal int MaxConcurrentChunkRenews =>
            Volatile.Read(ref _maxConcurrentChunkRenews);
        internal int ManifestPutCount =>
            Volatile.Read(ref _manifestPutCount);
        internal string[] CompletedPutKinds =>
            _completedPutKinds.ToArray();
        internal string[] ChunkReferences =>
            _payloads
                .Where(static item => IsKind(item.Value, "ZLTC"u8))
                .Select(static item => item.Key)
                .Order(StringComparer.Ordinal)
                .ToArray();

        public async ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var isChunk = IsKind(payload.Span, "ZLTC"u8);
            var isManifest =
                IsKind(payload.Span, "ZLTM"u8)
                || IsKind(payload.Span, "ZLPM"u8);
            if (isManifest)
                Interlocked.Increment(ref _manifestPutCount);
            if (isChunk)
            {
                var ordinal = Interlocked.Increment(ref _chunkPutOrdinal);
                var active = Interlocked.Increment(ref _activeChunkPuts);
                UpdateMaximum(ref _maxConcurrentChunkPuts, active);
                var bytes = Interlocked.Add(
                    ref _chunkPayloadBytesInFlight,
                    payload.Length);
                UpdateMaximum(ref _maxChunkPayloadBytesInFlight, bytes);
                try
                {
                    await Task.Delay(40, cancellationToken);
                    if (ordinal == FailChunkPutOrdinal)
                        throw new IOException("Injected chunk write failure.");
                }
                finally
                {
                    Interlocked.Add(
                        ref _chunkPayloadBytesInFlight,
                        -payload.Length);
                    Interlocked.Decrement(ref _activeChunkPuts);
                }
            }

            var reference =
                $"{Interlocked.Increment(ref _nextReference):D8}";
            var bytesCopy = payload.ToArray();
            _payloads[reference] = bytesCopy;
            _completedPutKinds.Enqueue(
                isChunk ? "chunk" : isManifest ? "manifest" : "other");
            var now = DateTimeOffset.UtcNow;
            return new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(bytesCopy),
                now + retention,
                now);
        }

        public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
            string reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var bytes = payload.ToArray();
            if (_payloads.TryGetValue(reference, out var current)
                && !current.AsSpan().SequenceEqual(bytes))
                throw new InvalidDataException("Relocation reference collision.");
            _payloads[reference] = bytes;
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(bytes),
                now + retention,
                now));
        }

        public async ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default)
        {
            if (!_payloads.TryGetValue(reference, out var payload))
                return new ZLinkRelocationReadResult.Missing();
            var isChunk = IsKind(payload, "ZLTC"u8);
            if (isChunk)
            {
                var active = Interlocked.Increment(ref _activeChunkReads);
                UpdateMaximum(ref _maxConcurrentChunkReads, active);
                try
                {
                    var order = BinaryPrimitives.ReadUInt32BigEndian(
                        payload.AsSpan(11, 4));
                    await Task.Delay(
                        order == 0 ? 20 : 5,
                        cancellationToken);
                }
                finally
                {
                    Interlocked.Decrement(ref _activeChunkReads);
                }
            }
            return new ZLinkRelocationReadResult.Found(payload);
        }

        public async ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
            string reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            if (_payloads.TryGetValue(reference, out var payload)
                && IsKind(payload, "ZLTC"u8))
            {
                var active = Interlocked.Increment(ref _activeChunkRenews);
                UpdateMaximum(ref _maxConcurrentChunkRenews, active);
                try
                {
                    if (active == 1)
                        await _secondChunkRenewEntered.Task.WaitAsync(
                            TimeSpan.FromSeconds(10),
                            cancellationToken);
                    else
                        _secondChunkRenewEntered.TrySetResult();
                }
                finally
                {
                    Interlocked.Decrement(ref _activeChunkRenews);
                }
            }
            var now = DateTimeOffset.UtcNow;
            return
                _payloads.ContainsKey(reference)
                    ? new ZLinkRelocationRenewResult.Renewed(
                        now + retention,
                        now)
                    : new ZLinkRelocationRenewResult.Missing();
        }

        public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(
                _payloads.TryRemove(reference, out _)
                    ? ZLinkRelocationDeleteResult.Deleted
                    : ZLinkRelocationDeleteResult.Missing);

        internal void ResetReadConcurrency()
        {
            Volatile.Write(ref _activeChunkReads, 0);
            Volatile.Write(ref _maxConcurrentChunkReads, 0);
        }

        internal void ResetRenewConcurrency()
        {
            Volatile.Write(ref _activeChunkRenews, 0);
            Volatile.Write(ref _maxConcurrentChunkRenews, 0);
            _secondChunkRenewEntered = NewSignal();
        }

        internal void Corrupt(string reference)
        {
            var payload = _payloads[reference];
            payload[^1] ^= 0xff;
            _corrupted[reference] = 0;
        }

        internal void UndoCorruption(string reference)
        {
            if (_corrupted.TryRemove(reference, out _))
                _payloads[reference][^1] ^= 0xff;
        }

        internal void Hide(string reference)
        {
            if (_payloads.TryRemove(reference, out var payload))
                _hidden[reference] = payload;
        }

        internal void Show(string reference)
        {
            if (_hidden.TryRemove(reference, out var payload))
                _payloads[reference] = payload;
        }

        private static bool IsKind(
            ReadOnlySpan<byte> payload,
            ReadOnlySpan<byte> magic) =>
            payload.Length >= magic.Length
            && payload[..magic.Length].SequenceEqual(magic);

        private static TaskCompletionSource NewSignal() =>
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        private static void UpdateMaximum(ref int maximum, int value)
        {
            var observed = Volatile.Read(ref maximum);
            while (observed < value)
            {
                var current = Interlocked.CompareExchange(
                    ref maximum,
                    value,
                    observed);
                if (current == observed)
                    return;
                observed = current;
            }
        }

        private static void UpdateMaximum(ref long maximum, long value)
        {
            var observed = Volatile.Read(ref maximum);
            while (observed < value)
            {
                var current = Interlocked.CompareExchange(
                    ref maximum,
                    value,
                    observed);
                if (current == observed)
                    return;
                observed = current;
            }
        }
    }

    private sealed class NonSeekableReadStream(byte[] payload)
        : MemoryStream(payload, writable: false)
    {
        public override bool CanSeek => false;

        public override long Seek(long offset, SeekOrigin loc) =>
            throw new NotSupportedException();

        public override long Position
        {
            get => base.Position;
            set => throw new NotSupportedException();
        }
    }

    private sealed class RepeatedLengthList(
        int count,
        long value) : IReadOnlyList<long>
    {
        public int Count { get; } = count;
        public long this[int index] =>
            index >= 0 && index < Count
                ? value
                : throw new ArgumentOutOfRangeException(nameof(index));
        public IEnumerator<long> GetEnumerator()
        {
            for (var index = 0; index < Count; index++)
                yield return value;
        }
        System.Collections.IEnumerator
            System.Collections.IEnumerable.GetEnumerator() =>
            GetEnumerator();
    }
}
