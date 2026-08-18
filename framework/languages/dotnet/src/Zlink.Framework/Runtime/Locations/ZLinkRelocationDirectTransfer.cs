using System.Buffers.Binary;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Source-side relocation payload for the direct-transfer contract
/// (spec 28 §4.2). The captured relocation envelope is encoded once into
/// memory; the encoded logical stream is the single handoff origin, split
/// into relocationState (command 52) chunks. The manifest values
/// (total length, chunk count, CRC-32C) are carried by relocationPrepare
/// (command 40) and verified by the target after reassembly.
/// </summary>
internal sealed class ZLinkRelocationTransferPayload
{
    private const uint EnvelopeMagic = 0x5a4c4452; // ZLDR
    private const byte EnvelopeVersion = 1;
    private const int InventoryDigestBytes = 32;
    private readonly byte[] _encoded;

    private ZLinkRelocationTransferPayload(byte[] encoded, int chunkBytes)
    {
        _encoded = encoded;
        ChunkBytes = chunkBytes;
        ChunkCount = (int)(((long)encoded.Length + chunkBytes - 1)
                           / chunkBytes);
        ChecksumCrc32c = ZLinkCrc32C.Compute(encoded);
    }

    internal long TotalLength => _encoded.LongLength;

    internal uint ChecksumCrc32c { get; }

    internal int ChunkCount { get; }

    internal int ChunkBytes { get; }

    internal ReadOnlyMemory<byte> Encoded => _encoded;

    /// <summary>
    /// Encodes the envelope's logical stream into memory and fixes the chunk
    /// plan. The effective chunk size is the configured limit, grown only
    /// when the wire chunk-count bound (4096) would otherwise be exceeded,
    /// and never beyond the wire chunk-byte bound (64 MiB).
    /// </summary>
    internal static ZLinkRelocationTransferPayload Create(
        ZLinkRelocationEnvelope envelope,
        long effectiveChunkLimit)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        if (effectiveChunkLimit <= 0)
            throw new ArgumentOutOfRangeException(nameof(effectiveChunkLimit));
        var encoded = EncodeEnvelope(envelope);
        if (encoded.LongLength == 0)
            throw new InvalidOperationException(
                "The relocation logical stream is empty.");
        var chunkBytes = Math.Min(
            effectiveChunkLimit,
            ZLinkServiceWireCodec.RelocationChunkBytesBound);
        var minimumChunkBytes =
            (encoded.LongLength
             + ZLinkServiceWireCodec.RelocationChunkCountBound - 1)
            / ZLinkServiceWireCodec.RelocationChunkCountBound;
        chunkBytes = Math.Max(chunkBytes, minimumChunkBytes);
        if (chunkBytes > ZLinkServiceWireCodec.RelocationChunkBytesBound)
            throw new InvalidOperationException(
                "The relocation payload exceeds the wire chunk bounds.");
        return new ZLinkRelocationTransferPayload(
            encoded,
            checked((int)chunkBytes));
    }

    internal static ZLinkRelocationEnvelope DecodeEnvelope(
        ReadOnlyMemory<byte> encoded)
    {
        if (encoded.Length < sizeof(uint) + 1 + InventoryDigestBytes)
            throw new ZLinkRelocationDataLostException(
                "The direct relocation envelope is truncated.");
        var bytes = encoded.Span;
        if (BinaryPrimitives.ReadUInt32BigEndian(bytes) != EnvelopeMagic
            || bytes[sizeof(uint)] != EnvelopeVersion)
            throw new ZLinkRelocationDataLostException(
                "The direct relocation envelope framing is invalid.");
        var digest = bytes.Slice(sizeof(uint) + 1, InventoryDigestBytes)
            .ToArray();
        var logical = bytes.Slice(sizeof(uint) + 1 + InventoryDigestBytes)
            .ToArray();
        if (logical.Length == 0)
            throw new ZLinkRelocationDataLostException(
                "The direct relocation envelope logical stream is empty.");
        using var stream = new MemoryStream(logical, writable: false);
        return ZLinkRelocationEnvelopeCodec.Decode(stream, digest);
    }

    private static byte[] EncodeEnvelope(ZLinkRelocationEnvelope envelope)
    {
        if (envelope.InventoryDigest.Length != InventoryDigestBytes)
            throw new ArgumentOutOfRangeException(nameof(envelope));
        using var logical = new MemoryStream();
        ZLinkRelocationEnvelopeCodec.EncodeTo(logical, envelope);
        if (logical.Length == 0)
            throw new InvalidOperationException(
                "The relocation logical stream is empty.");
        var encoded = new byte[checked(
            sizeof(uint) + 1 + InventoryDigestBytes + logical.Length)];
        BinaryPrimitives.WriteUInt32BigEndian(encoded, EnvelopeMagic);
        encoded[sizeof(uint)] = EnvelopeVersion;
        envelope.InventoryDigest.Span.CopyTo(
            encoded.AsSpan(sizeof(uint) + 1, InventoryDigestBytes));
        logical.GetBuffer().AsSpan(0, checked((int)logical.Length)).CopyTo(
            encoded.AsSpan(sizeof(uint) + 1 + InventoryDigestBytes));
        return encoded;
    }

    internal ReadOnlyMemory<byte> Chunk(int ordinal)
    {
        if (ordinal < 0 || ordinal >= ChunkCount)
            throw new ArgumentOutOfRangeException(nameof(ordinal));
        var offset = (long)ordinal * ChunkBytes;
        var length = (int)Math.Min(ChunkBytes, _encoded.LongLength - offset);
        return _encoded.AsMemory(checked((int)offset), length);
    }
}

