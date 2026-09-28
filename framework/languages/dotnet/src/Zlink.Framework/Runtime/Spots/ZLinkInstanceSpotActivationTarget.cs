using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkInstanceSpotAuthorityState : byte
{
    Creating = 1,
    Ready = 2,
}

internal sealed record ZLinkInstanceSpotAuthorityPayload(
    ZLinkInstanceSpotAuthorityState State,
    string SpotId,
    string StableType,
    string MeshName,
    RoutingId NodeRid,
    ulong NodeGeneration,
    string OwnerId,
    ulong OwnerLeaseGeneration,
    ZLinkInstanceSpotActivationRecoveryPointer? ActivationRecovery
);

internal sealed record ZLinkInstanceSpotActivationRecoveryPointer(
    string Reference,
    byte[] Sha256,
    uint EncodedSize,
    ulong InboxSequence,
    ulong ReplayCursor
);

internal static class ZLinkInstanceSpotAuthorityPayloadCodec
{
    private static ReadOnlySpan<byte> Magic => "ZLAU"u8;
    private const byte Version = 1;
    private const int MaximumBytes = 1024 * 1024;

    internal static byte[] Encode(ZLinkInstanceSpotAuthorityPayload payload)
    {
        ArgumentNullException.ThrowIfNull(payload);
        if (
            payload.ActivationRecovery is not null
            && payload.State != ZLinkInstanceSpotAuthorityState.Ready
        )
            throw new ArgumentOutOfRangeException(nameof(payload));

        var instance = new Writer();
        instance.Text8(payload.StableType);
        instance.Text8(payload.SpotId);
        var spot = new Writer();
        spot.U8(
            payload.State switch
            {
                ZLinkInstanceSpotAuthorityState.Creating => 1,
                ZLinkInstanceSpotAuthorityState.Ready => 2,
                _ => throw new ArgumentOutOfRangeException(nameof(payload)),
            }
        );
        spot.U16(checked((ushort)instance.Count));
        spot.Bytes(instance.WrittenSpan);
        var identity = new Writer();
        identity.U8(3);
        identity.U16(checked((ushort)spot.Count));
        identity.Bytes(spot.WrittenSpan);

        var body = new Writer();
        body.U8(payload.State == ZLinkInstanceSpotAuthorityState.Creating ? (byte)1 : (byte)0);
        body.U8(2);
        body.U16(checked((ushort)identity.Count));
        body.Bytes(identity.WrittenSpan);
        body.Text8(payload.OwnerId);
        body.NonZeroU64(payload.OwnerLeaseGeneration);
        body.Text8(payload.MeshName);
        body.Rid(payload.NodeRid);
        body.NonZeroU64(payload.NodeGeneration);
        body.U8(0);
        body.U32(0);
        if (payload.ActivationRecovery is { } recovery)
        {
            if (
                recovery.Sha256.Length != SHA256.HashSizeInBytes
                || recovery.EncodedSize > MaximumBytes
                || recovery.InboxSequence == 0
                || recovery.ReplayCursor > recovery.InboxSequence
            )
                throw new ArgumentOutOfRangeException(nameof(payload));
            var recoveryBody = new Writer();
            recoveryBody.Text16(recovery.Reference);
            recoveryBody.U8(SHA256.HashSizeInBytes);
            recoveryBody.Bytes(recovery.Sha256);
            recoveryBody.U32(recovery.EncodedSize);
            recoveryBody.NonZeroU64(recovery.InboxSequence);
            recoveryBody.U64(recovery.ReplayCursor);
            body.U8(1);
            body.U32(checked((uint)recoveryBody.Count));
            body.Bytes(recoveryBody.WrittenSpan);
        }
        else
        {
            body.U8(0);
            body.U32(0);
        }

        var result = new Writer();
        result.Bytes(Magic);
        result.U8(Version);
        result.U16(0);
        result.U32(checked((uint)body.Count));
        result.Bytes(body.WrittenSpan);
        result.U32(Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(result.WrittenSpan));
        if (result.Count > MaximumBytes)
            throw new ArgumentOutOfRangeException(nameof(payload));
        return result.ToArray();
    }

