using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkInstanceSpotAuthorityState : byte
{
    Creating = 1,
    Ready = 2,
    Closing = 3,
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
                ZLinkInstanceSpotAuthorityState.Creating => (byte)
                    ServiceWireCodec.InstanceAuthorityState.ColdActivating,
                ZLinkInstanceSpotAuthorityState.Ready => (byte)
                    ServiceWireCodec.InstanceAuthorityState.Ready,
                ZLinkInstanceSpotAuthorityState.Closing => (byte)
                    ServiceWireCodec.InstanceAuthorityState.Closing,
                _ => throw new ArgumentOutOfRangeException(nameof(payload)),
            }
        );
        spot.U16(checked((ushort)instance.Count));
        spot.Bytes(instance.WrittenSpan);
        var identity = new Writer();
        identity.U8((byte)ServiceWireCodec.SpotKind.Instance);
        identity.U16(checked((ushort)spot.Count));
        identity.Bytes(spot.WrittenSpan);

        var body = new Writer();
        body.U8(
            payload.State == ZLinkInstanceSpotAuthorityState.Creating
                ? (byte)ServiceWireCodec.AuthorityOperationKind.ColdActivation
                : (byte)ServiceWireCodec.AuthorityOperationKind.Steady
        );
        body.U8((byte)ServiceWireCodec.AuthorityObjectKind.Spot);
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
                || recovery.EncodedSize
                    > ZLinkCanonicalRelocationAuthorityStateCodec.MaximumPayloadBytes
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
        result.Bytes(ZLinkCanonicalRelocationAuthorityStateCodec.AuthorityMagic);
        result.U8(ZLinkCanonicalRelocationAuthorityStateCodec.AuthorityVersion);
        result.U16(0);
        result.U32(checked((uint)body.Count));
        result.Bytes(body.WrittenSpan);
        result.U32(Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(result.WrittenSpan));
        if (result.Count > ZLinkCanonicalRelocationAuthorityStateCodec.MaximumPayloadBytes)
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
            if (
                encoded.Length < ZLinkCanonicalRelocationAuthorityStateCodec.MinimumEnvelopeBytes
                || encoded.Length > ZLinkCanonicalRelocationAuthorityStateCodec.MaximumPayloadBytes
                || !encoded[..ZLinkCanonicalRelocationAuthorityStateCodec.MagicBytes]
                    .SequenceEqual(ZLinkCanonicalRelocationAuthorityStateCodec.AuthorityMagic)
            )
                return false;
            var reader = new Reader(encoded);
            reader.Skip(ZLinkCanonicalRelocationAuthorityStateCodec.MagicBytes);
            if (
                reader.U8() != ZLinkCanonicalRelocationAuthorityStateCodec.AuthorityVersion
                || reader.U16() != 0
            )
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
            if (body.U8() != (byte)ServiceWireCodec.AuthorityObjectKind.Spot)
                return false;
            var identity = body.Slice(body.U16());
            if (identity.U8() != (byte)ServiceWireCodec.SpotKind.Instance)
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
                (byte)ServiceWireCodec.InstanceAuthorityState.ColdActivating
                    when operation
                        == (byte)ServiceWireCodec.AuthorityOperationKind.ColdActivation =>
                    ZLinkInstanceSpotAuthorityState.Creating,
                (byte)ServiceWireCodec.InstanceAuthorityState.Ready
                    when operation == (byte)ServiceWireCodec.AuthorityOperationKind.Steady =>
                    ZLinkInstanceSpotAuthorityState.Ready,
                (byte)ServiceWireCodec.InstanceAuthorityState.Closing
                    when operation == (byte)ServiceWireCodec.AuthorityOperationKind.Steady =>
                    ZLinkInstanceSpotAuthorityState.Closing,
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
                if (
                    encodedSize > ZLinkCanonicalRelocationAuthorityStateCodec.MaximumPayloadBytes
                    || replayCursor > inboxSequence
                )
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

internal sealed class ZLinkInstanceSpotOperationGate<T>
{
    private readonly ConcurrentDictionary<string, Lazy<Task<T>>> pending = new(
        StringComparer.Ordinal
    );

