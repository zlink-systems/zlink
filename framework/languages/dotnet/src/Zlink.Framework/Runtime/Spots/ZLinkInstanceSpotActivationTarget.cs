using System.Security.Cryptography;
using System.Text;
using System.Collections.Concurrent;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkInstanceSpotAuthorityState : byte
{
    Creating = 1,
    Ready = 2
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
    string? RecoveryReference,
    uint RecoveryChecksum,
    ulong ReplayCursor);

internal static class ZLinkInstanceSpotAuthorityPayloadCodec
{
    private static readonly byte[] Magic = "ZLIS"u8.ToArray();
    private const byte Version = 1;

    internal static byte[] Encode(ZLinkInstanceSpotAuthorityPayload payload)
    {
        ArgumentNullException.ThrowIfNull(payload);
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(Version);
        writer.Write((byte)payload.State);
        WriteText(writer, payload.SpotId);
        WriteText(writer, payload.StableType);
        WriteText(writer, payload.MeshName);
        WriteBytes(writer, payload.NodeRid.ToBytes());
        writer.Write(payload.NodeGeneration);
        WriteText(writer, payload.OwnerId);
        writer.Write(payload.OwnerLeaseGeneration);
        writer.Write(payload.RecoveryReference is not null);
        if (payload.RecoveryReference is not null)
        {
            WriteText(writer, payload.RecoveryReference);
            writer.Write(payload.RecoveryChecksum);
        }
        writer.Write(payload.ReplayCursor);
        return stream.ToArray();
    }