/// <summary>
/// Target-side reassembly buffer for relocationState (command 52) chunks of
/// one exact relocation identity. Chunk bytes are copied into the
/// framework-owned buffer as they arrive; the full-payload CRC-32C is
/// verified only once every declared chunk has arrived. A manifest that
/// disagrees with the first declaration for the same exact identity is a
/// conflict; a checksum mismatch is an explicit, non-retryable failure and
/// never restores from a partial assembly (spec 28 §4.3, §9).
/// </summary>
internal sealed class ZLinkRelocationChunkAssembler
{
    private readonly byte[] _buffer;
    private readonly object _gate = new();
    private readonly List<int> _chunkLengths = [];
    private readonly TaskCompletionSource _completed = new(
        TaskCreationOptions.RunContinuationsAsynchronously);
    private long _received;

    internal ZLinkRelocationChunkAssembler(
        ulong totalLength,
        uint chunkCount,
        uint checksumCrc32c)
    {
        if (totalLength is 0 or > int.MaxValue
            || chunkCount is 0
            || chunkCount > ZLinkServiceWireCodec.RelocationChunkCountBound)
            throw new ZLinkRelocationDataLostException(
                "The relocation payload manifest bounds are invalid.");
        TotalLength = totalLength;
        ChunkCount = chunkCount;
        ChecksumCrc32c = checksumCrc32c;
        _buffer = new byte[totalLength];
    }

    internal ulong TotalLength { get; }

    internal uint ChunkCount { get; }

    internal uint ChecksumCrc32c { get; }

    internal Task Completed => _completed.Task;

    /// <summary>Poisons the assembly so a waiting prepare fails explicitly.</summary>
    internal void Fail(Exception failure) =>
        _completed.TrySetException(failure);

    internal bool MatchesManifest(
        ulong totalLength,
        uint chunkCount,
        uint checksumCrc32c) =>
        totalLength == TotalLength
        && chunkCount == ChunkCount
        && checksumCrc32c == ChecksumCrc32c;

    /// <summary>
    /// Copies one chunk into the assembly buffer. Chunks must arrive in
    /// ordinal order on the ordered connection; an identical re-delivery of
    /// an already-assembled ordinal is idempotent, anything else is a
    /// conflict failure.
    /// </summary>
    internal void Append(uint ordinal, ReadOnlySpan<byte> data)
    {
        lock (_gate)
        {
            if (ordinal < _chunkLengths.Count)
            {
                if (data.Length != _chunkLengths[checked((int)ordinal)])
                    throw new ZLinkRelocationDataLostException(
                        "A duplicate relocation chunk changed its length.");
                return;
            }
            if (ordinal != _chunkLengths.Count
                || ordinal >= ChunkCount
                || data.Length == 0
                || (ulong)data.Length > TotalLength - (ulong)_received)
                throw new ZLinkRelocationDataLostException(
                    "A relocation chunk violated its declared manifest.");
            data.CopyTo(_buffer.AsSpan(checked((int)_received)));
            _received += data.Length;
            _chunkLengths.Add(data.Length);
            if (_chunkLengths.Count == ChunkCount)
            {
                if ((ulong)_received != TotalLength)
                    throw new ZLinkRelocationDataLostException(
                        "The assembled relocation payload length does not match its manifest.");
                _completed.TrySetResult();
            }
        }
    }