    internal static bool TryDecode(
        ReadOnlySpan<byte> encoded,
        out ZLinkInstanceSpotAuthorityPayload payload
    )
    {
        payload = null!;
        try
        {
            if (encoded.Length is < 15 or > MaximumBytes || !encoded[..4].SequenceEqual(Magic))
                return false;
            var reader = new Reader(encoded);
            reader.Skip(4);
            if (reader.U8() != Version || reader.U16() != 0)
                return false;
            var body = reader.Slice(checked((int)reader.U32()));
            var checksumOffset = reader.Offset;
            if (
                reader.U32()
                    != Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(
                        encoded[..checksumOffset]
                    )
                || !reader.End
            )
                return false;
            var operation = body.U8();
            if (body.U8() != 2)
                return false;
            var identity = body.Slice(body.U16());
            if (identity.U8() != 3)
                return false;
            var spot = identity.Slice(identity.U16());
            var instanceState = spot.U8();
            var instance = spot.Slice(spot.U16());
            var stableType = instance.Text8();
            var spotId = instance.Text8();
            if (!instance.End || !spot.End || !identity.End)
                return false;
            var state = instanceState switch
            {
                1 when operation == 1 => ZLinkInstanceSpotAuthorityState.Creating,
                2 when operation == 0 => ZLinkInstanceSpotAuthorityState.Ready,
                _ => (ZLinkInstanceSpotAuthorityState)0,
            };
            if (state == 0)
                return false;
            var ownerId = body.Text8();
            var ownerLeaseGeneration = body.NonZeroU64();
            var meshName = body.Text8();
            var nodeRid = body.Rid();
            var nodeGeneration = body.NonZeroU64();
            if (body.U8() != 0 || body.U32() != 0)
                return false;
            var hasRecovery = body.U8();
            var recoveryBody = body.Slice(checked((int)body.U32()));
            ZLinkInstanceSpotActivationRecoveryPointer? recovery = null;
            if (hasRecovery == 1)
            {
                if (state != ZLinkInstanceSpotAuthorityState.Ready)
                    return false;
                var reference = recoveryBody.Text16();
                var digestLength = recoveryBody.U8();
                if (digestLength != SHA256.HashSizeInBytes)
                    return false;
                var sha256 = recoveryBody.Bytes(digestLength).ToArray();
                var encodedSize = recoveryBody.U32();
                var inboxSequence = recoveryBody.NonZeroU64();
                var replayCursor = recoveryBody.U64();
                if (encodedSize > MaximumBytes || replayCursor > inboxSequence)
                    return false;
                recovery = new ZLinkInstanceSpotActivationRecoveryPointer(
                    reference,
                    sha256,
                    encodedSize,
                    inboxSequence,
                    replayCursor
                );
            }
            else if (hasRecovery != 0 || !recoveryBody.End)
            {
                return false;
            }
            if (!recoveryBody.End || !body.End)
                return false;
            payload = new ZLinkInstanceSpotAuthorityPayload(
                state,
                spotId,
                stableType,
                meshName,
                nodeRid,
                nodeGeneration,
                ownerId,
                ownerLeaseGeneration,
                recovery
            );
            return true;
        }
        catch (Exception error)
            when (error
                    is InvalidDataException
                        or OverflowException
                        or DecoderFallbackException
                        or ArgumentException
            )
        {
            return false;
        }
    }

    private sealed class Writer
    {
        private readonly MemoryStream stream = new();
        internal int Count => checked((int)stream.Length);
        internal ReadOnlySpan<byte> WrittenSpan => stream.GetBuffer().AsSpan(0, Count);

        internal void U8(byte value) => stream.WriteByte(value);

        internal void U16(ushort value)
        {
            Span<byte> bytes = stackalloc byte[2];
            BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
            stream.Write(bytes);
        }

        internal void U32(uint value)
        {
            Span<byte> bytes = stackalloc byte[4];
            BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
            stream.Write(bytes);
        }

        internal void U64(ulong value)
        {
            Span<byte> bytes = stackalloc byte[8];
            BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
            stream.Write(bytes);
        }

        internal void NonZeroU64(ulong value)
        {
            if (value == 0)
                throw new ArgumentOutOfRangeException(nameof(value));
            U64(value);
        }

        internal void Text8(string value)
        {
            var bytes = TextBytes(value, byte.MaxValue);
            U8(checked((byte)bytes.Length));
            Bytes(bytes);
        }

        internal void Text16(string value)
        {
            var bytes = TextBytes(value, ushort.MaxValue);
            U16(checked((ushort)bytes.Length));
            Bytes(bytes);
        }

        internal void Rid(RoutingId value)
        {
            var bytes = value.ToBytes();
            if (bytes.Length is 0 or > byte.MaxValue)
                throw new ArgumentOutOfRangeException(nameof(value));
            U8(checked((byte)bytes.Length));
            Bytes(bytes);
        }

        internal void Bytes(ReadOnlySpan<byte> value) => stream.Write(value);

        internal byte[] ToArray() => stream.ToArray();

        private static byte[] TextBytes(string value, int maximum)
        {
            var bytes = new UTF8Encoding(false, true).GetBytes(value);
            if (bytes.Length is 0 || bytes.Length > maximum || value.Contains('\0'))
                throw new ArgumentOutOfRangeException(nameof(value));
            return bytes;
        }
    }

    private ref struct Reader(ReadOnlySpan<byte> source)
    {
        private readonly ReadOnlySpan<byte> source = source;
        internal int Offset { get; private set; }
        internal bool End => Offset == source.Length;

        internal void Skip(int count)
        {
            Require(count);
            Offset += count;
        }

        internal byte U8()
        {
            Require(1);
            return source[Offset++];
        }

        internal ushort U16()
        {
            Require(2);
            var value = BinaryPrimitives.ReadUInt16BigEndian(source[Offset..]);
            Offset += 2;
            return value;
        }

        internal uint U32()
        {
            Require(4);
            var value = BinaryPrimitives.ReadUInt32BigEndian(source[Offset..]);
            Offset += 4;
            return value;
        }

        internal ulong U64()
        {
            Require(8);
            var value = BinaryPrimitives.ReadUInt64BigEndian(source[Offset..]);
            Offset += 8;
            return value;
        }

        internal ulong NonZeroU64()
        {
            var value = U64();
            if (value == 0)
                throw new InvalidDataException();
            return value;
        }

        internal Reader Slice(int count)
        {
            Require(count);
            var value = new Reader(source.Slice(Offset, count));
            Offset += count;
            return value;
        }

        internal ReadOnlySpan<byte> Bytes(int count)
        {
            Require(count);
            var value = source.Slice(Offset, count);
            Offset += count;
            return value;
        }

        internal string Text8() => Text(U8());

        internal string Text16() => Text(U16());

        internal RoutingId Rid()
        {
            var length = U8();
            if (length == 0)
                throw new InvalidDataException();
            return RoutingId.From(Bytes(length));
        }

        private string Text(int length)
        {
            if (length == 0)
                throw new InvalidDataException();
            var value = new UTF8Encoding(false, true).GetString(Bytes(length));
            if (value.Contains('\0'))
                throw new InvalidDataException();
            return value;
        }

        private void Require(int count)
        {
            if (count < 0 || source.Length - Offset < count)
                throw new InvalidDataException();
        }
    }
}