    internal async Task<T> RunAsync(
        string operationKey,
        Func<Task<T>> operation,
        Func<T, Task<T>>? join = null
    )
    {
        var candidate = new Lazy<Task<T>>(operation, LazyThreadSafetyMode.ExecutionAndPublication);
        var selected = pending.GetOrAdd(operationKey, candidate);
        if (join is not null && !ReferenceEquals(selected, candidate))
        {
            while (true)
            {
                var previous = selected;
                var next = new Lazy<Task<T>>(
                    async () =>
                        await join(await previous.Value.ConfigureAwait(false))
                            .ConfigureAwait(false),
                    LazyThreadSafetyMode.ExecutionAndPublication
                );
                // Only a participant appends admission work; the creator owns Reserve.
                if (pending.TryUpdate(operationKey, next, previous))
                {
                    selected = next;
                    break;
                }
                selected = pending.GetOrAdd(operationKey, candidate);
                if (ReferenceEquals(selected, candidate))
                    break;
            }
        }
        try
        {
            return await selected.Value.ConfigureAwait(false);
        }
        finally
        {
            pending.TryRemove(new KeyValuePair<string, Lazy<Task<T>>>(operationKey, selected));
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
    private const int RecoveryScanPageSize = 128;
    private static readonly TimeSpan RecoveryRetention = TimeSpan.FromHours(24);
    private readonly ZLinkInstanceSpotOperationGate<
        Task<InstanceSpotActivationTerminal>
    > operationGate = new();
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
                () => DispatchAsync()
            )
        );

        async Task<InstanceSpotActivationTerminal> DispatchAsync()
        {
            var terminal = await operationGate
                .RunAsync(
                    operation.Target.TargetSpotId,
                    () => ActivateCoreAsync(operation, metadata, payload, cancellationToken),
                    _ => JoinAsync()
                )
                .ConfigureAwait(false);
            return await terminal.ConfigureAwait(false);
        }

        async Task<Task<InstanceSpotActivationTerminal>> JoinAsync()
        {
            try
            {
                return await ActivateCoreAsync(operation, metadata, payload, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception error)
                when (error is ZLinkFrameworkException or OperationCanceledException)
            {
                // A participant's refusal belongs to that operation, not to the activation.
                return Task.FromException<InstanceSpotActivationTerminal>(error);
            }
        }
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

    private async Task<Task<InstanceSpotActivationTerminal>> ActivateCoreAsync(
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

        var read = await authorityStore
            .ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (
            read is ZLinkAuthorityReadResult.Found found
            && found.Snapshot.Allocation.State == ZLinkPlacementAllocationState.Active
            && ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var existing
            )
            && existing.NodeRid == node.RoutingId
            && existing.NodeGeneration == node.MeshStatus().LifecycleGeneration
        )
        {
            if (existing.StableType != operation.Target.StableType)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    "Instance Spot type does not match."
                );
            var activation = await catalog
                .TryGetInstanceActivationAsync(
                    existing.SpotId,
                    existing.StableType,
                    found.Snapshot.ObjectGeneration
                )
                .ConfigureAwait(false);
            if (activation is not null)
                return await AdmitMessageAsync(
                        activation,
                        operation,
                        requestSource,
                        found.Snapshot,
                        metadata,
                        payload,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
        }

        var envelope = ZLinkServiceWireCodec.EncodeInstanceSpotActivationRecovery(
            operation,
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
                        ),
                        registration.InstanceSpotRelocations.TryGetValue(
                            operation.Target.StableType,
                            out var policy
                        )
                            ? policy.PolicyKind
                            : null
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
                throw ReserveFailure(operation, reserve);
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
                    DateTimeOffset.FromUnixTimeMilliseconds(
                        checked((long)operation.DeadlineUnixMs)
                    ),
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
            return await AdmitMessageAsync(
                    prepared.Activation,
                    operation,
                    requestSource,
                    readySnapshot,
                    metadata,
                    payload,
                    cancellationToken,
                    _ =>
                        CompleteAndClearRecoveryAsync(
                            key,
                            readySnapshot,
                            readyPayload,
                            stored.Reference
                        )
                )
                .ConfigureAwait(false);
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
                .ListAuthoritiesAsync(
                    ZLinkAuthorityKeyCodec.Prefix(ZLinkAuthorityKeyKind.Spot),
                    cursor,
                    RecoveryScanPageSize,
                    cancellationToken
                )
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
        )
            return;