    /// <summary>
    /// Verifies the manifest CRC-32C over the complete assembled payload and
    /// decodes the relocation envelope. A mismatch is an explicit failure —
    /// no retry, no partial restore.
    /// </summary>
    internal ZLinkRelocationEnvelope VerifyAndDecode()
    {
        lock (_gate)
        {
            if (_chunkLengths.Count != ChunkCount
                || (ulong)_received != TotalLength)
                throw new ZLinkRelocationDataLostException(
                    "The relocation payload assembly is incomplete.");
            if (ZLinkCrc32C.Compute(_buffer) != ChecksumCrc32c)
                throw new ZLinkRelocationDataLostException(
                    "The relocation payload checksum does not match its manifest.");
            return ZLinkRelocationTransferPayload.DecodeEnvelope(_buffer);
        }
    }
}

/// <summary>
/// Computes the cutover boundary values (spec 28 §4.4): the count of
/// post-capture ingress-hold records relayed via relocationData (command 31)
/// before the cutover, and a CRC-32C over the concatenated encoded frozen
/// records in relay order. The captured saved-work prefix and timers never
/// count — they travel only in relocationState chunks.
/// </summary>
internal static class ZLinkRelocationBoundaryBatch
{
    internal static uint ComputeChecksum(
        IEnumerable<ReadOnlyMemory<byte>> frozenRecords)
    {
        ArgumentNullException.ThrowIfNull(frozenRecords);
        var crc = uint.MaxValue;
        foreach (var record in frozenRecords)
            ZLinkCrc32C.Append(ref crc, record.Span);
        return ~crc;
    }
}

/// <summary>
/// In-flight relocation chunk byte budget (spec 28 §5.3). One instance per
/// peer connection plus one node-wide instance. Phase 1 accounting is the
/// submit-to-terminal window of each chunk; the effective budget is
/// min(configured, conservative constant) until Core exposes a per-pipe
/// accounted-charge observation API. A budget of zero disables the gate.
/// Waiters are served in FIFO order and a single chunk larger than the
/// budget is still admitted when the gate is idle — no payload size is
/// unstartable.
/// </summary>
internal sealed class ZLinkRelocationTransferBudget
{
    //  Phase 1 conservative cap: derived from the smallest per-pipe-role
    //  receive floor the mesh transport guarantees (16 MiB default frame
    //  budget for mesh peer pipes). Replace with min(configured,
    //  applied HWM / 2) once the Core observation API exists.
    internal const long ConservativeBudgetBytes = 16L * 1024 * 1024;

    private readonly long _budget;
    private readonly object _gate = new();
    private readonly Queue<(long Bytes, TaskCompletionSource Waiter)> _waiters
        = new();
    private long _inFlight;

    internal ZLinkRelocationTransferBudget(long configuredBudget)
    {
        if (configuredBudget < 0)
            throw new ArgumentOutOfRangeException(nameof(configuredBudget));
        _budget = configuredBudget == 0
            ? 0
            : Math.Min(configuredBudget, ConservativeBudgetBytes);
    }

    internal long InFlightBytes
    {
        get
        {
            lock (_gate)
                return _inFlight;
        }
    }

    internal bool Enabled => _budget > 0;

    /// <summary>
    /// Waits for headroom, then charges the chunk. Returns immediately when
    /// the budget is disabled or the gate is idle.
    /// </summary>
    internal async ValueTask ChargeAsync(
        long bytes,
        CancellationToken cancellationToken)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(bytes);
        if (_budget == 0)
            return;
        TaskCompletionSource waiter;
        lock (_gate)
        {
            if (_waiters.Count == 0
                && (_inFlight == 0 || _inFlight + bytes <= _budget))
            {
                _inFlight += bytes;
                return;
            }
            waiter = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _waiters.Enqueue((bytes, waiter));
        }
        //  Release pre-charges the admitted waiter under the gate; the
        //  awaiter only observes the completion. On cancellation the
        //  reservation is returned if the waiter had already been admitted.
        try
        {
            await waiter.Task.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            if (waiter.Task.IsCompletedSuccessfully)
                Release(bytes);
            throw;
        }
    }

    internal void Release(long bytes)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(bytes);
        if (_budget == 0)
            return;
        List<TaskCompletionSource>? admitted = null;
        lock (_gate)
        {
            _inFlight = Math.Max(0, _inFlight - bytes);
            while (_waiters.Count > 0)
            {
                var (nextBytes, waiter) = _waiters.Peek();
                if (_inFlight != 0 && _inFlight + nextBytes > _budget)
                    break;
                _waiters.Dequeue();
                _inFlight += nextBytes;
                (admitted ??= []).Add(waiter);
            }
        }
        if (admitted is not null)
            foreach (var waiter in admitted)
                waiter.TrySetResult();
    }
}