internal static class ZLinkInstanceSpotActivationEnvelopeCodec
{
    private static readonly byte[] Magic = "ZLIA"u8.ToArray();
    private const byte Version = 2;

    internal sealed record ActivationRecord(
        bool IsRequest,
        MeshOperationId OperationId,
        ulong DeadlineUnixMs,
        RoutingId SourceNodeRid,
        ulong SourceNodeGeneration,
        ZLinkServiceWireCodec.RequestSourceFence RequestSource,
        string SourceSpotId,
        ReadOnlyMemory<byte>? Metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> Payload
    );

    internal static byte[] Encode(
        InstanceSpotActivationOperation operation,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload
    )
    {
        if (
            requestSource.NodeRid != operation.SourceNodeRid
            || requestSource.NodeGeneration != operation.SourceNodeGeneration
            || string.IsNullOrWhiteSpace(requestSource.OwnerId)
            || requestSource.LeaseGeneration == 0
        )
            throw new ArgumentException(
                "The Instance Spot activation source fence is invalid.",
                nameof(requestSource)
            );
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(Version);
        writer.Write((byte)1);
        writer.Write(operation.IsRequest);
        writer.Write(operation.OperationId.High);
        writer.Write(operation.OperationId.Low);
        writer.Write(operation.DeadlineUnixMs);
        WriteBytes(writer, operation.SourceNodeRid.ToBytes());
        writer.Write(operation.SourceNodeGeneration);
        WriteText(writer, requestSource.OwnerId);
        writer.Write(requestSource.LeaseGeneration);
        WriteText(writer, operation.SourceSpotId);
        writer.Write(metadata.HasValue);
        if (metadata.HasValue)
            WriteBytes(writer, metadata.Value.Span);
        writer.Write(payload.Count);
        foreach (var part in payload)
            WriteBytes(writer, part.Span);
        return stream.ToArray();
    }

    internal static bool TryDecodeActivation(
        ReadOnlySpan<byte> encoded,
        out ActivationRecord record
    )
    {
        record = null!;
        try
        {
            using var stream = new MemoryStream(encoded.ToArray(), writable: false);
            using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: false);
            if (
                !reader.ReadBytes(Magic.Length).AsSpan().SequenceEqual(Magic)
                || reader.ReadByte() != Version
                || reader.ReadByte() != 1
            )
                return false;
            var request = reader.ReadBoolean();
            var operationId = new MeshOperationId(reader.ReadUInt64(), reader.ReadUInt64());
            var deadline = reader.ReadUInt64();
            var sourceRid = RoutingId.From(ReadBytes(reader));
            var sourceGeneration = reader.ReadUInt64();
            var sourceOwnerId = ReadText(reader);
            var sourceOwnerLeaseGeneration = reader.ReadUInt64();
            if (
                sourceRid.IsEmpty
                || sourceGeneration == 0
                || string.IsNullOrWhiteSpace(sourceOwnerId)
                || sourceOwnerLeaseGeneration == 0
            )
                return false;
            var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
                sourceOwnerId,
                sourceOwnerLeaseGeneration,
                sourceRid,
                sourceGeneration
            );
            var sourceSpotId = ReadText(reader);
            ReadOnlyMemory<byte>? metadata = reader.ReadBoolean() ? ReadBytes(reader) : null;
            var count = reader.ReadInt32();
            if (count is < 1 or > 1024)
                return false;
            var payload = new ReadOnlyMemory<byte>[count];
            for (var index = 0; index < count; index++)
                payload[index] = ReadBytes(reader);
            if (stream.Position != stream.Length)
                return false;
            record = new ActivationRecord(
                request,
                operationId,
                deadline,
                sourceRid,
                sourceGeneration,
                requestSource,
                sourceSpotId,
                metadata,
                payload
            );
            return true;
        }
        catch (Exception error)
            when (error is EndOfStreamException or IOException or ArgumentException)
        {
            return false;
        }
    }

    private static void WriteText(BinaryWriter writer, string value) =>
        WriteBytes(writer, Encoding.UTF8.GetBytes(value));

    private static void WriteBytes(BinaryWriter writer, ReadOnlySpan<byte> value)
    {
        writer.Write(value.Length);
        writer.Write(value);
    }

    private static byte[] ReadBytes(BinaryReader reader)
    {
        var length = reader.ReadInt32();
        if (length is < 0 or > 4 * 1024 * 1024)
            throw new InvalidDataException("Instance Spot activation field length is invalid.");
        var value = reader.ReadBytes(length);
        if (value.Length != length)
            throw new EndOfStreamException();
        return value;
    }

    private static string ReadText(BinaryReader reader) =>
        Encoding.UTF8.GetString(ReadBytes(reader));
}