    internal static bool TryDecode(
        ReadOnlySpan<byte> encoded,
        out ZLinkInstanceSpotAuthorityPayload payload)
    {
        payload = null!;
        if (ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                encoded,
                out var canonical))
        {
            payload = new ZLinkInstanceSpotAuthorityPayload(
                canonical.State == ZLinkUserSpotAuthorityState.Ready
                    ? ZLinkInstanceSpotAuthorityState.Ready
                    : ZLinkInstanceSpotAuthorityState.Creating,
                canonical.SpotId,
                canonical.StableType,
                canonical.MeshName,
                canonical.NodeRid,
                canonical.NodeGeneration,
                canonical.OwnerId,
                canonical.OwnerLeaseGeneration,
                RecoveryReference: null,
                RecoveryChecksum: 0,
                ReplayCursor: 0);
            return true;
        }
        try
        {
            using var stream = new MemoryStream(encoded.ToArray(), writable: false);
            using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: false);
            if (!reader.ReadBytes(Magic.Length).AsSpan().SequenceEqual(Magic)
                || reader.ReadByte() != Version)
                return false;
            var state = (ZLinkInstanceSpotAuthorityState)reader.ReadByte();
            var spotId = ReadText(reader);
            var stableType = ReadText(reader);
            var meshName = ReadText(reader);
            var nodeRid = RoutingId.From(ReadBytes(reader));
            var nodeGeneration = reader.ReadUInt64();
            var ownerId = ReadText(reader);
            var ownerLeaseGeneration = reader.ReadUInt64();
            string? reference = null;
            uint checksum = 0;
            if (reader.ReadBoolean())
            {
                reference = ReadText(reader);
                checksum = reader.ReadUInt32();
            }
            var replayCursor = reader.ReadUInt64();
            if (stream.Position != stream.Length
                || state is not (ZLinkInstanceSpotAuthorityState.Creating
                    or ZLinkInstanceSpotAuthorityState.Ready))
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
                reference,
                checksum,
                replayCursor);
            return true;
        }
        catch (Exception error) when (error is EndOfStreamException
                                      or IOException
                                      or ArgumentException)
        {
            return false;
        }
    }

    private static void WriteText(BinaryWriter writer, string value) =>
        WriteBytes(writer, Encoding.UTF8.GetBytes(value));

    private static string ReadText(BinaryReader reader) =>
        Encoding.UTF8.GetString(ReadBytes(reader));

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
        if (value.Length != length) throw new EndOfStreamException();
        return value;
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
        IReadOnlyList<ReadOnlyMemory<byte>> Payload);

    internal static byte[] Encode(
        InstanceSpotActivationOperation operation,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload)
    {
        if (requestSource.NodeRid != operation.SourceNodeRid
            || requestSource.NodeGeneration != operation.SourceNodeGeneration
            || string.IsNullOrWhiteSpace(requestSource.OwnerId)
            || requestSource.LeaseGeneration == 0)
            throw new ArgumentException(
                "The Instance Spot activation source fence is invalid.",
                nameof(requestSource));
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
        if (metadata.HasValue) WriteBytes(writer, metadata.Value.Span);
        writer.Write(payload.Count);
        foreach (var part in payload) WriteBytes(writer, part.Span);
        return stream.ToArray();
    }

    internal static byte[] EncodeTerminal(
        ReadOnlyMemory<byte> activationEnvelope,
        InstanceSpotActivationTerminal terminal)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(Version);
        writer.Write((byte)2);
        WriteBytes(writer, activationEnvelope.Span);
        writer.Write((int)terminal.Result);
        writer.Write((uint)terminal.FailureCode);
        writer.Write(terminal.ReplyParts.Count);
        foreach (var part in terminal.ReplyParts) WriteBytes(writer, part.Span);
        return stream.ToArray();
    }

    internal static bool TryDecodeTerminal(
        ReadOnlySpan<byte> encoded,
        out ActivationRecord activation,
        out InstanceSpotActivationTerminal terminal)
    {
        activation = null!;
        terminal = null!;
        try
        {
            using var stream = new MemoryStream(encoded.ToArray(), writable: false);
            using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: false);
            if (!reader.ReadBytes(Magic.Length).AsSpan().SequenceEqual(Magic)
                || reader.ReadByte() != Version
                || reader.ReadByte() != 2)
                return false;
            var original = ReadBytes(reader);
            if (!TryDecodeActivation(original, out activation))
                return false;
            var result = (RequestResult)reader.ReadInt32();
            var failure = (ServiceWireConstants.FrameworkErrorCode)reader.ReadUInt32();
            var count = reader.ReadInt32();
            if (count is < 0 or > 1024) return false;
            var reply = new ReadOnlyMemory<byte>[count];
            for (var index = 0; index < count; index++)
                reply[index] = ReadBytes(reader);
            if (stream.Position != stream.Length) return false;
            terminal = new InstanceSpotActivationTerminal(result, failure, reply);
            return true;
        }
        catch (Exception error) when (error is EndOfStreamException
                                      or IOException
                                      or ArgumentException)
        {
            return false;
        }
    }

    internal static bool TryDecodeActivation(
        ReadOnlySpan<byte> encoded,
        out ActivationRecord record)
    {
        record = null!;
        try
        {
            using var stream = new MemoryStream(encoded.ToArray(), writable: false);
            using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: false);
            if (!reader.ReadBytes(Magic.Length).AsSpan().SequenceEqual(Magic)
                || reader.ReadByte() != Version
                || reader.ReadByte() != 1)
                return false;
            var request = reader.ReadBoolean();
            var operationId = new MeshOperationId(
                reader.ReadUInt64(),
                reader.ReadUInt64());
            var deadline = reader.ReadUInt64();
            var sourceRid = RoutingId.From(ReadBytes(reader));
            var sourceGeneration = reader.ReadUInt64();
            var sourceOwnerId = ReadText(reader);
            var sourceOwnerLeaseGeneration = reader.ReadUInt64();
            if (sourceRid.IsEmpty || sourceGeneration == 0
                || string.IsNullOrWhiteSpace(sourceOwnerId)
                || sourceOwnerLeaseGeneration == 0)
                return false;
            var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
                sourceOwnerId,
                sourceOwnerLeaseGeneration,
                sourceRid,
                sourceGeneration);
            var sourceSpotId = ReadText(reader);
            ReadOnlyMemory<byte>? metadata = reader.ReadBoolean()
                ? ReadBytes(reader)
                : null;
            var count = reader.ReadInt32();
            if (count is < 1 or > 1024) return false;
            var payload = new ReadOnlyMemory<byte>[count];
            for (var index = 0; index < count; index++)
                payload[index] = ReadBytes(reader);
            if (stream.Position != stream.Length) return false;
            record = new ActivationRecord(
                request,
                operationId,
                deadline,
                sourceRid,
                sourceGeneration,
                requestSource,
                sourceSpotId,
                metadata,
                payload);
            return true;
        }
        catch (Exception error) when (error is EndOfStreamException
                                      or IOException
                                      or ArgumentException)
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
        if (value.Length != length) throw new EndOfStreamException();
        return value;
    }

    private static string ReadText(BinaryReader reader) =>
        Encoding.UTF8.GetString(ReadBytes(reader));
}

