using System.Security.Cryptography;
using System.Text;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Host;

internal sealed record ZLinkStandaloneActorRelocationTakeover(
    ZLinkAuthoritySnapshot Authority,
    ZLinkActorAuthorityPayload TargetAuthority,
    ZLinkActorRuntimeState ActorState,
    ZLinkObjectRelocationRegistration Relocation,
    ulong TargetAttemptGeneration,
    ZLinkRelocationPermitPool.ZLinkRelocationPermitLease Permit);

/// <summary>
/// Replaces a failed standalone Actor relocation target. The immutable root is
/// prepared data; one authority CAS is the publication boundary for the new
/// target attempt.
/// </summary>
internal sealed class ZLinkStandaloneActorRelocationTakeoverCoordinator(
    ZLinkFrameworkRuntime runtime,
    ZLinkActorSessionManager actorSessions,
    ZLinkFrameworkRegistration registration)
{
    internal async ValueTask<bool> HasLiveRemoteRecoveryOwnerAsync(
        ZLinkCanonicalRelocationAuthorityProjection projection,
        string meshName,
        CancellationToken cancellationToken)
    {
        var store = registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        var descriptors = await store.ListAllMeshNodesAsync(
                meshName,
                cancellationToken)
            .ConfigureAwait(false);

        if (projection.TargetAttemptGeneration != 0
            && await IsLiveRemoteFenceAsync(
                    projection.State.TargetNodeRid,
                    projection.State.TargetNodeGeneration,
                    projection.TargetOwnerId,
                    projection.TargetOwnerLeaseGeneration,
                    descriptors,
                    store,
                    cancellationToken)
                .ConfigureAwait(false))
            return true;

        // Once owner publication has committed, the target attempt is the
        // only runtime allowed to keep recovery remote. The source
        // coordinator may still be healthy, but it cannot materialize the
        // committed target after that target has failed.
        if (projection.Phase >= 4)
            return false;

        return await IsLiveRemoteFenceAsync(
                projection.State.CoordinatorNodeRid,
                projection.State.CoordinatorNodeGeneration,
                projection.State.CoordinatorOwnerId,
                projection.State.CoordinatorLeaseGeneration,
                descriptors,
                store,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkStandaloneActorRelocationTakeover> TakeOverAsync(
        ZLinkAuthorityEntry current,
        ZLinkCanonicalRelocationAuthorityProjection projection,
        ZLinkRelocationEnvelope envelope,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkCanonicalParticipantRecovery recovery,
        CancellationToken cancellationToken)
    {
        var store = registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        if (projection.Phase == 3)
        {
            var previousFence = ZLinkCanonicalRelocationReservationOwner
                .CapacityFence(
                    new ZLinkServiceWireCodec.RelocationWireId(
                        envelope.CanonicalRelocationHigh,
                        envelope.CanonicalRelocationLow),
                    projection.TargetAttemptGeneration);
            var aborted = await store.AbortRelocationCapacityAsync(
                    previousFence,
                    cancellationToken)
                .ConfigureAwait(false);
            if (aborted is not (
                    ZLinkRelocationCapacityAbortResult.Aborted
                    or ZLinkRelocationCapacityAbortResult.AlreadyAborted))
                throw DataLost(
                    "Standalone Actor takeover could not close the previous target capacity fence.");
        }
        if (!ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                current.Snapshot.Payload.Span,
                out var previousTarget))
            throw DataLost(
                "Standalone Actor takeover cannot decode its current authority.");
        if (projection.TargetAttemptGeneration == ulong.MaxValue)
            throw new ZLinkAuthorityGenerationExhaustedException(
                "replacing a standalone Actor relocation target");

        var nextAttempt = projection.TargetAttemptGeneration + 1;
        var requiredPolicy = recoveryPolicy(previousTarget.StableType);
        var candidates = await store.ListAllMeshNodesAsync(
                current.Snapshot.Allocation.Descriptor.MeshName,
                cancellationToken)
            .ConfigureAwait(false);
        var localCandidates = candidates
            .Where(candidate => IsEligibleLocalCandidate(
                candidate,
                projection,
                recovery.StableType,
                requiredPolicy,
                envelope.CanonicalApplicationVersion))
            .OrderByDescending(static candidate => candidate.PlacementWeight)
            .ThenBy(static candidate => candidate.Rid.ToHex(), StringComparer.Ordinal)
            .ToArray();
        if (localCandidates.Length == 0)
            throw TargetUnavailable(previousTarget.ActorId);

        foreach (var candidate in localCandidates)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var owner = new ZLinkLocationOwnerToken(
                candidate.OwnerId,
                candidate.LeaseGeneration);
            var reservation = await store.ReserveRelocationCapacityAsync(
                    new ZLinkRelocationCapacityReservationRequest(
                        ReservationId(envelope.AggregateId, nextAttempt, candidate),
                        current.Key,
                        current.Snapshot.StoreVersion,
                        ZLinkPlacementObjectKind.Actor,
                        recovery.StableType,
                        current.Snapshot.Allocation.Descriptor,
                        current.Snapshot.Allocation.DescriptorLifecycleGeneration,
                        new ZLinkLocationOwnerToken(
                            current.Snapshot.OwnerId,
                            current.Snapshot.OwnerLeaseGeneration),
                        new ZLinkMeshNodeDescriptorKey(
                            candidate.MeshName,
                            candidate.Rid),
                        candidate.LifecycleGeneration,
                        owner,
                        new ZLinkCapacityVector(1, 0, null)),
                    cancellationToken)
                .ConfigureAwait(false);
            if (reservation is ZLinkRelocationCapacityReserveResult.Conflict)
                throw Moving(previousTarget.ActorId,
                    "authority changed while reserving replacement capacity");
            if (reservation is ZLinkRelocationCapacityReserveResult.TargetUnavailable
                or ZLinkRelocationCapacityReserveResult.PlacementCapacityExhausted)
                continue;
            var (capacityFence, targetAuthorityOwnerGeneration) =
                reservation switch
            {
                ZLinkRelocationCapacityReserveResult.Reserved reserved =>
                    (reserved.Fence,
                        reserved.TargetAuthorityOwnerGeneration),
                ZLinkRelocationCapacityReserveResult.AlreadyReserved already =>
                    (already.Fence,
                        already.TargetAuthorityOwnerGeneration),
                _ => throw new InvalidOperationException(
                    "Location Store returned an invalid relocation capacity result.")
            };
            if (targetAuthorityOwnerGeneration
                    <= current.Snapshot.AuthorityOwnerGeneration
                || targetAuthorityOwnerGeneration > long.MaxValue)
                throw DataLost(
                    "Replacement capacity reservation returned an invalid target authority generation.");

            var node = runtime.GetSpotNodeRuntime(candidate.Rid);
            var relocation = node.Registration.ActorRelocations[recovery.StableType];
            var targetAuthority = previousTarget with
            {
                State = ZLinkActorAuthorityState.Ready,
                CurrentSpotId = candidate.EntrySpotId!,
                CurrentSpotGeneration = candidate.LifecycleGeneration,
                CurrentSpotKind = ZLinkSpotKind.Entry,
                OwnerId = candidate.OwnerId,
                OwnerLeaseGeneration = checked((ulong)candidate.LeaseGeneration),
                MeshName = candidate.MeshName,
                NodeRid = candidate.Rid,
                NodeGeneration = candidate.LifecycleGeneration
            };
            var actorState = actorSessions.GetOrCreateState(previousTarget.ActorId);
            var created = false;
            var recoveryPermit =
                default(ZLinkRelocationPermitPool.ZLinkRelocationPermitLease);
            try
            {
                if (!runtime.RelocationPermits.TryAcquire(
                        ZLinkRelocationPermitRequest.Inbound(
                            ZLinkRelocationEnvelopeCodec.MeasureEncodedLength(envelope),
                            restore: relocation.PolicyKind == 2),
                        out recoveryPermit))
                    throw Moving(previousTarget.ActorId,
                        "replacement restore admission is busy");
                await actorSessions.PrepareForTransferredActivationAsync(
                        actorState,
                        cancellationToken)
                    .ConfigureAwait(false);
                var creation = await actorSessions.RelocateAndBindActorAsync(
                        targetAuthority.ActorId,
                        targetAuthority.StableType,
                        relocation,
                        ZLinkActorRelocationRegistry.ValidateIncomingPayload(
                            relocation,
                            targetAuthority.StableType,
                            relocation.PolicyKind == 2
                                ? ZLinkRemoteActorJoinPackets
                                    .SnapshotRelocationContentType
                                : ZLinkRemoteActorJoinPackets
                                    .RecreateRelocationContentType,
                        participant.ApplicationState),
                        participant.ObjectGeneration,
                        targetAuthorityOwnerGeneration,
                        ZLinkActorClaimMode.StagedRelocation,
                        publishActorRef: false,
                        cancellationToken)
                    .ConfigureAwait(false);
                created = creation.Created;

                var replacement = ZLinkCanonicalRelocationAuthorityStateCodec
                    .ReplaceRelocationState(
                        ZLinkActorAuthorityPayloadCodec.Encode(targetAuthority),
                        projection.State with
                        {
                            TargetAttemptGeneration = nextAttempt,
                            TargetNodeRid = candidate.Rid.ToHex(),
                            TargetNodeGeneration = candidate.LifecycleGeneration,
                            TargetOwnerId = candidate.OwnerId,
                            TargetOwnerLeaseGeneration = checked(
                                (ulong)candidate.LeaseGeneration),
                            ReservationGeneration = nextAttempt,
                            CoordinatorOwnerId = candidate.OwnerId,
                            CoordinatorLeaseGeneration = checked(
                                (ulong)candidate.LeaseGeneration),
                            CoordinatorNodeRid = candidate.Rid.ToHex(),
                            CoordinatorNodeGeneration = candidate.LifecycleGeneration,
                            Phase = (byte)
                                ZLinkStandaloneActorCanonicalPhase.Committed
                        },
                        envelope);
                var exchanged = await store.CompareExchangeAuthorityAsync(
                        current.Key,
                        current.Snapshot.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            replacement,
                            ZLinkAuthorityGenerationTransition.NewOwner,
                            owner,
                            capacityFence),
                        cancellationToken)
                    .ConfigureAwait(false);
                if (exchanged is ZLinkAuthorityCompareExchangeResult.Stored stored)
                    return new ZLinkStandaloneActorRelocationTakeover(
                        stored.Snapshot,
                        targetAuthority,
                        actorState,
                        relocation,
                        nextAttempt,
                        recoveryPermit);
                if (exchanged is ZLinkAuthorityCompareExchangeResult.GenerationExhausted)
                    throw new ZLinkAuthorityGenerationExhaustedException(
                        "publishing a replacement standalone Actor relocation target");
                throw Moving(previousTarget.ActorId,
                    "another recovery owner published the replacement target");
            }
            catch
            {
                try
                {
                    if (created)
                        await actorSessions.RollbackTransferredActorAsync(
                                previousTarget.ActorId,
                                CancellationToken.None)
                            .ConfigureAwait(false);
                }
                finally
                {
                    try
                    {
                        await store.AbortRelocationCapacityAsync(
                                capacityFence,
                                CancellationToken.None)
                            .ConfigureAwait(false);
                    }
                    finally
                    {
                        recoveryPermit.Dispose();
                    }
                }
                throw;
            }
        }

        throw TargetUnavailable(previousTarget.ActorId);

        ZLinkObjectMaintenancePolicyKind recoveryPolicy(string stableType)
        {
            foreach (var node in registration.SpotNodes.Values)
                if (node.ActorRelocations.TryGetValue(stableType, out var relocation))
                    return relocation.PolicyKind switch
                    {
                        1 => ZLinkObjectMaintenancePolicyKind.Recreate,
                        2 => ZLinkObjectMaintenancePolicyKind.Snapshot,
                        _ => ZLinkObjectMaintenancePolicyKind.Disabled
                    };
            return ZLinkObjectMaintenancePolicyKind.Disabled;
        }
    }

    internal static bool IsCurrentAttempt(
        ZLinkAuthoritySnapshot snapshot,
        Guid relocationId,
        ulong targetAttemptGeneration,
        ZLinkActorAuthorityPayload targetAuthority)
    {
        if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                snapshot.Payload.Span,
                out var current))
            return false;
        Span<byte> id = stackalloc byte[16];
        relocationId.TryWriteBytes(id, bigEndian: true, out _);
        var high = System.Buffers.Binary.BinaryPrimitives
            .ReadUInt64BigEndian(id);
        var low = System.Buffers.Binary.BinaryPrimitives
            .ReadUInt64BigEndian(id[8..]);
        return current.RelocationHigh == high
               && current.RelocationLow == low
               && current.TargetAttemptGeneration == targetAttemptGeneration
               && StringComparer.Ordinal.Equals(
                   current.State.TargetNodeRid,
                   targetAuthority.NodeRid.ToHex())
               && current.State.TargetNodeGeneration
               == targetAuthority.NodeGeneration
               && StringComparer.Ordinal.Equals(
                   current.TargetOwnerId,
                   targetAuthority.OwnerId)
               && current.TargetOwnerLeaseGeneration
               == targetAuthority.OwnerLeaseGeneration
               && StringComparer.Ordinal.Equals(
                   snapshot.OwnerId,
                   targetAuthority.OwnerId)
               && snapshot.OwnerLeaseGeneration
               == checked((long)targetAuthority.OwnerLeaseGeneration);
    }

    private bool IsEligibleLocalCandidate(
        ZLinkMeshNodeDescriptor candidate,
        ZLinkCanonicalRelocationAuthorityProjection current,
        string stableType,
        ZLinkObjectMaintenancePolicyKind requiredPolicy,
        long minimumApplicationVersion)
    {
        var node = runtime.TryGetSpotNodeRuntime(candidate.Rid);
        return node is not null
               && node.Node.MeshStatus().LifecycleGeneration
               == candidate.LifecycleGeneration
               && !StringComparer.Ordinal.Equals(
                   candidate.Rid.ToHex(),
                   current.State.TargetNodeRid)
               && candidate.State == ZLinkFrameworkRuntimeState.Serving
               && candidate.ObjectRole == ZLinkMeshNodeObjectRole.Server
               && candidate.PlacementWeight > 0
               && candidate.LeaseGeneration > 0
               && candidate.Rid is { Size: > 0 }
               && !string.IsNullOrWhiteSpace(candidate.EntrySpotId)
               && ZLinkSpotRetireTargetRuntime
                   .MatchesTakeoverApplicationVersion(
                       candidate.ApplicationVersion,
                       minimumApplicationVersion)
               && ZLinkSpotRetireTargetRuntime.HasHeadroom(
                   candidate.Capacity.Actors,
                   1)
               && candidate.ActivationConcurrency.Limit
                  - candidate.ActivationConcurrency.Active >= 1
               && candidate.ObjectCapabilities.Any(capability =>
                   capability.ObjectKind == ZLinkPlacementObjectKind.Actor
                   && StringComparer.Ordinal.Equals(capability.StableType, stableType)
                   && capability.Policy == requiredPolicy
                   && (requiredPolicy != ZLinkObjectMaintenancePolicyKind.Snapshot
                       || capability.HasSnapshotAdapter))
               && node.Registration.ActorRelocations.TryGetValue(
                   stableType,
                   out var relocation)
               && relocation.PolicyKind != 0;
    }

    private async ValueTask<bool> IsLiveRemoteFenceAsync(
        string nodeRid,
        ulong nodeGeneration,
        string ownerId,
        ulong ownerLeaseGeneration,
        IReadOnlyList<ZLinkMeshNodeDescriptor> descriptors,
        IZLinkLocationRepository store,
        CancellationToken cancellationToken)
    {
        if (nodeGeneration == 0
            || ownerLeaseGeneration == 0
            || ownerLeaseGeneration > long.MaxValue
            || string.IsNullOrWhiteSpace(nodeRid)
            || string.IsNullOrWhiteSpace(ownerId))
            throw DataLost(
                "Standalone Actor recovery owner fence is malformed.");

        RoutingId rid;
        try
        {
            rid = RoutingId.FromHex(nodeRid);
        }
        catch (Exception exception) when (exception is ArgumentException
                                          or FormatException)
        {
            throw DataLost(
                "Standalone Actor recovery owner RID is malformed.");
        }

        var local = runtime.TryGetSpotNodeRuntime(rid);
        if (local is not null
            && local.Node.MeshStatus().LifecycleGeneration == nodeGeneration)
            return false;

        var lease = await store.ReadOwnerLeaseAsync(ownerId, cancellationToken)
            .ConfigureAwait(false);
        if (lease is not ZLinkOwnerLeaseReadResult.Found found
            || !StringComparer.Ordinal.Equals(found.Token.OwnerId, ownerId)
            || found.Token.LeaseGeneration
               != checked((long)ownerLeaseGeneration)
            || found.LeaseExpiresAt <= found.StoreNow)
            return false;

        return descriptors.Any(descriptor =>
            descriptor.Rid == rid
            && descriptor.LifecycleGeneration == nodeGeneration
            && StringComparer.Ordinal.Equals(descriptor.OwnerId, ownerId)
            && descriptor.LeaseGeneration
               == checked((long)ownerLeaseGeneration));
    }

    private static Guid ReservationId(
        Guid relocationId,
        ulong attempt,
        ZLinkMeshNodeDescriptor target)
    {
        var identity = Encoding.UTF8.GetBytes(
            $"{relocationId:N}:{attempt}:{target.Rid.ToHex()}:{target.LifecycleGeneration}");
        var digest = SHA256.HashData(identity);
        return new Guid(digest.AsSpan(0, 16), bigEndian: true);
    }

    private static ZLinkFrameworkException TargetUnavailable(string actorId) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"Actor '{actorId}' has no eligible local replacement target.",
            retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);

    private static ZLinkFrameworkException Moving(string actorId, string reason) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"Actor '{actorId}' recovery will retry because {reason}.",
            retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);

    private static ZLinkFrameworkException DataLost(string message) =>
        new(
            ZLinkFrameworkErrorKind.DataLost,
            message,
            retryAdvice: ZLinkRetryAdvice.DoNotRetry);
}