internal sealed class ZLinkInstanceSpotOperationGate
{
    private readonly ConcurrentDictionary<
        string,
        Lazy<Task<InstanceSpotActivationTerminal>>
    > pending = new(StringComparer.Ordinal);

    internal async Task<InstanceSpotActivationTerminal> RunAsync(
        string operationKey,
        Func<Task<InstanceSpotActivationTerminal>> operation
    )
    {
        var selected = pending.GetOrAdd(
            operationKey,
            _ => new Lazy<Task<InstanceSpotActivationTerminal>>(
                operation,
                LazyThreadSafetyMode.ExecutionAndPublication
            )
        );
        try
        {
            return await selected.Value.ConfigureAwait(false);
        }
        finally
        {
            pending.TryRemove(
                new KeyValuePair<string, Lazy<Task<InstanceSpotActivationTerminal>>>(
                    operationKey,
                    selected
                )
            );
        }
    }
}

internal sealed class ZLinkInstanceSpotActivationTarget(
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore,
    ZLinkSpotNodeCatalog catalog,
    IZLinkBackendSpotNode node,
    ZLinkSpotNodeRegistration registration,
    ZLinkLocationOwnerToken owner
) : IInstanceSpotActivationTarget
{
    private static readonly TimeSpan RecoveryRetention = TimeSpan.FromHours(24);
    private readonly ZLinkInstanceSpotOperationGate operationGate = new();
    private readonly ZLinkInstanceSpotMonitoring monitoring = new();

    public ValueTask<InstanceSpotActivationTerminal> ActivateAsync(
        InstanceSpotActivationOperation operation,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        CancellationToken cancellationToken
    )
    {
        var operationKey =
            $"{operation.Target.TargetSpotId}\0"
            + $"{operation.OperationId.High:x16}{operation.OperationId.Low:x16}";
        return new ValueTask<InstanceSpotActivationTerminal>(
            monitoring.ObserveAsync(
                operationKey,
                operation.Target.MeshName,
                operation.Target.StableType,
                PendingBytes(metadata, payload),
                () =>
                    operationGate.RunAsync(
                        operationKey,
                        () => ActivateCoreAsync(operation, metadata, payload, cancellationToken)
                    )
            )
        );
    }

    internal ZLinkInstanceSpotOperationSnapshot MonitoringSnapshot(string stableType) =>
        monitoring.Snapshot(stableType);

    private static ulong PendingBytes(
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload
    )
    {
        var bytes = checked((ulong)(metadata?.Length ?? 0));
        foreach (var part in payload)
            bytes = checked(bytes + (ulong)part.Length);
        return bytes;
    }

    private async Task<InstanceSpotActivationTerminal> ActivateCoreAsync(
        InstanceSpotActivationOperation operation,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        CancellationToken cancellationToken
    )
    {
        ValidateTarget(operation);
        if (!registration.InstanceSpotFactories.ContainsKey(operation.Target.StableType))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TypeMismatch,
                $"Instance Spot type '{operation.Target.StableType}' is not registered."
            );

        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(operation.Target.TargetSpotId);
        var requestSource = await ResolveRequestSourceAsync(
                operation.Target.MeshName,
                operation.SourceNodeRid,
                operation.SourceNodeGeneration,
                cancellationToken
            )
            .ConfigureAwait(false);

        var envelope = ZLinkInstanceSpotActivationEnvelopeCodec.Encode(
            operation,
            requestSource,
            metadata,
            payload
        );
        var envelopeHash = SHA256.HashData(envelope);
        var stored = await relocationStore
            .PutRelocationAsync(envelope, RecoveryRetention, cancellationToken)
            .ConfigureAwait(false);
        var creatingPayload = AuthorityPayload(
            operation,
            ZLinkInstanceSpotAuthorityState.Creating,
            null
        );
        ZLinkObjectReservation? reservation = null;
        PreparedReservedSpot? prepared = null;
        var committed = false;
        var published = false;
        try
        {
            var reserve = await authorityStore
                .ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.InstanceSpot,
                        key,
                        operation.Target.StableType,
                        stored.Reference,
                        envelopeHash,
                        envelope.Length,
                        new ZLinkMeshNodeDescriptorKey(
                            operation.Target.MeshName,
                            operation.Target.TargetNodeRid
                        ),
                        operation.Target.TargetNodeGeneration,
                        owner,
                        ZLinkInstanceSpotAuthorityPayloadCodec.Encode(creatingPayload),
                        new ZLinkCapacityVector(
                            0,
                            1,
                            new ZLinkSpotTypeCapacityDelta(
                                ZLinkPlacementObjectKind.InstanceSpot,
                                operation.Target.StableType,
                                1
                            )
                        )
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (reserve is ZLinkObjectReserveResult.TypeMismatch)
            {
                ZLinkRuntimeMetrics.RecordInstanceSpotClaimConflict(
                    operation.Target.MeshName,
                    operation.Target.StableType,
                    "spot_type"
                );
                throw ReserveFailure(operation, reserve);
            }
            if (reserve is not ZLinkObjectReserveResult.Reserved reserved)
                return await JoinExistingAsync(
                        operation,
                        metadata,
                        payload,
                        reserve,
                        stored,
                        envelopeHash,
                        envelope.Length,
                        requestSource,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            reservation = reserved.Reservation;
            prepared = await catalog
                .PrepareInstanceReservedAsync(
                    operation.Target.StableType,
                    operation.Target.TargetSpotId,
                    reservation.ObjectGeneration,
                    reservation.AuthorityOwnerGeneration,
                    cancellationToken
                )
                .ConfigureAwait(false);
            var readyPayload = AuthorityPayload(
                operation,
                ZLinkInstanceSpotAuthorityState.Ready,
                new ZLinkInstanceSpotActivationRecoveryPointer(
                    stored.Reference,
                    envelopeHash,
                    checked((uint)envelope.Length),
                    1,
                    0
                )
            );
            var commit = await authorityStore
                .CommitAsync(
                    reservation,
                    ZLinkInstanceSpotAuthorityPayloadCodec.Encode(readyPayload),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (
                commit
                is not (
                    ZLinkObjectCommitResult.Committed
                    or ZLinkObjectCommitResult.AlreadyCommitted
                )
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Instance Spot '{operation.Target.TargetSpotId}' Ready commit conflicted.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );
            var readySnapshot = commit switch
            {
                ZLinkObjectCommitResult.Committed value => value.Snapshot,
                ZLinkObjectCommitResult.AlreadyCommitted value => value.Snapshot,
                _ => throw new InvalidOperationException(),
            };
            committed = true;
            await catalog
                .PublishInstanceReservedAsync(
                    prepared,
                    readySnapshot.ObjectGeneration,
                    readySnapshot.AuthorityOwnerGeneration,
                    cancellationToken
                )
                .ConfigureAwait(false);
            published = true;
            var terminal = await DispatchFirstMessageAsync(
                    prepared.Activation,
                    operation,
                    requestSource,
                    readySnapshot,
                    metadata,
                    payload,
                    cancellationToken
                )
                .ConfigureAwait(false);
            await CompleteAndClearRecoveryAsync(key, readySnapshot, readyPayload, stored.Reference)
                .ConfigureAwait(false);
            return terminal;
        }
        catch
        {
            if (!published && prepared is not null)
                await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
            if (!committed && reservation is not null)
                await authorityStore
                    .AbortAsync(reservation, CancellationToken.None)
                    .ConfigureAwait(false);
            if (!committed)
                await relocationStore
                    .DeleteRelocationAsync(stored.Reference, CancellationToken.None)
                    .ConfigureAwait(false);
            throw;
        }
    }

    internal async ValueTask RecoverAsync(CancellationToken cancellationToken)
    {
        ZLinkAuthorityScanCursor? cursor = null;
        do
        {
            var scan = await authorityStore
                .ListAuthoritiesAsync("zla1:s:", cursor, 128, cancellationToken)
                .ConfigureAwait(false);
            if (scan is ZLinkAuthorityScanResult.ScanExpired)
            {
                cursor = null;
                continue;
            }

            var page = ((ZLinkAuthorityScanResult.Page)scan).Value;
            foreach (var entry in page.Items)
                await RecoverEntryAsync(entry, cancellationToken).ConfigureAwait(false);
            cursor = page.NextCursor;
        } while (cursor is not null);
    }

    private async ValueTask RecoverEntryAsync(
        ZLinkAuthorityEntry entry,
        CancellationToken cancellationToken
    )
    {
        var snapshot = entry.Snapshot;
        var status = node.MeshStatus();
        if (
            snapshot.Allocation.ObjectKind != ZLinkPlacementObjectKind.InstanceSpot
            || !ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var authority
            )
            || authority.NodeRid != node.RoutingId
            || authority.NodeGeneration != status.LifecycleGeneration
        )
            return;

        ZLinkInstanceSpotActivationRecoveryPointer recovery;
        if (authority.State == ZLinkInstanceSpotAuthorityState.Creating)
        {
            if (snapshot.ReservedCreation is not { } pending)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    $"Instance Spot '{authority.SpotId}' reservation is incomplete."
                );
            recovery = new ZLinkInstanceSpotActivationRecoveryPointer(
                pending.RequestContentReference,
                pending.RequestSha256.ToArray(),
                checked((uint)pending.RequestEncodedSize),
                1,
                0
            );
        }
        else if (authority.ActivationRecovery is { } activationRecovery)
        {
            recovery = activationRecovery;
        }
        else
        {
            return;
        }

        if (recovery.ReplayCursor == recovery.InboxSequence)
        {
            await ClearRecoveryAsync(entry.Key, snapshot, authority, recovery.Reference)
                .ConfigureAwait(false);
            return;
        }
        var root = await relocationStore
            .GetRelocationAsync(recovery.Reference, cancellationToken)
            .ConfigureAwait(false);
        if (
            root is not ZLinkRelocationReadResult.Found found
            || found.Payload.Length != recovery.EncodedSize
            || !SHA256.HashData(found.Payload.Span).AsSpan().SequenceEqual(recovery.Sha256)
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Instance Spot '{authority.SpotId}' activation recovery payload is unavailable."
            );
        if (
            recovery.ReplayCursor >= recovery.InboxSequence
            || !ZLinkInstanceSpotActivationEnvelopeCodec.TryDecodeActivation(
                found.Payload.Span,
                out var record
            )
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                $"Instance Spot '{authority.SpotId}' activation recovery payload is invalid."
            );

        var activation = await catalog
            .TryGetInstanceActivationAsync(
                authority.SpotId,
                authority.StableType,
                snapshot.ObjectGeneration
            )
            .ConfigureAwait(false);
        if (activation is null)
        {
            var prepared = await catalog
                .PrepareInstanceReservedAsync(
                    authority.StableType,
                    authority.SpotId,
                    snapshot.ObjectGeneration,
                    snapshot.AuthorityOwnerGeneration,
                    cancellationToken
                )
                .ConfigureAwait(false);
            // Recovery re-materializes the Instance Spot under the activation admission the
            // prepared Spot holds; a failure before publication completes discards it.
            try
            {
                if (authority.State == ZLinkInstanceSpotAuthorityState.Creating)
                {
                    var pending =
                        snapshot.ReservedCreation
                        ?? throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.ProtocolError,
                            $"Instance Spot '{authority.SpotId}' reservation is incomplete."
                        );
                    var readyAuthority = authority with
                    {
                        State = ZLinkInstanceSpotAuthorityState.Ready,
                        ActivationRecovery = recovery,
                    };
                    var reservation = new ZLinkObjectReservation(
                        entry.Key,
                        snapshot.StoreVersion,
                        snapshot.ObjectGeneration,
                        snapshot.AuthorityOwnerGeneration,
                        pending.ReservationId,
                        snapshot.Allocation.Descriptor,
                        snapshot.Allocation.DescriptorLifecycleGeneration,
                        new ZLinkLocationOwnerToken(snapshot.OwnerId, snapshot.OwnerLeaseGeneration)
                    );
                    var committed = await authorityStore
                        .CommitAsync(
                            reservation,
                            ZLinkInstanceSpotAuthorityPayloadCodec.Encode(readyAuthority),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                    snapshot = committed switch
                    {
                        ZLinkObjectCommitResult.Committed value => value.Snapshot,
                        ZLinkObjectCommitResult.AlreadyCommitted value => value.Snapshot,
                        _ => throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            $"Instance Spot '{authority.SpotId}' recovery lost its reservation.",
                            ZLinkRetryAdvice.RetryAfterBackoff
                        ),
                    };
                    authority = readyAuthority;
                }
                else if (authority.State != ZLinkInstanceSpotAuthorityState.Ready)
                {
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.ProtocolError,
                        $"Instance Spot '{authority.SpotId}' authority state is invalid."
                    );
                }
                await catalog
                    .PublishInstanceReservedAsync(
                        prepared,
                        snapshot.ObjectGeneration,
                        snapshot.AuthorityOwnerGeneration,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
            }
            catch
            {
                await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
                throw;
            }
            activation = prepared.Activation;
        }

        var operation = new InstanceSpotActivationOperation(
            new InstanceSpotActivationTarget(
                authority.MeshName,
                authority.NodeRid,
                authority.NodeGeneration,
                authority.SpotId,
                authority.StableType,
                snapshot.StoreVersion
            ),
            record.SourceNodeRid,
            record.SourceNodeGeneration,
            record.SourceSpotId,
            record.OperationId,
            record.IsRequest,
            0,
            record.DeadlineUnixMs
        );
        await DispatchFirstMessageAsync(
                activation,
                operation,
                record.RequestSource,
                snapshot,
                record.Metadata,
                record.Payload,
                cancellationToken
            )
            .ConfigureAwait(false);
        await CompleteAndClearRecoveryAsync(entry.Key, snapshot, authority, recovery.Reference)
            .ConfigureAwait(false);
    }

    private async ValueTask<InstanceSpotActivationTerminal> JoinExistingAsync(
        InstanceSpotActivationOperation operation,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        ZLinkObjectReserveResult reserve,
        ZLinkRelocationStored operationRoot,
        byte[] operationSha256,
        int operationEncodedSize,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        CancellationToken cancellationToken
    )
    {
        var deadline =
            Stopwatch.GetElapsedTime(0)
            + (
                DateTimeOffset.FromUnixTimeMilliseconds(checked((long)operation.DeadlineUnixMs))
                - DateTimeOffset.UtcNow
            );
        var current = reserve switch
        {
            ZLinkObjectReserveResult.AlreadyExists value => value.Current,
            ZLinkObjectReserveResult.Conflict { Current: ZLinkAuthorityReadResult.Found value } =>
                value.Snapshot,
            _ => throw ReserveFailure(operation, reserve),
        };
        while (true)
        {
            if (current.Allocation.ObjectKind != ZLinkPlacementObjectKind.InstanceSpot)
            {
                ZLinkRuntimeMetrics.RecordInstanceSpotClaimConflict(
                    operation.Target.MeshName,
                    operation.Target.StableType,
                    "spot_kind"
                );
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    $"Instance Spot '{operation.Target.TargetSpotId}' has another stable type."
                );
            }
            if (
                !string.Equals(
                    current.Allocation.StableType,
                    operation.Target.StableType,
                    StringComparison.Ordinal
                )
            )
            {
                ZLinkRuntimeMetrics.RecordInstanceSpotClaimConflict(
                    operation.Target.MeshName,
                    operation.Target.StableType,
                    "spot_type"
                );
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    $"Instance Spot '{operation.Target.TargetSpotId}' has another stable type."
                );
            }
            if (
                !ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    current.Payload.Span,
                    out var authority
                )
            )
            {
                ZLinkRuntimeMetrics.RecordInstanceSpotClaimConflict(
                    operation.Target.MeshName,
                    operation.Target.StableType,
                    "authority"
                );
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Instance Spot '{operation.Target.TargetSpotId}' activation moved to another owner.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );
            }
            if (
                authority.NodeRid != node.RoutingId
                || authority.NodeGeneration != node.MeshStatus().LifecycleGeneration
            )
            {
                var forwarded = operation with
                {
                    Target = new InstanceSpotActivationTarget(
                        authority.MeshName,
                        authority.NodeRid,
                        authority.NodeGeneration,
                        authority.SpotId,
                        authority.StableType,
                        current.StoreVersion
                    ),
                };
                while (true)
                {
                    try
                    {
                        var terminal = await node.ForwardInstanceSpotActivationAsync(
                                forwarded,
                                payload,
                                metadata,
                                cancellationToken
                            )
                            .ConfigureAwait(false);
                        await relocationStore
                            .DeleteRelocationAsync(operationRoot.Reference, CancellationToken.None)
                            .ConfigureAwait(false);
                        return terminal;
                    }
                    catch (ZlinkSubmitException exception)
                        when (exception.Result
                                is ZlinkSubmitException.ErrorCode.Backpressured
                                    or ZlinkSubmitException.ErrorCode.NotConnected
                        )
                    {
                        // The accepted operation remains durable while the
                        // winner route is temporarily unavailable.
                    }

                    var forwardRemaining = (
                        deadline - Stopwatch.GetElapsedTime(0)
                    ).TotalMilliseconds;
                    if (forwardRemaining <= 0)
                        throw new TimeoutException(
                            $"Instance Spot '{operation.Target.TargetSpotId}' activation forwarding deadline elapsed."
                        );
                    await Task.Delay(
                            TimeSpan.FromMilliseconds(Math.Min(2, forwardRemaining)),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                }
            }
            var anotherOperationIsAccepted =
                authority.ActivationRecovery is { } acceptedRecovery
                && acceptedRecovery.ReplayCursor < acceptedRecovery.InboxSequence
                && !string.Equals(
                    acceptedRecovery.Reference,
                    operationRoot.Reference,
                    StringComparison.Ordinal
                );
            if (
                authority.State == ZLinkInstanceSpotAuthorityState.Ready
                && !anotherOperationIsAccepted
                && (
                    await catalog
                        .TryGetInstanceActivationAsync(
                            authority.SpotId,
                            authority.StableType,
                            current.ObjectGeneration
                        )
                        .ConfigureAwait(false)
                )
                    is { } activation
            )
            {
                var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
                    operation.Target.TargetSpotId
                );
                var claimedAuthority = authority with
                {
                    ActivationRecovery = new ZLinkInstanceSpotActivationRecoveryPointer(
                        operationRoot.Reference,
                        operationSha256,
                        checked((uint)operationEncodedSize),
                        1,
                        0
                    ),
                };
                var claimed = await authorityStore
                    .CompareExchangeAuthorityAsync(
                        key,
                        current.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            ZLinkInstanceSpotAuthorityPayloadCodec.Encode(claimedAuthority),
                            ZLinkAuthorityGenerationTransition.Preserve,
                            null,
                            null
                        ),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (claimed is not ZLinkAuthorityCompareExchangeResult.Stored stored)
                {
                    if (
                        claimed is ZLinkAuthorityCompareExchangeResult.Conflict
                        {
                            Current: ZLinkAuthorityReadResult.Found conflict
                        }
                    )
                    {
                        current = conflict.Snapshot;
                        continue;
                    }
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"Instance Spot '{operation.Target.TargetSpotId}' operation claim conflicted.",
                        ZLinkRetryAdvice.RetryAfterBackoff
                    );
                }
                if (
                    authority.ActivationRecovery is { } previousRecovery
                    && !string.Equals(
                        previousRecovery.Reference,
                        operationRoot.Reference,
                        StringComparison.Ordinal
                    )
                )
                    await relocationStore
                        .DeleteRelocationAsync(previousRecovery.Reference, CancellationToken.None)
                        .ConfigureAwait(false);
                var terminal = await DispatchFirstMessageAsync(
                        activation,
                        operation,
                        requestSource,
                        stored.Snapshot,
                        metadata,
                        payload,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                await CompleteAndClearRecoveryAsync(
                        key,
                        stored.Snapshot,
                        claimedAuthority,
                        operationRoot.Reference
                    )
                    .ConfigureAwait(false);
                return terminal;
            }

            var remaining = (deadline - Stopwatch.GetElapsedTime(0)).TotalMilliseconds;
            if (remaining <= 0)
                throw new TimeoutException(
                    $"Instance Spot '{operation.Target.TargetSpotId}' activation deadline elapsed."
                );
            await Task.Delay(TimeSpan.FromMilliseconds(Math.Min(10, remaining)), cancellationToken)
                .ConfigureAwait(false);
            var read = await authorityStore
                .ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(operation.Target.TargetSpotId),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Instance Spot '{operation.Target.TargetSpotId}' activation authority disappeared.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );
            current = found.Snapshot;
        }
    }

    private async ValueTask<InstanceSpotActivationTerminal> DispatchFirstMessageAsync(
        ZLinkSpotActivation activation,
        InstanceSpotActivationOperation operation,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        ZLinkAuthoritySnapshot authority,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        CancellationToken cancellationToken
    )
    {
        return await activation
            .DispatchDurableActivationAsync(
                operation.OperationId,
                operation.SourceNodeRid,
                operation.SourceSpotId,
                requestSource,
                operation.Target.TargetNodeGeneration,
                authority.AuthorityOwnerGeneration,
                checked((ulong)authority.OwnerLeaseGeneration),
                payload,
                metadata,
                operation.IsRequest,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkServiceWireCodec.RequestSourceFence> ResolveRequestSourceAsync(
        string meshName,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        CancellationToken cancellationToken
    )
    {
        var descriptors = await authorityStore
            .ListAllMeshNodesAsync(meshName, cancellationToken)
            .ConfigureAwait(false);
        var descriptor = descriptors.SingleOrDefault(value =>
            value.Rid == sourceNodeRid && value.LifecycleGeneration == sourceNodeGeneration
        );
        if (descriptor is null || descriptor.LeaseGeneration <= 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "The Instance Spot activation request-source fence is unavailable."
            );
        return new ZLinkServiceWireCodec.RequestSourceFence(
            descriptor.OwnerId,
            checked((ulong)descriptor.LeaseGeneration),
            sourceNodeRid,
            sourceNodeGeneration
        );
    }

    private async ValueTask CompleteAndClearRecoveryAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot readySnapshot,
        ZLinkInstanceSpotAuthorityPayload readyPayload,
        string recoveryReference
    )
    {
        var recovery =
            readyPayload.ActivationRecovery
            ?? throw new InvalidOperationException(
                "The Ready Instance Spot authority has no activation recovery pointer."
            );
        var advancedPayload = readyPayload with
        {
            ActivationRecovery = recovery with { ReplayCursor = recovery.InboxSequence },
        };
        var advanced = await authorityStore
            .CompareExchangeAuthorityAsync(
                key,
                readySnapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    ZLinkInstanceSpotAuthorityPayloadCodec.Encode(advancedPayload),
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                ),
                CancellationToken.None
            )
            .ConfigureAwait(false);
        if (advanced is not ZLinkAuthorityCompareExchangeResult.Stored stored)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Instance Spot '{readyPayload.SpotId}' activation completion conflicted.",
                ZLinkRetryAdvice.RetryAfterBackoff
            );
        await ClearRecoveryAsync(key, stored.Snapshot, advancedPayload, recoveryReference)
            .ConfigureAwait(false);
    }

    private async ValueTask ClearRecoveryAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot completedSnapshot,
        ZLinkInstanceSpotAuthorityPayload completedPayload,
        string recoveryReference
    )
    {
        var cleared = await authorityStore
            .CompareExchangeAuthorityAsync(
                key,
                completedSnapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                        completedPayload with
                        {
                            ActivationRecovery = null,
                        }
                    ),
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                ),
                CancellationToken.None
            )
            .ConfigureAwait(false);
        if (cleared is not ZLinkAuthorityCompareExchangeResult.Stored)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Instance Spot '{completedPayload.SpotId}' activation recovery release conflicted.",
                ZLinkRetryAdvice.RetryAfterBackoff
            );
        await relocationStore
            .DeleteRelocationAsync(recoveryReference, CancellationToken.None)
            .ConfigureAwait(false);
    }

    private ZLinkInstanceSpotAuthorityPayload AuthorityPayload(
        InstanceSpotActivationOperation operation,
        ZLinkInstanceSpotAuthorityState state,
        ZLinkInstanceSpotActivationRecoveryPointer? activationRecovery
    ) =>
        new(
            state,
            operation.Target.TargetSpotId,
            operation.Target.StableType,
            operation.Target.MeshName,
            node.RoutingId,
            node.MeshStatus().LifecycleGeneration,
            owner.OwnerId,
            checked((ulong)owner.LeaseGeneration),
            activationRecovery
        );

    private void ValidateTarget(InstanceSpotActivationOperation operation)
    {
        var status = node.MeshStatus();
        if (
            operation.Target.TargetNodeRid != node.RoutingId
            || operation.Target.TargetNodeGeneration != status.LifecycleGeneration
            || !string.Equals(
                operation.Target.MeshName,
                registration.SpotNodeName,
                StringComparison.Ordinal
            )
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "The Instance Spot activation target descriptor is stale."
            );
    }

    private static Exception ReserveFailure(
        InstanceSpotActivationOperation operation,
        ZLinkObjectReserveResult result
    ) =>
        result switch
        {
            ZLinkObjectReserveResult.TypeMismatch => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TypeMismatch,
                $"Instance Spot '{operation.Target.TargetSpotId}' has another stable type."
            ),
            ZLinkObjectReserveResult.PlacementCapacityExhausted => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "The Instance Spot target has no remaining capacity.",
                ZLinkRetryAdvice.RetryAfterBackoff
            ),
            _ => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Instance Spot '{operation.Target.TargetSpotId}' activation conflicted.",
                ZLinkRetryAdvice.RetryAfterBackoff
            ),
        };
}