        if (
            authority.NodeRid != node.RoutingId
            || authority.NodeGeneration != status.LifecycleGeneration
        )
        {
            if (
                authority.State == ZLinkInstanceSpotAuthorityState.Creating
                && snapshot.ReservedCreation is { } pending
                && await authorityStore
                    .ReleaseEndedReservationAsync(
                        entry.Key,
                        snapshot.StoreVersion,
                        cancellationToken
                    )
                    .ConfigureAwait(false)
            )
                await relocationStore
                    .DeleteRelocationAsync(pending.RequestContentReference, cancellationToken)
                    .ConfigureAwait(false);
            return;
        }

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
        var context = new ServiceWireCodec.DecodeContext(
            null,
            null,
            null,
            found.Payload.Length,
            found.Payload.Length
        );
        ServiceWireCodec.InstanceActivationRecoveryV1 record;
        try
        {
            record = ServiceWireCodec.DecodeDurableInstanceActivationRecoveryV1(
                found.Payload.ToArray(),
                context
            );
        }
        catch (Exception error) when (error is IOException or ArgumentException)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                $"Instance Spot '{authority.SpotId}' activation recovery payload is invalid.",
                innerException: error
            );
        }
        if (
            record.TargetSpotId.Value != authority.SpotId
            || record.StableType.Value != authority.StableType
            || record.TargetMeshName.Value != authority.MeshName
            || RoutingId.From(record.TargetNodeRid.Value) != authority.NodeRid
            || record.TargetNodeGeneration.Value != authority.NodeGeneration
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "The activation recovery root does not match its authority."
            );
        var sourceNodeRid = RoutingId.From(record.SourceNodeRid.Value);
        var requestSource = await ResolveRequestSourceAsync(
                authority.MeshName,
                sourceNodeRid,
                record.SourceNodeGeneration.Value,
                cancellationToken
            )
            .ConfigureAwait(false);
        var application = ServiceWireCodec.EncodeApplicationPayloadEnvelopeV1(
            record.ApplicationPayload,
            context
        );
        if (
            !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                application,
                out var parts
            )
        )
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "The activation recovery application payload is invalid."
            );
        var payload = parts.Select(static part => (ReadOnlyMemory<byte>)part.ToArray()).ToArray();
        ZLinkMessageParts.DisposeAll(parts);
        ReadOnlyMemory<byte>? metadata = record.Metadata is null
            ? null
            : ServiceWireCodec.EncodeMetadataFrame(
                record.Metadata,
                ServiceWireCodec.DecodeContext.Empty
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
                record.TargetMeshName.Value,
                RoutingId.From(record.TargetNodeRid.Value),
                record.TargetNodeGeneration.Value,
                record.TargetSpotId.Value,
                record.StableType.Value,
                record.TargetDescriptorVersion.Value
            ),
            sourceNodeRid,
            record.SourceNodeGeneration.Value,
            record.SourceSpotId?.Value ?? string.Empty,
            new MeshOperationId(record.Operation.High.Value, record.Operation.Low.Value),
            record.OperationKind == ServiceWireCodec.InstanceOperationKind.Request,
            record.ReplyRoute is ServiceWireCodec.InstanceReplyRouteCase1 route
                ? route.ReplyRouteId.Value
                : 0,
            record.DeadlineUnixMs.Value
        );
        _ = await DispatchFirstMessageAsync(
                activation,
                operation,
                requestSource,
                snapshot,
                metadata,
                payload,
                cancellationToken,
                _ =>
                    CompleteAndClearRecoveryAsync(
                        entry.Key,
                        snapshot,
                        authority,
                        recovery.Reference
                    )
            )
            .ConfigureAwait(false);
    }

    private async ValueTask<InstanceSpotActivationTerminal> DispatchFirstMessageAsync(
        ZLinkSpotActivation activation,
        InstanceSpotActivationOperation operation,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        ZLinkAuthoritySnapshot authority,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        CancellationToken cancellationToken,
        Func<CancellationToken, ValueTask>? recordTerminal = null
    )
    {
        var terminal = await AdmitMessageAsync(
                activation,
                operation,
                requestSource,
                authority,
                metadata,
                payload,
                cancellationToken,
                recordTerminal
            )
            .ConfigureAwait(false);
        return await terminal.ConfigureAwait(false);
    }

    private ValueTask<Task<InstanceSpotActivationTerminal>> AdmitMessageAsync(
        ZLinkSpotActivation activation,
        InstanceSpotActivationOperation operation,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        ZLinkAuthoritySnapshot authority,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        CancellationToken cancellationToken,
        Func<CancellationToken, ValueTask>? recordTerminal = null
    ) =>
        activation.AdmitDurableActivationAsync(
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
            cancellationToken,
            operation,
            recordTerminal
        );

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