internal sealed class ZLinkInstanceSpotOperationGate
{
    private readonly ConcurrentDictionary<
        string,
        Lazy<Task<InstanceSpotActivationTerminal>>> pending =
        new(StringComparer.Ordinal);

    internal async Task<InstanceSpotActivationTerminal> RunAsync(
        string operationKey,
        Func<Task<InstanceSpotActivationTerminal>> operation)
    {
        var selected = pending.GetOrAdd(
            operationKey,
            _ => new Lazy<Task<InstanceSpotActivationTerminal>>(
                operation,
                LazyThreadSafetyMode.ExecutionAndPublication));
        try
        {
            return await selected.Value.ConfigureAwait(false);
        }
        finally
        {
            pending.TryRemove(
                new KeyValuePair<string, Lazy<Task<InstanceSpotActivationTerminal>>>(
                    operationKey,
                    selected));
        }
    }
}

internal sealed class ZLinkInstanceSpotActivationTarget(
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore,
    ZLinkSpotNodeCatalog catalog,
    IZLinkBackendSpotNode node,
    ZLinkSpotNodeRegistration registration,
    ZLinkLocationOwnerToken owner) : IInstanceSpotActivationTarget
{
    private static readonly TimeSpan RecoveryRetention = TimeSpan.FromHours(24);
    private readonly ZLinkInstanceSpotOperationGate operationGate = new();
    private readonly ZLinkInstanceSpotMonitoring monitoring = new();

    public ValueTask<InstanceSpotActivationTerminal> ActivateAsync(
        InstanceSpotActivationOperation operation,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        CancellationToken cancellationToken)
    {
        var operationKey = $"{operation.Target.TargetSpotId}\0"
                           + $"{operation.OperationId.High:x16}{operation.OperationId.Low:x16}";
        return new ValueTask<InstanceSpotActivationTerminal>(
            monitoring.ObserveAsync(
                operationKey,
                operation.Target.MeshName,
                operation.Target.StableType,
                PendingBytes(metadata, payload),
                () => operationGate.RunAsync(
                    operationKey,
                    () => ActivateCoreAsync(
                        operation,
                        metadata,
                        payload,
                        cancellationToken))));
    }

    internal ZLinkInstanceSpotOperationSnapshot MonitoringSnapshot(
        string stableType) => monitoring.Snapshot(stableType);

    private static ulong PendingBytes(
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload)
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
        CancellationToken cancellationToken)
    {
        ValidateTarget(operation);
        if (!registration.InstanceSpotFactories.ContainsKey(operation.Target.StableType))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TypeMismatch,
                $"Instance Spot type '{operation.Target.StableType}' is not registered.");

        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
            operation.Target.TargetSpotId);
        var replay = await TryReplayRetainedTerminalAsync(
                key,
                operation.OperationId,
                cancellationToken)
            .ConfigureAwait(false);
        if (replay is not null) return replay;

        var requestSource = await ResolveRequestSourceAsync(
                operation.Target.MeshName,
                operation.SourceNodeRid,
                operation.SourceNodeGeneration,
                cancellationToken)
            .ConfigureAwait(false);

        var envelope = ZLinkInstanceSpotActivationEnvelopeCodec.Encode(
            operation,
            requestSource,
            metadata,
            payload);
        var envelopeHash = SHA256.HashData(envelope);
        var stored = await relocationStore.PutRelocationAsync(
                envelope,
                RecoveryRetention,
                cancellationToken)
            .ConfigureAwait(false);
        var creatingPayload = AuthorityPayload(
            operation,
            ZLinkInstanceSpotAuthorityState.Creating,
            stored.Reference,
            stored.ChecksumCrc32c);
        ZLinkObjectReservation? reservation = null;
        PreparedReservedSpot? prepared = null;
        var committed = false;
        var published = false;
        try
        {
            var reserve = await authorityStore.ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.InstanceSpot,
                        key,
                        operation.Target.StableType,
                        stored.Reference,
                        envelopeHash,
                        envelope.Length,
                        new ZLinkMeshNodeDescriptorKey(
                            operation.Target.MeshName,
                            operation.Target.TargetNodeRid),
                        operation.Target.TargetNodeGeneration,
                        owner,
                        ZLinkInstanceSpotAuthorityPayloadCodec.Encode(creatingPayload),
                        new ZLinkCapacityVector(
                            0,
                            1,
                            new ZLinkSpotTypeCapacityDelta(
                                ZLinkPlacementObjectKind.InstanceSpot,
                                operation.Target.StableType,
                                1))),
                    cancellationToken)
                .ConfigureAwait(false);
            if (reserve is ZLinkObjectReserveResult.TypeMismatch)
            {
                ZLinkRuntimeMetrics.RecordInstanceSpotClaimConflict(
                    operation.Target.MeshName,
                    operation.Target.StableType,
                    "spot_type");
                throw ReserveFailure(operation, reserve);
            }
            if (reserve is not ZLinkObjectReserveResult.Reserved reserved)
                return await JoinExistingAsync(
                        operation,
                        metadata,
                        payload,
                        reserve,
                        stored,
                        requestSource,
                        cancellationToken)
                    .ConfigureAwait(false);
            reservation = reserved.Reservation;
            prepared = await catalog.PrepareInstanceReservedAsync(
                    operation.Target.StableType,
                    operation.Target.TargetSpotId,
                    reservation.ObjectGeneration,
                    reservation.AuthorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
            var readyPayload = AuthorityPayload(
                operation,
                ZLinkInstanceSpotAuthorityState.Ready,
                stored.Reference,
                stored.ChecksumCrc32c);
            var commit = await authorityStore.CommitAsync(
                    reservation,
                    ZLinkInstanceSpotAuthorityPayloadCodec.Encode(readyPayload),
                    cancellationToken)
                .ConfigureAwait(false);
            if (commit is not (ZLinkObjectCommitResult.Committed
                or ZLinkObjectCommitResult.AlreadyCommitted))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Instance Spot '{operation.Target.TargetSpotId}' Ready commit conflicted.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            var readySnapshot = commit switch
            {
                ZLinkObjectCommitResult.Committed value => value.Snapshot,
                ZLinkObjectCommitResult.AlreadyCommitted value => value.Snapshot,
                _ => throw new InvalidOperationException()
            };
            committed = true;
            await catalog.PublishInstanceReservedAsync(
                    prepared,
                    readySnapshot.ObjectGeneration,
                    readySnapshot.AuthorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
            published = true;
            var terminal = await DispatchFirstMessageAsync(
                    prepared.Activation,
                    operation,
                    requestSource,
                    readySnapshot,
                    metadata,
                    payload,
                    cancellationToken)
                .ConfigureAwait(false);
            await RecordTerminalAndClearRecoveryAsync(
                    key,
                    readySnapshot,
                    readyPayload,
                    stored.Reference,
                    terminal)
                .ConfigureAwait(false);
            return terminal;
        }
        catch
        {
            if (!published && prepared is not null)
                await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
            if (!committed && reservation is not null)
                await authorityStore.AbortAsync(reservation, CancellationToken.None)
                    .ConfigureAwait(false);
            if (!committed)
                await relocationStore.DeleteRelocationAsync(
                        stored.Reference,
                        CancellationToken.None)
                    .ConfigureAwait(false);
            throw;
        }
    }

    internal async ValueTask RecoverAsync(CancellationToken cancellationToken)
    {
        ZLinkAuthorityScanCursor? cursor = null;
        do
        {
            var scan = await authorityStore.ListAuthoritiesAsync(
                    "zla1:s:",
                    cursor,
                    128,
                    cancellationToken)
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
        CancellationToken cancellationToken)
    {
        var snapshot = entry.Snapshot;
        var status = node.MeshStatus();
        if (snapshot.Allocation.ObjectKind != ZLinkPlacementObjectKind.InstanceSpot
            || !ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var authority)
            || authority.NodeRid != node.RoutingId
            || authority.NodeGeneration != status.LifecycleGeneration
            || authority.RecoveryReference is null)
            return;

        var root = await relocationStore.GetRelocationAsync(
                authority.RecoveryReference,
                cancellationToken)
            .ConfigureAwait(false);
        if (root is not ZLinkRelocationReadResult.Found found
            || Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(
                found.Payload.Span) != authority.RecoveryChecksum)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Instance Spot '{authority.SpotId}' activation recovery payload is unavailable.");

        if (authority.ReplayCursor == 1)
        {
            // A durable operation journal must acknowledge the retained terminal
            // before this pointer can be released. Startup recovery deliberately
            // preserves it until that journal boundary is available.
            return;
        }
        if (authority.ReplayCursor != 0
            || !ZLinkInstanceSpotActivationEnvelopeCodec.TryDecodeActivation(
                found.Payload.Span,
                out var record))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                $"Instance Spot '{authority.SpotId}' activation recovery payload is invalid.");

        if (!catalog.TryGetInstanceActivation(
                authority.SpotId,
                authority.StableType,
                snapshot.ObjectGeneration,
                out var activation))
        {
            var prepared = await catalog.PrepareInstanceReservedAsync(
                    authority.StableType,
                    authority.SpotId,
                    snapshot.ObjectGeneration,
                    snapshot.AuthorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
            if (authority.State == ZLinkInstanceSpotAuthorityState.Creating)
            {
                var pending = snapshot.ReservedCreation
                              ?? throw new ZLinkFrameworkException(
                                  ZLinkFrameworkErrorKind.ProtocolError,
                                  $"Instance Spot '{authority.SpotId}' reservation is incomplete.");
                var readyAuthority = authority with
                {
                    State = ZLinkInstanceSpotAuthorityState.Ready
                };
                var reservation = new ZLinkObjectReservation(
                    entry.Key,
                    snapshot.StoreVersion,
                    snapshot.ObjectGeneration,
                    snapshot.AuthorityOwnerGeneration,
                    pending.ReservationId,
                    snapshot.Allocation.Descriptor,
                    snapshot.Allocation.DescriptorLifecycleGeneration,
                    new ZLinkLocationOwnerToken(
                        snapshot.OwnerId,
                        snapshot.OwnerLeaseGeneration));
                var committed = await authorityStore.CommitAsync(
                        reservation,
                        ZLinkInstanceSpotAuthorityPayloadCodec.Encode(readyAuthority),
                        cancellationToken)
                    .ConfigureAwait(false);
                snapshot = committed switch
                {
                    ZLinkObjectCommitResult.Committed value => value.Snapshot,
                    ZLinkObjectCommitResult.AlreadyCommitted value => value.Snapshot,
                    _ => throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"Instance Spot '{authority.SpotId}' recovery lost its reservation.",
                        ZLinkRetryAdvice.RetryAfterBackoff)
                };
                authority = readyAuthority;
            }
            else if (authority.State != ZLinkInstanceSpotAuthorityState.Ready)
            {
                await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    $"Instance Spot '{authority.SpotId}' authority state is invalid.");
            }
            await catalog.PublishInstanceReservedAsync(
                    prepared,
                    snapshot.ObjectGeneration,
                    snapshot.AuthorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
            activation = prepared.Activation;
        }

        var operation = new InstanceSpotActivationOperation(
            new InstanceSpotActivationTarget(
                authority.MeshName,
                authority.NodeRid,
                authority.NodeGeneration,
                authority.SpotId,
                authority.StableType,
                snapshot.StoreVersion),
            record.SourceNodeRid,
            record.SourceNodeGeneration,
            record.SourceSpotId,
            record.OperationId,
            record.IsRequest,
            0,
            record.DeadlineUnixMs);
        var terminal = await DispatchFirstMessageAsync(
                activation,
                operation,
                record.RequestSource,
                snapshot,
                record.Metadata,
                record.Payload,
                cancellationToken)
            .ConfigureAwait(false);
        await RecordTerminalAndClearRecoveryAsync(
                entry.Key,
                snapshot,
                authority,
                authority.RecoveryReference,
                terminal)
            .ConfigureAwait(false);
    }

    private async ValueTask<InstanceSpotActivationTerminal> JoinExistingAsync(
        InstanceSpotActivationOperation operation,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        ZLinkObjectReserveResult reserve,
        ZLinkRelocationStored operationRoot,
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        CancellationToken cancellationToken)
    {
        var current = reserve switch
        {
            ZLinkObjectReserveResult.AlreadyExists value => value.Current,
            ZLinkObjectReserveResult.Conflict
            {
                Current: ZLinkAuthorityReadResult.Found value
            } => value.Snapshot,
            _ => throw ReserveFailure(operation, reserve)
        };
        while (true)
        {
            if (current.Allocation.ObjectKind != ZLinkPlacementObjectKind.InstanceSpot)
            {
                ZLinkRuntimeMetrics.RecordInstanceSpotClaimConflict(
                    operation.Target.MeshName,
                    operation.Target.StableType,
                    "spot_kind");
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    $"Instance Spot '{operation.Target.TargetSpotId}' has another stable type.");
            }
            if (!string.Equals(
                    current.Allocation.StableType,
                    operation.Target.StableType,
                    StringComparison.Ordinal))
            {
                ZLinkRuntimeMetrics.RecordInstanceSpotClaimConflict(
                    operation.Target.MeshName,
                    operation.Target.StableType,
                    "spot_type");
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    $"Instance Spot '{operation.Target.TargetSpotId}' has another stable type.");
            }
            if (!ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    current.Payload.Span,
                    out var authority))
            {
                ZLinkRuntimeMetrics.RecordInstanceSpotClaimConflict(
                    operation.Target.MeshName,
                    operation.Target.StableType,
                    "authority");
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Instance Spot '{operation.Target.TargetSpotId}' activation moved to another owner.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            }
            if (authority.NodeRid != node.RoutingId
                || authority.NodeGeneration != node.MeshStatus().LifecycleGeneration)
            {
                var forwarded = operation with
                {
                    Target = new InstanceSpotActivationTarget(
                        authority.MeshName,
                        authority.NodeRid,
                        authority.NodeGeneration,
                        authority.SpotId,
                        authority.StableType,
                        current.StoreVersion)
                };
                while (true)
                {
                    try
                    {
                        var terminal = await node
                            .ForwardInstanceSpotActivationAsync(
                                forwarded,
                                payload,
                                metadata,
                                cancellationToken)
                            .ConfigureAwait(false);
                        await relocationStore.DeleteRelocationAsync(
                                operationRoot.Reference,
                                CancellationToken.None)
                            .ConfigureAwait(false);
                        return terminal;
                    }
                    catch (ZlinkSubmitException exception)
                        when (exception.Result is
                            ZlinkSubmitException.ErrorCode.Backpressured
                            or ZlinkSubmitException.ErrorCode.NotConnected)
                    {
                        // The accepted operation remains durable while the
                        // winner route is temporarily unavailable.
                    }

                    var forwardRemaining = checked((long)operation.DeadlineUnixMs)
                                           - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
                    if (forwardRemaining <= 0)
                        throw new TimeoutException(
                            $"Instance Spot '{operation.Target.TargetSpotId}' activation forwarding deadline elapsed.");
                    await Task.Delay(
                            TimeSpan.FromMilliseconds(Math.Min(2, forwardRemaining)),
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
            var anotherOperationIsAccepted =
                authority.ReplayCursor == 0
                && authority.RecoveryReference is { } acceptedReference
                && !string.Equals(
                    acceptedReference,
                    operationRoot.Reference,
                    StringComparison.Ordinal);
            if (authority.State == ZLinkInstanceSpotAuthorityState.Ready
                && !anotherOperationIsAccepted
                && catalog.TryGetInstanceActivation(
                    authority.SpotId,
                    authority.StableType,
                    current.ObjectGeneration,
                    out var activation))
            {
                var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
                    operation.Target.TargetSpotId);
                var claimedAuthority = authority with
                {
                    RecoveryReference = operationRoot.Reference,
                    RecoveryChecksum = operationRoot.ChecksumCrc32c,
                    ReplayCursor = 0
                };
                var claimed = await authorityStore.CompareExchangeAuthorityAsync(
                        key,
                        current.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                                claimedAuthority),
                            ZLinkAuthorityGenerationTransition.Preserve,
                            null,
                            null),
                        cancellationToken)
                    .ConfigureAwait(false);
                if (claimed is not ZLinkAuthorityCompareExchangeResult.Stored stored)
                {
                    if (claimed is ZLinkAuthorityCompareExchangeResult.Conflict
                        {
                            Current: ZLinkAuthorityReadResult.Found conflict
                        })
                    {
                        current = conflict.Snapshot;
                        continue;
                    }
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"Instance Spot '{operation.Target.TargetSpotId}' operation claim conflicted.",
                        ZLinkRetryAdvice.RetryAfterBackoff);
                }
                if (authority.RecoveryReference is { } previousReference
                    && !string.Equals(
                        previousReference,
                        operationRoot.Reference,
                        StringComparison.Ordinal))
                    await relocationStore.DeleteRelocationAsync(
                            previousReference,
                            CancellationToken.None)
                        .ConfigureAwait(false);
                var terminal = await DispatchFirstMessageAsync(
                        activation,
                        operation,
                        requestSource,
                        stored.Snapshot,
                        metadata,
                        payload,
                        cancellationToken)
                    .ConfigureAwait(false);
                await RecordTerminalAndClearRecoveryAsync(
                        key,
                        stored.Snapshot,
                        claimedAuthority,
                        operationRoot.Reference,
                        terminal)
                    .ConfigureAwait(false);
                return terminal;
            }

            var remaining = checked((long)operation.DeadlineUnixMs)
                            - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            if (remaining <= 0)
                throw new TimeoutException(
                    $"Instance Spot '{operation.Target.TargetSpotId}' activation deadline elapsed.");
            await Task.Delay(
                    TimeSpan.FromMilliseconds(Math.Min(10, remaining)),
                    cancellationToken)
                .ConfigureAwait(false);
            var read = await authorityStore.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
                        operation.Target.TargetSpotId),
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Instance Spot '{operation.Target.TargetSpotId}' activation authority disappeared.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
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
        CancellationToken cancellationToken)
    {
        return await activation.DispatchDurableActivationAsync(
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
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkServiceWireCodec.RequestSourceFence>
        ResolveRequestSourceAsync(
            string meshName,
            RoutingId sourceNodeRid,
            ulong sourceNodeGeneration,
            CancellationToken cancellationToken)
    {
        var descriptors = await authorityStore.ListAllMeshNodesAsync(
                meshName,
                cancellationToken)
            .ConfigureAwait(false);
        var descriptor = descriptors.SingleOrDefault(value =>
            value.Rid == sourceNodeRid
            && value.LifecycleGeneration == sourceNodeGeneration);
        if (descriptor is null || descriptor.LeaseGeneration <= 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "The Instance Spot activation request-source fence is unavailable.");
        return new ZLinkServiceWireCodec.RequestSourceFence(
            descriptor.OwnerId,
            checked((ulong)descriptor.LeaseGeneration),
            sourceNodeRid,
            sourceNodeGeneration);
    }

    private async ValueTask RecordTerminalAndClearRecoveryAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot readySnapshot,
        ZLinkInstanceSpotAuthorityPayload readyPayload,
        string recoveryReference,
        InstanceSpotActivationTerminal terminal)
    {
        var activationRoot = await relocationStore.GetRelocationAsync(
                recoveryReference,
                CancellationToken.None)
            .ConfigureAwait(false);
        if (activationRoot is not ZLinkRelocationReadResult.Found activation)
            throw new Zlink.Framework.Runtime.Locations.ZLinkRelocationDataLostException(
                "The Instance Spot activation root disappeared before terminal publication.");
        var terminalEnvelope = ZLinkInstanceSpotActivationEnvelopeCodec.EncodeTerminal(
            activation.Payload,
            terminal);
        var terminalStored = await relocationStore.PutRelocationAsync(
                terminalEnvelope,
                RecoveryRetention,
                CancellationToken.None)
            .ConfigureAwait(false);
        var advancedPayload = readyPayload with
        {
            RecoveryReference = terminalStored.Reference,
            RecoveryChecksum = terminalStored.ChecksumCrc32c,
            ReplayCursor = 1
        };
        var advanced = await authorityStore.CompareExchangeAuthorityAsync(
                key,
                readySnapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    ZLinkInstanceSpotAuthorityPayloadCodec.Encode(advancedPayload),
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null),
                CancellationToken.None)
            .ConfigureAwait(false);
        if (advanced is not ZLinkAuthorityCompareExchangeResult.Stored)
            return;
        await relocationStore.DeleteRelocationAsync(
                recoveryReference,
                CancellationToken.None)
            .ConfigureAwait(false);
        // Keep the retained terminal published until an operation journal
        // acknowledges it. Clearing it here would lose the only replay evidence
        // if the source retries after this process exits.
    }

    private async ValueTask<InstanceSpotActivationTerminal?> TryReplayRetainedTerminalAsync(
        ZLinkAuthorityKey key,
        MeshOperationId operationId,
        CancellationToken cancellationToken)
    {
        var read = await authorityStore.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || !ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var authority)
            || authority.ReplayCursor != 1
            || authority.RecoveryReference is null)
            return null;
        var root = await relocationStore.GetRelocationAsync(
                authority.RecoveryReference,
                cancellationToken)
            .ConfigureAwait(false);
        if (root is not ZLinkRelocationReadResult.Found retained
            || Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(
                retained.Payload.Span) != authority.RecoveryChecksum
            || !ZLinkInstanceSpotActivationEnvelopeCodec.TryDecodeTerminal(
                retained.Payload.Span,
                out var original,
                out var terminal))
            throw new Zlink.Framework.Runtime.Locations.ZLinkRelocationDataLostException(
                "The retained Instance Spot activation terminal is invalid.");
        return original.OperationId == operationId ? terminal : null;
    }

    private ZLinkInstanceSpotAuthorityPayload AuthorityPayload(
        InstanceSpotActivationOperation operation,
        ZLinkInstanceSpotAuthorityState state,
        string? recoveryReference,
        uint recoveryChecksum) =>
        new(
            state,
            operation.Target.TargetSpotId,
            operation.Target.StableType,
            operation.Target.MeshName,
            node.RoutingId,
            node.MeshStatus().LifecycleGeneration,
            owner.OwnerId,
            checked((ulong)owner.LeaseGeneration),
            recoveryReference,
            recoveryChecksum,
            0);

    private void ValidateTarget(InstanceSpotActivationOperation operation)
    {
        var status = node.MeshStatus();
        if (operation.Target.TargetNodeRid != node.RoutingId
            || operation.Target.TargetNodeGeneration != status.LifecycleGeneration
            || !string.Equals(
                operation.Target.MeshName,
                registration.SpotNodeName,
                StringComparison.Ordinal))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "The Instance Spot activation target descriptor is stale.");
    }

    private static Exception ReserveFailure(
        InstanceSpotActivationOperation operation,
        ZLinkObjectReserveResult result) =>
        result switch
        {
            ZLinkObjectReserveResult.TypeMismatch => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TypeMismatch,
                $"Instance Spot '{operation.Target.TargetSpotId}' has another stable type."),
            ZLinkObjectReserveResult.PlacementCapacityExhausted =>
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.CapacityExceeded,
                    "The Instance Spot target has no remaining capacity.",
                    ZLinkRetryAdvice.RetryAfterBackoff),
            _ => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Instance Spot '{operation.Target.TargetSpotId}' activation conflicted.",
                ZLinkRetryAdvice.RetryAfterBackoff)
        };

}
