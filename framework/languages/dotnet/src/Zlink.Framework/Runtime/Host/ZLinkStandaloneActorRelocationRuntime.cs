using System.Collections.Concurrent;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Host;

internal readonly record struct ZLinkStandaloneActorRelocationDestination(
    string SpotId,
    ulong SpotObjectGeneration,
    ZLinkSpotKind SpotKind,
    RoutingId NodeRid,
    ulong NodeLifecycleGeneration,
    string MeshName,
    ZLinkLocationOwnerToken Owner);

internal enum ZLinkStandaloneActorRelocationResult
{
    Committed,
    Deferred,
    TargetRejected
}

/// <summary>
/// Owns maintenance relocation for Actors that are attached to an Entry Spot.
/// Application join admission is deliberately outside this module: a maintenance
/// relocation restores an existing Actor under an exact authority fence and is
/// not a new application-level join.
/// </summary>
internal sealed class ZLinkStandaloneActorRelocationRuntime(
    ZLinkFrameworkRuntime runtime,
    ZLinkActorSessionManager actorSessions,
    ZLinkFrameworkRegistration registration)
{
    private const int MaximumTargetStages = 1024;
    private static readonly TimeSpan TargetStageTtl = TimeSpan.FromMinutes(5);
    private readonly ConcurrentDictionary<AttemptKey, TargetStage> _targetStages = new();
    private readonly ConcurrentDictionary<AttemptKey, SemaphoreSlim> _targetStageGates = new();
    private readonly ConcurrentDictionary<AttemptKey,
        ZLinkRelocationPermitPool.ZLinkRelocationPermitLease>
        _recoveryPermits = new();

    internal async ValueTask<ZLinkStandaloneActorRelocationResult> RelocateSourceAsync(
        ZLinkActorRuntimeState actorState,
        ZLinkMeshNodeDescriptor target,
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken)
    {
        var sourceActivation = actorState.LiveActivation;
        var destination = ResolveDestination(sourceActivation, target);
        var actor = actorState.Actor;
        var sourceRef = actorState.NativeActorRef;
        var actorType = actorState.ActorType;
        //  두 거부 조건이 같은 열거값으로 합쳐져 호출자는 무엇 때문인지 알 수
        //  없다. 특히 아래 동일-node 검사는 `Descriptor.Rid`로 하는데 후보를
        //  거르는 쪽은 `Target.NodeRid`를 쓰므로, 두 식별자가 어긋나면 필터를
        //  통과한 후보가 여기서 전부 거부된다. 어느 쪽인지 남긴다.
        if (actor is null || sourceRef is null
            || string.IsNullOrWhiteSpace(actorType))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                "relocation_target_rejected reason=incomplete_actor_state actor="
                + actorState.ActorId);
            return ZLinkStandaloneActorRelocationResult.TargetRejected;
        }
        if (target.Rid == sourceRef.Value.NodeRid)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                "relocation_target_rejected reason=target_is_source actor="
                + actorState.ActorId
                + " targetRid=" + target.Rid
                + " sourceRid=" + sourceRef.Value.NodeRid);
            return ZLinkStandaloneActorRelocationResult.TargetRejected;
        }
        ZLinkActorRelocationRegistry.TryResolve(
            registration,
            actorType,
            sourceRef.Value.NodeRid,
            out var relocation);
        if (relocation is null || relocation.PolicyKind == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Relocation is disabled for Actor '{actorState.ActorId}'.");

        var authorityStore = registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Standalone Actor relocation requires a Location Store.");
        var relocationStore = registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Standalone Actor relocation requires a Relocation Store.");
        var read = await authorityStore.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorState.ActorId),
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || found.Snapshot.ObjectGeneration != sourceRef.Value.Generation
            || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                found.Snapshot.Payload.Span,
                out var sourceAuthority)
            || sourceAuthority.NodeRid != sourceRef.Value.NodeRid)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorState.ActorId}' authority changed before maintenance relocation.");

        var predictedBytes = ZLinkRemoteActorJoinPackets
            .MeasureStandaloneMaintenancePayloadUpperBound(
                snapshot: relocation.PolicyKind == 2);
        if (!runtime.RelocationPermits.TryAcquire(
                ZLinkRelocationPermitRequest.Outbound(
                    predictedBytes,
                    capture: relocation.PolicyKind == 2),
                out var sourcePermit))
            return ZLinkStandaloneActorRelocationResult.Deferred;
        using (sourcePermit)
        {
            var relocationId = Guid.NewGuid();
            var handoffId = relocationId.ToString("N");
            //  Drain이 끄는 per-actor relocation도 remote-join 경로와 같은 terminal
            //  metric을 남겨야 한다. 여기가 없으면 `zlink.relocation.completed`는
            //  application이 직접 옮긴 경우에만 올라간다.
            var relocationMetric = ZLinkRuntimeMetrics.CreateRelocation(
                actor.Context.MeshName,
                ZLinkRelocationMetricObjectKind.Actor,
                relocation.PolicyKind != 2
                    ? ZLinkRelocationMetricPolicy.Recreate
                    : ZLinkRelocationMetricPolicy.Snapshot);
            relocationMetric.Start();
            var captureStarted = false;
            var committed = false;
            var sourceTerminalized = false;
            ZLinkActorBoundSession? sealedSession = null;
            ZLinkPreparedRelocation? initialPrepared = null;
            ZLinkPreparedRelocation? prepared = null;
            ZLinkRelocationEnvelope? envelope = null;
            IZLinkBackendCanonicalRelocationReservation? canonical = null;
            ZLinkServiceWireCodec.RelocationPrepareRecord? prepare = null;
            var precommit = new ZLinkStandaloneActorRelocationPrecommitCoordinator(
                authorityStore);
            ZLinkAuthoritySnapshot? precommitSnapshot = null;
            var acceptedCount = 0;
            var interruption =
                ZLinkRelocationInterruptionOperation.Disabled;
            try
            {
                var route = default(ZLinkRemoteActorBoundSessionRoute);
                if (actorState.TryGetBoundSession(out var bound))
                {
                    sealedSession = await SealSessionRouteAsync(
                            actorState,
                            bound,
                            handoffId,
                            cancellationToken)
                        .ConfigureAwait(false);
                    route = ToRemoteRoute(sealedSession.Value);
                }

                _ = await actorState.BeginHandoffCaptureAsync(cancellationToken)
                    .ConfigureAwait(false);
                captureStarted = true;
                interruption = runtime.RelocationInterruption.Start(
                    ZLinkRelocationUnitKind.Actor,
                    sourceActivation is null ? "entry" : "per_actor");
                var applicationState = await ZLinkActorRelocationRegistry.CaptureAsync(
                        runtime.Services,
                        relocation,
                        actor,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (applicationState.Length
                    > ZLinkRemoteActorJoinPackets
                        .SnapshotApplicationStateReservationBytes)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Rejected,
                        $"Actor '{actorState.ActorId}' relocation state exceeds 64 MiB.");
                // Switch ingress to the bounded final hold while the immutable
                // journal is built. The exact commit boundary is frozen only
                // after the source fences in the initial journal are verified.
                var sourceNode = runtime.GetSpotNodeRuntime(
                    sourceRef.Value.NodeRid);
                actorState.Handoff.SealCapture();
                var semanticSealRecords = CreateAcceptedRecords(
                    actorState.Handoff.SnapshotFrames());
                await ZLinkActorRequestSourceFenceValidator.ValidateAsync(
                        authorityStore,
                        sourceAuthority.MeshName,
                        sourceNode.Node.MeshStatus(),
                        sourceNode.Node.MeshPeers(),
                        semanticSealRecords,
                        RemainingTimeout(absoluteDeadline),
                        cancellationToken)
                    .ConfigureAwait(false);
                precommitSnapshot = await precommit.BeginPreparingAsync(
                        found.Snapshot,
                        sourceAuthority,
                        relocationId,
                        registration.ApplicationVersion,
                        cancellationToken)
                    .ConfigureAwait(false);
                var initialEnvelope = ZLinkCanonicalActorRelocationWriter.CreateInitial(
                    CreateImmutableRoot(
                        precommitSnapshot,
                        sourceAuthority,
                        destination,
                        relocationId,
                        applicationState,
                        semanticSealRecords,
                        route,
                        maintenancePolicy: ToMaintenancePolicy(
                            relocation.PolicyKind)),
                    registration.ApplicationVersion);
                var publication = new ZLinkRelocationPublicationCoordinator(
                    authorityStore,
                    relocationStore);
                initialPrepared = await publication.PrepareAsync(
                        initialEnvelope,
                        cancellationToken)
                    .ConfigureAwait(false);
                precommitSnapshot = await precommit.CaptureAsync(
                        precommitSnapshot,
                        initialEnvelope,
                        initialPrepared.Relocation,
                        cancellationToken)
                    .ConfigureAwait(false);

                if (sourceNode.Node
                    is not IZLinkBackendCanonicalRelocationReservation backend)
                    throw new ZLinkConfigurationException(
                        "The source MeshNode does not support canonical relocation commands.");
                canonical = backend;
                prepare = CreatePrepare(
                        precommitSnapshot,
                        sourceAuthority,
                        target,
                        initialEnvelope,
                        initialPrepared.Relocation,
                        sealedSession,
                        registration.ApplicationVersion,
                        checked((ulong)semanticSealRecords.Count
                            + ZLinkBoundedIngressAdmission
                                .SourceIngressHoldRecordCapacity),
                        checked(initialEnvelope.Participants[0].AcceptedJobs
                                .Aggregate(0UL, static (sum, job) =>
                                    checked(sum + (ulong)job.Payload.Length))
                            + (ulong)ZLinkBoundedIngressAdmission
                                .SourceIngressHoldByteCapacity),
                        checked((ulong)sourcePermit.ReservedPayloadBytes));
                _ = await canonical.ReserveCanonicalRelocationAsync(
                        target.Rid,
                        prepare,
                        RemainingTimeout(absoluteDeadline),
                        cancellationToken)
                    .ConfigureAwait(false);
                var commitBoundary = actorState.Handoff
                    .FreezeCaptureCommitBoundary();
                var acceptedRecords = CreateAcceptedRecords(
                    commitBoundary.Frames);
                acceptedCount = acceptedRecords.Count;
                await ZLinkActorRequestSourceFenceValidator.ValidateAsync(
                        authorityStore,
                        sourceAuthority.MeshName,
                        sourceNode.Node.MeshStatus(),
                        sourceNode.Node.MeshPeers(),
                        acceptedRecords,
                        RemainingTimeout(absoluteDeadline),
                        cancellationToken)
                    .ConfigureAwait(false);
                envelope = ZLinkCanonicalActorRelocationWriter.CreateInitial(
                    CreateImmutableRoot(
                        precommitSnapshot,
                        sourceAuthority,
                        destination,
                        relocationId,
                        applicationState,
                        acceptedRecords,
                        route,
                        maintenancePolicy: ToMaintenancePolicy(
                            relocation.PolicyKind)),
                    registration.ApplicationVersion);
                if (envelope.Participants.Single().AcceptedBoundary
                    != commitBoundary.AcceptedHighWater)
                    throw DataLost(
                        "Standalone Actor commit high-water changed while building its final root.");
                prepared = await publication.PrepareAsync(
                        envelope,
                        cancellationToken)
                    .ConfigureAwait(false);
                precommitSnapshot = await precommit.RefreshCapturedRootAsync(
                        precommitSnapshot,
                        envelope,
                        prepared.Relocation,
                        ZLinkCanonicalRelocationReservationOwner
                            .CapacityFence(
                                prepare.RelocationId,
                                prepare.TargetAttemptGeneration),
                        cancellationToken)
                    .ConfigureAwait(false);
                var encodedBytes = ZLinkRelocationEnvelopeCodec
                    .MeasureEncodedLength(envelope);
                if (!sourcePermit.TryShrinkPayload(checked(
                        encodedBytes + commitBoundary.RemainingHoldBytes)))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Rejected,
                        $"Actor '{actorState.ActorId}' relocation root exceeded its reservation.");
                var data = envelope.Participants[0].AcceptedJobs
                    .Select((job, index) =>
                        new ZLinkServiceWireCodec.RelocationDataRecord(
                            prepare.RelocationId,
                            prepare.TargetAttemptGeneration,
                            prepare.Coordinator,
                            1,
                            1,
                            checked((ulong)index + 1),
                            new ZLinkServiceWireCodec.FrozenRecord(
                                job.Payload.ToArray())))
                    .ToArray();
                await canonical.StageCanonicalRelocationAsync(
                        target.Rid,
                        prepare,
                        data,
                        RemainingTimeout(absoluteDeadline),
                        cancellationToken)
                    .ConfigureAwait(false);
                var committedTarget = await WaitForCommittedTargetAuthorityAsync(
                        authorityStore,
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                            actorState.ActorId),
                        found.Snapshot,
                        prepared.Relocation,
                        relocationId,
                        target,
                        prepare.TargetAttemptGeneration,
                        cancellationToken)
                    .ConfigureAwait(false);
                committed = true;
                if (!StringComparer.Ordinal.Equals(
                        initialPrepared.Relocation.Reference,
                        prepared.Relocation.Reference)
                    || initialPrepared.Relocation.ChecksumCrc32c
                       != prepared.Relocation.ChecksumCrc32c)
                    await publication.DiscardPreparedAsync(initialPrepared)
                        .ConfigureAwait(false);
                await CompleteCommittedSourceAsync(
                        actor,
                        actorState,
                        sourceRef.Value,
                        sourceAuthority,
                        found.Snapshot,
                        target,
                        relocationId,
                        acceptedCount,
                        envelope,
                        canonical,
                        prepare,
                        committedTarget.AuthorityOwnerGeneration,
                        interruption,
                        cancellationToken)
                    .ConfigureAwait(false);
                sourceTerminalized = true;
                relocationMetric.Complete(
                    ZLinkRelocationMetricOutcome.Completed);
                return ZLinkStandaloneActorRelocationResult.Committed;
            }
            catch (Exception error)
            {
                if (!committed)
                {
                    if (canonical is not null && prepare is not null)
                        canonical.CancelCanonicalRelocation(
                            target.Rid,
                            prepare.RelocationId,
                            prepare.TargetAttemptGeneration,
                            prepare.Coordinator);
                    var authority = await authorityStore.ReadAuthorityAsync(
                            ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                                actorState.ActorId),
                            cancellationToken)
                        .ConfigureAwait(false);
                    var restoredPrecommit = false;
                    if (authority is ZLinkAuthorityReadResult.Found current
                        && ZLinkStandaloneActorRelocationPrecommitCoordinator
                            .IsSourcePrecommit(current.Snapshot, relocationId))
                    {
                        var restored = await precommit.AbortSourceAsync(
                                ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                                    actorState.ActorId),
                                relocationId,
                                cancellationToken)
                            .ConfigureAwait(false);
                        authority = new ZLinkAuthorityReadResult.Found(restored);
                        restoredPrecommit = true;
                    }
                    if (!IsExactSourceAuthority(
                            authority,
                            found.Snapshot,
                            sourceAuthority,
                            requireStoreVersion: !restoredPrecommit))
                    {
                        if (prepared is null
                            || canonical is null
                            || prepare is null
                            || !IsExactCommittedTargetAuthority(
                                authority,
                                found.Snapshot,
                                prepared.Relocation,
                                relocationId,
                                target,
                                prepare.TargetAttemptGeneration,
                                requireActivated: false))
                            throw DataLost(
                                $"Actor '{actorState.ActorId}' authority changed to an unrelated owner during relocation.");

                        // Command 34 can be lost after the target CAS. Continue
                        // the exact committed attempt instead of reopening or
                        // selecting another target.
                        committed = true;
                        var committedTarget =
                            await WaitForCommittedTargetAuthorityAsync(
                                authorityStore,
                                ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                                    actorState.ActorId),
                                found.Snapshot,
                                prepared.Relocation,
                                relocationId,
                                target,
                                prepare.TargetAttemptGeneration,
                                cancellationToken)
                            .ConfigureAwait(false);
                        await CompleteCommittedSourceAsync(
                                actor,
                                actorState,
                                sourceRef.Value,
                                sourceAuthority,
                                found.Snapshot,
                                target,
                                relocationId,
                                acceptedCount,
                                envelope ?? prepared.Envelope,
                                canonical,
                                prepare,
                                committedTarget.AuthorityOwnerGeneration,
                                interruption,
                                cancellationToken)
                            .ConfigureAwait(false);
                        sourceTerminalized = true;
                        relocationMetric.Complete(
                            ZLinkRelocationMetricOutcome.Completed);
                        return ZLinkStandaloneActorRelocationResult.Committed;
                    }
                    var cleanup = new ZLinkRelocationPublicationCoordinator(
                        authorityStore,
                        relocationStore);
                    if (prepared is not null)
                        await cleanup.DiscardPreparedAsync(prepared)
                            .ConfigureAwait(false);
                    if (initialPrepared is not null
                        && (prepared is null
                            || !StringComparer.Ordinal.Equals(
                                initialPrepared.Relocation.Reference,
                                prepared.Relocation.Reference)
                            || initialPrepared.Relocation.ChecksumCrc32c
                            != prepared.Relocation.ChecksumCrc32c))
                        await cleanup.DiscardPreparedAsync(initialPrepared)
                            .ConfigureAwait(false);
                    if (captureStarted)
                        await runtime.RestoreStandaloneActorRelocationSourceAsync(
                                actorState)
                            .ConfigureAwait(false);
                    if (sealedSession is { } session)
                        await AbortSessionRouteBestEffortAsync(
                                actorState.ActorId,
                                session,
                                handoffId,
                                cancellationToken)
                            .ConfigureAwait(false);
                    sourceTerminalized = true;
                }
                relocationMetric.Complete(
                    error is OperationCanceledException
                        && runtime.ShutdownToken.IsCancellationRequested
                        ? ZLinkRelocationMetricOutcome.Shutdown
                        : committed
                            ? ZLinkRelocationMetricOutcome.Failed
                            : error is OperationCanceledException
                                ? ZLinkRelocationMetricOutcome.Shutdown
                                : ZLinkRelocationMetricOutcome.Aborted);
                if (committed
                    || (error is not OperationCanceledException
                        && !ZLinkActorRelocationFailureException
                            .IsRetryableTargetFailure(error)))
                    throw new ZLinkActorRelocationFailureException(
                        committed
                            ? ZLinkFrameworkRelocationReason.RelocationFailed
                            : ZLinkActorRelocationFailureException.MapReason(
                                error),
                        committed
                            ? ZLinkRelocationCommitKnowledge.Committed
                            : ZLinkRelocationCommitKnowledge.NotCommitted,
                        sourceTerminalized,
                        error);
                throw;
            }
        }
    }

    private static bool IsExactSourceAuthority(
        ZLinkAuthorityReadResult current,
        ZLinkAuthoritySnapshot expected,
        ZLinkActorAuthorityPayload expectedPayload,
        bool requireStoreVersion)
    {
        return current is ZLinkAuthorityReadResult.Found found
               && (!requireStoreVersion
                   || found.Snapshot.StoreVersion == expected.StoreVersion)
               && found.Snapshot.ObjectGeneration == expected.ObjectGeneration
               && found.Snapshot.AuthorityOwnerGeneration
               == expected.AuthorityOwnerGeneration
               && found.Snapshot.OwnerId == expected.OwnerId
               && found.Snapshot.OwnerLeaseGeneration
               == expected.OwnerLeaseGeneration
               && ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                   found.Snapshot.Payload.Span,
                   out var payload)
               && payload == expectedPayload;
    }

    internal static bool IsExactCommittedTargetAuthority(
        ZLinkAuthorityReadResult current,
        ZLinkAuthoritySnapshot source,
        ZLinkRelocationStored root,
        Guid relocationId,
        ZLinkMeshNodeDescriptor target,
        ulong targetAttemptGeneration,
        bool requireActivated)
    {
        if (current is not ZLinkAuthorityReadResult.Found found
            || found.Snapshot.ObjectGeneration != source.ObjectGeneration
            || source.AuthorityOwnerGeneration
               is 0 or > long.MaxValue
            || found.Snapshot.AuthorityOwnerGeneration
               is 0 or > long.MaxValue
            || found.Snapshot.AuthorityOwnerGeneration
               <= source.AuthorityOwnerGeneration
            || found.Snapshot.OwnerId != target.OwnerId
            || found.Snapshot.OwnerLeaseGeneration != target.LeaseGeneration)
            return false;

        if (ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                found.Snapshot.Payload.Span,
                out var canonical))
        {
            return canonical.Phase >= (byte)(requireActivated
                       ? ZLinkStandaloneActorCanonicalPhase.Activated
                       : ZLinkStandaloneActorCanonicalPhase.Committed)
                   && canonical.RelocationHigh == ToWireId(relocationId).High
                   && canonical.RelocationLow == ToWireId(relocationId).Low
                   && canonical.TargetAttemptGeneration
                      == targetAttemptGeneration
                   && StringComparer.Ordinal.Equals(
                       canonical.RelocationReference,
                       root.Reference)
                   && canonical.RelocationChecksumCrc32c
                      == root.ChecksumCrc32c
                   && StringComparer.Ordinal.Equals(
                       canonical.TargetOwnerId,
                       target.OwnerId)
                   && canonical.TargetOwnerLeaseGeneration
                      == checked((ulong)target.LeaseGeneration);
        }

        return ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                   found.Snapshot.Payload.Span,
                   out var publication)
               && publication.AggregateId == relocationId
               && StringComparer.Ordinal.Equals(
                   publication.Reference,
                   root.Reference)
               && publication.ChecksumCrc32c == root.ChecksumCrc32c
               && StringComparer.Ordinal.Equals(
                   publication.TargetOwnerId,
                   target.OwnerId)
               && publication.TargetOwnerLeaseGeneration
                  == target.LeaseGeneration;
    }

    private static async ValueTask<ZLinkAuthoritySnapshot>
        WaitForCommittedTargetAuthorityAsync(
        IZLinkLocationRepository authorityStore,
        ZLinkAuthorityKey authorityKey,
        ZLinkAuthoritySnapshot source,
        ZLinkRelocationStored root,
        Guid relocationId,
        ZLinkMeshNodeDescriptor target,
        ulong targetAttemptGeneration,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            var read = await authorityStore.ReadAuthorityAsync(
                    authorityKey,
                    cancellationToken)
                .ConfigureAwait(false);
            if (IsExactCommittedTargetAuthority(
                    read,
                    source,
                    root,
                    relocationId,
                    target,
                    targetAttemptGeneration,
                    requireActivated: true))
                return ((ZLinkAuthorityReadResult.Found)read).Snapshot;
            await Task.Delay(5, cancellationToken).ConfigureAwait(false);
        }
    }

    private async ValueTask CompleteCommittedSourceAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef sourceRef,
        ZLinkActorAuthorityPayload sourceAuthority,
        ZLinkAuthoritySnapshot sourceSnapshot,
        ZLinkMeshNodeDescriptor target,
        Guid relocationId,
        int acceptedCount,
        ZLinkRelocationEnvelope envelope,
        IZLinkBackendCanonicalRelocationReservation canonical,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ulong targetAuthorityOwnerGeneration,
        ZLinkRelocationInterruptionOperation interruption,
        CancellationToken cancellationToken)
    {
        var targetRef = new ZLinkBackendActorRef(
            target.Rid, actorState.ActorId, sourceRef.Generation);
        var targetOwner = new ZLinkLocationOwnerToken(
            prepare.Candidate.OwnerId,
            checked((long)prepare.Candidate.OwnerLeaseGeneration));
        var progress = new ZLinkStandaloneActorRelocationProgressCoordinator(
            registration.Locations.ResolveStore()
            ?? throw new ZLinkConfigurationException(
                "Location Store is not registered."),
            registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered."),
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId,
                prepare.TargetAttemptGeneration,
                prepare.Candidate.NodeRid,
                prepare.Candidate.NodeGeneration,
                targetOwner));
        _ = await progress.AdvancePhaseAsync(
                envelope,
                ZLinkActorRelocationAuthorityPhase.Activated,
                ZLinkActorRelocationAuthorityPhase.Cleaning,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        var trailing = actorState.Handoff.CutoverCaptureToMessageFollow(
            acceptedCount,
            sourceRef,
            targetRef,
            sourceAuthority.MeshName,
            sourceAuthority.NodeGeneration,
            target.LifecycleGeneration,
            sourceSnapshot.AuthorityOwnerGeneration,
            targetAuthorityOwnerGeneration,
            sourceAuthority.OwnerLeaseGeneration,
            checked((ulong)target.LeaseGeneration));
        var trailingDeliveries =
            runtime.RelayStandaloneActorRelocationTrailing(
                actorState, sourceRef, trailing);
        actorState.Handoff.CommitMessageFollow(
            registration.Locations.Options.MessageFollowDuration);
        if (trailingDeliveries.Count != 0
            && (await Task.WhenAll(trailingDeliveries)
                    .ConfigureAwait(false)).Any(static delivered => !delivered))
            throw new ZLinkRelocationDataLostException(
                $"Actor '{actorState.ActorId}' could not deliver its pre-cutover Message Follow backlog.");
        _ = await progress.PublishAdmissionReadyAuthorityAsync(
                envelope,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        await canonical.CompleteCanonicalRelocationAsync(
                target.Rid,
                new ZLinkServiceWireCodec.RelocationCompleteRecord(
                    prepare.RelocationId,
                    prepare.TargetAttemptGeneration,
                    prepare.Coordinator,
                    1,
                    new ZLinkServiceWireCodec.RequestSourceFence(
                        prepare.Coordinator.OwnerId,
                        prepare.Coordinator.LeaseGeneration,
                        prepare.SourceNodeRid,
                        prepare.SourceNodeGeneration),
                    1),
                cancellationToken)
            .ConfigureAwait(false);
        interruption.Complete();
        await runtime.CompleteStandaloneActorRelocationSourceAsync(
                actor,
                actorState,
                sourceRef,
                targetRef,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static TimeSpan RemainingTimeout(DateTimeOffset absoluteDeadline)
    {
        var remaining = absoluteDeadline - DateTimeOffset.UtcNow;
        return remaining > TimeSpan.Zero
            ? remaining
            : TimeSpan.FromTicks(1);
    }

    internal static ZLinkRelocationEnvelope CreateImmutableRoot(
        ZLinkAuthoritySnapshot sourceSnapshot,
        ZLinkActorAuthorityPayload sourceAuthority,
        ZLinkMeshNodeDescriptor target,
        Guid relocationId,
        ReadOnlyMemory<byte> applicationState,
        IReadOnlyList<ZLinkActorAcceptedRecord> acceptedRecords,
        ZLinkRemoteActorBoundSessionRoute boundSessionRoute,
        ReadOnlyMemory<byte> remoteJoinRecovery = default,
        ZLinkObjectMaintenancePolicyKind maintenancePolicy =
            ZLinkObjectMaintenancePolicyKind.Snapshot)
    {
        ArgumentNullException.ThrowIfNull(target);
        return CreateImmutableRoot(
            sourceSnapshot,
            sourceAuthority,
            new ZLinkStandaloneActorRelocationDestination(
                target.EntrySpotId
                ?? throw new ArgumentException(
                    "The target Entry Spot id is required.",
                    nameof(target)),
                target.LifecycleGeneration,
                ZLinkSpotKind.Entry,
                target.Rid,
                target.LifecycleGeneration,
                target.MeshName,
                new ZLinkLocationOwnerToken(
                    target.OwnerId,
                    target.LeaseGeneration)),
            relocationId,
            applicationState,
            acceptedRecords,
            boundSessionRoute,
            remoteJoinRecovery,
            maintenancePolicy);
    }

    private static ZLinkObjectMaintenancePolicyKind ToMaintenancePolicy(
        int policyKind) =>
        policyKind switch
        {
            1 => ZLinkObjectMaintenancePolicyKind.Recreate,
            2 => ZLinkObjectMaintenancePolicyKind.Snapshot,
            _ => throw new ZLinkConfigurationException(
                $"Unknown Actor relocation policy kind '{policyKind}'.")
        };

    internal static ZLinkRelocationEnvelope CreateImmutableRoot(
        ZLinkAuthoritySnapshot sourceSnapshot,
        ZLinkActorAuthorityPayload sourceAuthority,
        ZLinkStandaloneActorRelocationDestination destination,
        Guid relocationId,
        ReadOnlyMemory<byte> applicationState,
        IReadOnlyList<ZLinkActorAcceptedRecord> acceptedRecords,
        ZLinkRemoteActorBoundSessionRoute boundSessionRoute,
        ReadOnlyMemory<byte> remoteJoinRecovery = default,
        ZLinkObjectMaintenancePolicyKind maintenancePolicy =
            ZLinkObjectMaintenancePolicyKind.Snapshot)
    {
        ArgumentNullException.ThrowIfNull(sourceSnapshot);
        ArgumentNullException.ThrowIfNull(sourceAuthority);
        if (relocationId == Guid.Empty
            || sourceSnapshot.ObjectGeneration == 0
            || sourceSnapshot.AuthorityOwnerGeneration == 0
            || sourceSnapshot.OwnerLeaseGeneration <= 0
            || sourceSnapshot.OwnerId != sourceAuthority.OwnerId
            || checked((ulong)sourceSnapshot.OwnerLeaseGeneration)
            != sourceAuthority.OwnerLeaseGeneration
            || sourceSnapshot.Allocation.Descriptor.Rid
            != sourceAuthority.NodeRid
            || sourceSnapshot.Allocation.DescriptorLifecycleGeneration
            != sourceAuthority.NodeGeneration
            || destination.NodeLifecycleGeneration == 0
            || destination.SpotObjectGeneration == 0
            || destination.Owner.LeaseGeneration <= 0
            || string.IsNullOrWhiteSpace(destination.Owner.OwnerId)
            || string.IsNullOrWhiteSpace(destination.SpotId)
            || string.IsNullOrWhiteSpace(destination.MeshName)
            || destination.SpotKind is not (ZLinkSpotKind.Entry or ZLinkSpotKind.User))
            throw new ArgumentOutOfRangeException(nameof(relocationId));

        var targetAuthority = sourceAuthority with
        {
            State = ZLinkActorAuthorityState.Ready,
            CurrentSpotId = destination.SpotId,
            CurrentSpotGeneration = destination.SpotObjectGeneration,
            CurrentSpotKind = destination.SpotKind,
            OwnerId = destination.Owner.OwnerId,
            OwnerLeaseGeneration = checked((ulong)destination.Owner.LeaseGeneration),
            MeshName = destination.MeshName,
            NodeRid = destination.NodeRid,
            NodeGeneration = destination.NodeLifecycleGeneration
        };
        var relocatingAuthority = ZLinkActorRelocationAuthorityPayloadCodec.Encode(
            new ZLinkActorRelocationAuthorityPayload(
                relocationId,
                ZLinkActorRelocationAuthorityPhase.Activated,
                boundSessionRoute,
                ZLinkActorAuthorityPayloadCodec.Encode(targetAuthority),
                checked((uint)acceptedRecords.Count(static accepted =>
                    (accepted.Frame.Flags & 1U) != 0))));
        var sourceFence = new ZLinkServiceWireCodec.RequestSourceFence(
            sourceSnapshot.OwnerId,
            checked((ulong)sourceSnapshot.OwnerLeaseGeneration),
            sourceAuthority.NodeRid,
            sourceAuthority.NodeGeneration);
        var sourceActor = new ZLinkBackendActorRef(
            sourceAuthority.NodeRid,
            sourceAuthority.ActorId,
            sourceSnapshot.ObjectGeneration);
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Encode(
            new ZLinkCanonicalParticipantRecovery(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                    sourceAuthority.ActorId),
                ZLinkPlacementObjectKind.Actor,
                sourceSnapshot.ObjectGeneration,
                sourceSnapshot.AuthorityOwnerGeneration,
                sourceSnapshot.StoreVersion,
                sourceAuthority.StableType,
                relocatingAuthority,
                ZLinkActorRelocationSourceFenceCodec.Encode(
                    new ZLinkActorRelocationSourceFence(
                        sourceFence.OwnerId,
                        sourceFence.LeaseGeneration,
                        sourceFence.NodeRid,
                        sourceFence.NodeGeneration)),
                remoteJoinRecovery,
                maintenancePolicy));
        var participant = new ZLinkRelocationParticipantEnvelope(
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(sourceAuthority.ActorId),
            ZLinkPlacementObjectKind.Actor,
            sourceSnapshot.ObjectGeneration,
            sourceSnapshot.AuthorityOwnerGeneration,
            applicationState,
            acceptedRecords
                .OrderBy(static accepted => accepted.Frame.ArrivalIndex)
                .Select((accepted, index) => new ZLinkRelocationQueuedJob(
                    checked((ulong)index + 1),
                    ZLinkCanonicalActorAcceptedJournal.Encode(
                        accepted,
                        sourceActor)))
                .ToArray(),
            [],
            RecoveryPayload: recovery)
        {
            CanonicalParticipantId = 1,
            AcceptedBoundary = checked((ulong)acceptedRecords.Count)
        };
        var publicationParticipant = new ZLinkAggregateRelocationParticipant(
            participant,
            sourceSnapshot.StoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            relocatingAuthority,
            ReadOnlyMemory<byte>.Empty);
        var digest = ZLinkAggregateInventoryDigest.Compute([publicationParticipant]);
        var relocationBytes = relocationId.ToByteArray(bigEndian: true);
        return new ZLinkRelocationEnvelope(
            relocationId,
            1,
            digest,
            [participant])
        {
            CanonicalRelocationHigh =
                System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(
                    relocationBytes.AsSpan(0, 8)),
            CanonicalRelocationLow =
                System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(
                    relocationBytes.AsSpan(8, 8))
        };
    }

    private static ZLinkStandaloneActorRelocationDestination ResolveDestination(
        ZLinkSpotActivation? sourceActivation,
        ZLinkMeshNodeDescriptor target)
    {
        ArgumentNullException.ThrowIfNull(target);
        if (sourceActivation is null)
            return new ZLinkStandaloneActorRelocationDestination(
                target.EntrySpotId
                ?? throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "The target Entry Spot id is unavailable."),
                target.LifecycleGeneration,
                ZLinkSpotKind.Entry,
                target.Rid,
                target.LifecycleGeneration,
                target.MeshName,
                new ZLinkLocationOwnerToken(
                    target.OwnerId,
                    target.LeaseGeneration));

        if (sourceActivation.ExecutionMode != ZLinkUserSpotExecutionMode.PerActor
            || sourceActivation.PerActorShellRelocationPlan is not { } plan)
            throw new InvalidOperationException(
                "Only a PerActor User Spot with a committed shell relocation plan "
                + "can relocate member Actors independently.");
        if (target.Rid != plan.TargetNodeRid
            || target.LifecycleGeneration != plan.TargetNodeLifecycleGeneration
            || target.OwnerId != plan.TargetOwner.OwnerId
            || target.LeaseGeneration != plan.TargetOwner.LeaseGeneration
            || !StringComparer.Ordinal.Equals(
                target.MeshName,
                sourceActivation.MeshName))
            throw DataLost(
                $"PerActor SPOT '{sourceActivation.SpotId}' Actor target "
                + "does not match its committed shell relocation plan.");

        return new ZLinkStandaloneActorRelocationDestination(
            sourceActivation.SpotId,
            sourceActivation.ObjectGeneration,
            ZLinkSpotKind.User,
            plan.TargetNodeRid,
            plan.TargetNodeLifecycleGeneration,
            sourceActivation.MeshName,
            plan.TargetOwner);
    }

    private async ValueTask<ZLinkActorBoundSession> SealSessionRouteAsync(
        ZLinkActorRuntimeState actorState,
        ZLinkActorBoundSession session,
        string handoffId,
        CancellationToken cancellationToken)
    {
        var sourceRef = actorState.NativeActorRef
                        ?? throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NotFound,
                            $"Actor '{actorState.ActorId}' has no source reference.");
        var sessionNode = session.SessionNodeRid ?? sourceRef.NodeRid;
        var normalized = session with
        {
            SessionNodeRid = sessionNode,
            BindingGeneration = Math.Max(1, session.BindingGeneration),
            ObjectGeneration = sourceRef.Generation,
            AuthorityOwnerGeneration = session.AuthorityOwnerGeneration == 0
                ? throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{actorState.ActorId}' session authority fence is empty.")
                : session.AuthorityOwnerGeneration
        };
        var request = new ZLinkSessionRouteSealRequest(
            actorState.ActorId,
            normalized.BindingToken,
            normalized.BindingGeneration,
            normalized.ObjectGeneration,
            normalized.AuthorityOwnerGeneration,
            normalized.MeshName,
            normalized.TargetNodeGeneration,
            normalized.OwnerLeaseGeneration,
            normalized.SessionOwnerNodeGeneration,
            handoffId);
        ZLinkSessionRouteSealReply reply;
        if (sessionNode == runtime.GetMeshNodeRuntime(normalized.MeshName)
                .Node.RoutingId)
        {
            var result = await runtime.SealSessionActorRouteAsync(
                    new ZLinkSessionRouteSeal(
                        request.ActorId,
                        request.BindingToken,
                        request.BindingGeneration,
                        request.ObjectGeneration,
                        request.AuthorityOwnerGeneration,
                        request.MeshName,
                        request.TargetNodeGeneration,
                        request.OwnerLeaseGeneration,
                        request.SessionOwnerNodeGeneration,
                        request.HandoffId),
                    cancellationToken)
                .ConfigureAwait(false);
            reply = new ZLinkSessionRouteSealReply(
                result.Acknowledged,
                result.AcceptedHighWater);
        }
        else
        {
            reply = await runtime.RequestSessionRouteSealAsync(
                    normalized.MeshName,
                    sessionNode,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        if (!reply.Acknowledged)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorState.ActorId}' session ingress seal was fenced.");
        normalized = normalized with { AcceptedHighWater = reply.AcceptedHighWater };
        actorState.BindSession(
            normalized.SessionNodeRid,
            normalized.SessionRid,
            normalized.BindingToken,
            normalized.BindingGeneration,
            normalized.ObjectGeneration,
            normalized.AuthorityOwnerGeneration,
            normalized.MeshName,
            normalized.TargetNodeGeneration,
            normalized.OwnerLeaseGeneration,
            normalized.SessionOwnerNodeGeneration,
            normalized.AcceptedHighWater);
        return normalized;
    }

    private async ValueTask AbortSessionRouteBestEffortAsync(
        string actorId,
        ZLinkActorBoundSession session,
        string handoffId,
        CancellationToken cancellationToken)
    {
        try
        {
            var seal = new ZLinkSessionRouteSeal(
                actorId,
                session.BindingToken,
                session.BindingGeneration,
                session.ObjectGeneration,
                session.AuthorityOwnerGeneration,
                session.MeshName,
                session.TargetNodeGeneration,
                session.OwnerLeaseGeneration,
                session.SessionOwnerNodeGeneration,
                handoffId);
            var sessionNode = session.SessionNodeRid!.Value;
            if (sessionNode == runtime.GetMeshNodeRuntime(session.MeshName)
                    .Node.RoutingId)
            {
                _ = runtime.AbortSessionActorRouteSeal(seal);
                return;
            }
            _ = await runtime.RequestSessionRouteAbortAsync(
                    session.MeshName,
                    sessionNode,
                    new ZLinkSessionRouteAbortRequest(
                        actorId,
                        session.BindingToken,
                        session.BindingGeneration,
                        session.ObjectGeneration,
                        session.AuthorityOwnerGeneration,
                        session.MeshName,
                        session.TargetNodeGeneration,
                        session.OwnerLeaseGeneration,
                        session.SessionOwnerNodeGeneration,
                        handoffId),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            // A disconnected or replaced exact binding owns its own cleanup.
        }
    }

    private static ZLinkRemoteActorBoundSessionRoute ToRemoteRoute(
        ZLinkActorBoundSession session) => new(
        session.SessionNodeRid,
        session.SessionRid,
        session.BindingToken,
        session.BindingGeneration,
        session.ObjectGeneration,
        session.AuthorityOwnerGeneration,
        session.MeshName,
        session.TargetNodeGeneration,
        session.OwnerLeaseGeneration,
        session.SessionOwnerNodeGeneration,
        session.AcceptedHighWater);

    internal static ZLinkServiceWireCodec.RelocationPrepareRecord CreatePrepare(
        ZLinkAuthoritySnapshot sourceSnapshot,
        ZLinkActorAuthorityPayload sourceAuthority,
        ZLinkMeshNodeDescriptor target,
        ZLinkRelocationEnvelope envelope,
        ZLinkRelocationStored root,
        ZLinkActorBoundSession? boundSession,
        long applicationVersion,
        ulong? negotiatedMessages = null,
        ulong? negotiatedAcceptedBytes = null,
        ulong? negotiatedPayloadBytes = null)
    {
        var relocationId = ToWireId(envelope.AggregateId);
        var participant = envelope.Participants.Single();
        var acceptedBytes = participant.AcceptedJobs.Aggregate(
            0UL,
            static (sum, job) => checked(sum + (ulong)job.Payload.Length));
        var requiredMessages = negotiatedMessages
                               ?? checked((ulong)participant.AcceptedJobs.Count);
        var allowanceBytes = negotiatedAcceptedBytes ?? acceptedBytes;
        var requiredBytes = negotiatedPayloadBytes
                            ?? checked((ulong)ZLinkRelocationEnvelopeCodec
                                .MeasureEncodedLength(envelope));
        if (requiredMessages < checked((ulong)participant.AcceptedJobs.Count)
            || allowanceBytes < acceptedBytes
            || requiredBytes < checked((ulong)ZLinkRelocationEnvelopeCodec
                .MeasureEncodedLength(envelope)))
            throw new ArgumentOutOfRangeException(nameof(negotiatedMessages));
        return new ZLinkServiceWireCodec.RelocationPrepareRecord(
            relocationId,
            1,
            1,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                sourceSnapshot.OwnerId,
                checked((ulong)sourceSnapshot.OwnerLeaseGeneration),
                sourceAuthority.NodeRid,
                sourceAuthority.NodeGeneration,
                sourceSnapshot.StoreVersion),
            new ZLinkServiceWireCodec.RelocationCandidateRecord(
                target.Rid,
                target.LifecycleGeneration,
                target.OwnerId,
                checked((ulong)target.LeaseGeneration)),
            1,
            new ZLinkServiceWireCodec.RelocationObjectRecord(
                1,
                string.Empty,
                sourceAuthority.ActorId,
                sourceSnapshot.ObjectGeneration,
                sourceSnapshot.AuthorityOwnerGeneration),
            sourceAuthority.NodeRid,
            sourceAuthority.NodeGeneration,
            requiredMessages,
            requiredBytes,
            [
                //  Wire 계약(`relocation-participant-identity`)은 participant
                //  kind별 conditional-union이며 `objectMailbox`(1) case의 필드
                //  목록은 비어 있다. Session 신원은 `boundSession`(2) case에만
                //  있다. 따라서 kind=1 record에 session 값을 채워도 인코딩되지
                //  않고, target이 echo한 Reserved에는 그 자리가 비어 돌아온다.
                //  그러면 MatchesReserved의 등가 비교가 거부해 예약이 완료되지
                //  않고 relocation이 deadline까지 간다. 실을 수 없는 값은 싣지
                //  않는다.
                new ZLinkServiceWireCodec.RelocationParticipantRecord(
                    1,
                    1,
                    default,
                    0,
                    null,
                    0,
                    default,
                    0,
                    requiredMessages,
                    allowanceBytes)
            ],
            new ZLinkServiceWireCodec.RelocationRootRecord(
                root.Reference,
                root.ChecksumCrc32c),
            checked((ulong)applicationVersion));
    }

    private static ZLinkServiceWireCodec.RelocationWireId ToWireId(Guid value)
    {
        var id = value.ToByteArray(bigEndian: true);
        return new ZLinkServiceWireCodec.RelocationWireId(
            System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(
                id.AsSpan(0, 8)),
            System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(
                id.AsSpan(8, 8)));
    }

    internal async ValueTask StageTargetAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid,
        ulong targetAuthorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        await SweepExpiredTargetStagesAsync().ConfigureAwait(false);
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!_targetStages.ContainsKey(key)
            && !_targetStageGates.ContainsKey(key)
            && checked(_targetStages.Count + _targetStageGates.Count)
               >= MaximumTargetStages)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "Standalone Actor target staging capacity is exhausted.",
                retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
        var gate = _targetStageGates.GetOrAdd(
            key,
            static _ => new SemaphoreSlim(1, 1));
        await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await StageTargetCoreAsync(
                    prepare,
                    authenticatedSourceNodeRid,
                    targetAuthorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            gate.Release();
        }
    }

    internal async ValueTask ApplyFinalRootAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ZLinkRelocationEnvelope finalEnvelope,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(prepare);
        ArgumentNullException.ThrowIfNull(finalEnvelope);
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        var gate = _targetStageGates.GetOrAdd(
            key,
            static _ => new SemaphoreSlim(1, 1));
        await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!_targetStages.TryGetValue(key, out var stage))
                throw DataLost(
                    "Standalone Actor initial target staging is unavailable.");
            stage.ValidateRetry(prepare, stage.SourceNodeRid);
            if (stage.FinalRootApplied)
            {
                if (stage.Envelope.InventoryDigest.Span
                    .SequenceEqual(finalEnvelope.InventoryDigest.Span)) return;
                throw DataLost(
                    "Standalone Actor final root changed after target verification.");
            }
            if (finalEnvelope.AggregateId != stage.Envelope.AggregateId
                || finalEnvelope.AggregateGeneration
                   != stage.Envelope.AggregateGeneration
                || finalEnvelope.Participants.Count != 1)
                throw DataLost(
                    "Standalone Actor final root changed its aggregate identity.");
            var initialJobs = stage.Envelope.Participants[0].AcceptedJobs
                .OrderBy(static job => job.AcceptedSequence)
                .ToArray();
            var finalJobs = finalEnvelope.Participants[0].AcceptedJobs
                .OrderBy(static job => job.AcceptedSequence)
                .ToArray();
            if (initialJobs.Length > finalJobs.Length)
                throw DataLost(
                    "Standalone Actor final root lost its initial journal.");
            for (var index = 0; index < initialJobs.Length; index++)
                if (initialJobs[index].AcceptedSequence
                        != finalJobs[index].AcceptedSequence
                    || !initialJobs[index].Payload.Span.SequenceEqual(
                        finalJobs[index].Payload.Span))
                    throw DataLost(
                        "Standalone Actor final root changed its initial journal prefix.");
            var allFrames = DecodeAcceptedRecords(finalJobs)
                .Select(static accepted => accepted.Frame)
                .ToArray();
            stage.ActorState.Handoff.AppendCanonicalMaintenanceImport(
                finalEnvelope.AggregateId.ToString("N"),
                allFrames.Skip(initialJobs.Length).ToArray());
            stage.ApplyFinalRoot(finalEnvelope, allFrames);
        }
        finally
        {
            gate.Release();
        }
    }

    private async ValueTask SweepExpiredTargetStagesAsync()
    {
        var cutoff = TimeProvider.System.GetUtcNow() - TargetStageTtl;
        foreach (var pair in _targetStages)
        {
            var stage = pair.Value;
            if (stage.AuthorityPublished
                || stage.CreatedAt > cutoff
                || !_targetStages.TryRemove(pair))
                continue;
            _targetStageGates.TryRemove(pair.Key, out _);
            stage.ReleasePermit();
            stage.ActorState.AbortRelocationSessionRoute(
                stage.Envelope.AggregateId.ToString("N"));
            DetachTargetMembership(stage);
            await actorSessions.RollbackTransferredActorAsync(
                    stage.ActorState.ActorId,
                    CancellationToken.None)
                .ConfigureAwait(false);
        }

        foreach (var pair in _targetStageGates)
            if (!_targetStages.ContainsKey(pair.Key)
                && pair.Value.CurrentCount == 1)
                _targetStageGates.TryRemove(pair);
    }

    internal static bool OwnsRecovery(
        ZLinkRelocationRecoveryCandidate candidate)
    {
        if (candidate.Envelope.Participants.Count != 1
            || candidate.Authorities.Count != 1)
            return false;
        try
        {
            var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                candidate.Envelope.Participants[0].RecoveryPayload.Span);
            return recovery.ObjectKind == ZLinkPlacementObjectKind.Actor;
        }
        catch (Exception exception) when (exception is InvalidDataException
                                          or EndOfStreamException)
        {
            return false;
        }
    }

    internal async ValueTask RecoverPublishedAsync(
        ZLinkRelocationRecoveryCandidate candidate,
        CancellationToken cancellationToken)
    {
        if (!OwnsRecovery(candidate))
            throw DataLost(
                "Standalone Actor recovery received a non-canonical root.");
        var participant = candidate.Envelope.Participants.Single();
        var authority = candidate.Authorities.Single();
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        var sourceFence = ZLinkActorRelocationSourceFenceCodec.Decode(
            recovery.MembershipMutation.Span);
        if (recovery.AuthorityKey != authority.Key
            || recovery.AuthorityKey != participant.AuthorityKey
            || recovery.ObjectGeneration != participant.ObjectGeneration
            || recovery.AuthorityOwnerGeneration
            != participant.AuthorityOwnerGeneration
            || authority.Snapshot.ObjectGeneration
            != participant.ObjectGeneration
            || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                authority.Snapshot.Payload.Span,
                out var publication)
            || publication.AggregateId != candidate.Envelope.AggregateId
            || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                authority.Snapshot.Payload.Span,
                out var canonical)
            || canonical.RelocationReference != candidate.Reference.Reference
            || canonical.RelocationChecksumCrc32c
            != candidate.Reference.ChecksumCrc32c
            || !ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                recovery.AuthorityPayload.Span,
                out var relocating)
            || relocating.RelocationId != candidate.Envelope.AggregateId
            || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                authority.Snapshot.Payload.Span,
                out var targetAuthority)
            || ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                   targetAuthority.ActorId) != recovery.AuthorityKey
            || targetAuthority.StableType != recovery.StableType
            || targetAuthority.OwnerId != authority.Snapshot.OwnerId
            || checked((long)targetAuthority.OwnerLeaseGeneration)
            != authority.Snapshot.OwnerLeaseGeneration)
            throw DataLost(
                "Standalone Actor recovery root does not match its published authority.");

        var sourceOwned = canonical.Phase is 2 or 3;
        if (sourceOwned)
        {
            if (authority.Snapshot.AuthorityOwnerGeneration
                != participant.AuthorityOwnerGeneration
                || !StringComparer.Ordinal.Equals(
                    authority.Snapshot.OwnerId,
                    canonical.State.SourceOwnerId)
                || authority.Snapshot.OwnerLeaseGeneration
                != checked((long)canonical.State.SourceOwnerLeaseGeneration)
                || targetAuthority.NodeRid.ToHex()
                != canonical.State.SourceNodeRid
                || targetAuthority.NodeGeneration
                != canonical.State.SourceNodeGeneration)
                throw DataLost(
                    "Standalone Actor source-owned recovery lost its exact source fence.");
        }
        else if (participant.AuthorityOwnerGeneration
                 is 0 or > long.MaxValue
                 || authority.Snapshot.AuthorityOwnerGeneration
                 is 0 or > long.MaxValue
                 || authority.Snapshot.AuthorityOwnerGeneration
                 <= participant.AuthorityOwnerGeneration)
            throw DataLost(
                "Standalone Actor target-owned recovery did not advance owner generation.");

        if (canonical.SourceCleanupState == 1
            && canonical.Phase
               < (byte)ZLinkStandaloneActorCanonicalPhase.Cleaning)
            throw DataLost(
                "Standalone Actor source cleanup completed before the Cleaning phase.");

        if (canonical.SourceCleanupState == 0
            && canonical.Phase
               >= (byte)ZLinkStandaloneActorCanonicalPhase.Committed)
        {
            var authorityStore = registration.Locations.ResolveStore()
                                 ?? throw new ZLinkConfigurationException(
                                     "Location Store is not registered.");
            var sourceLease = await authorityStore.ReadOwnerLeaseAsync(
                    sourceFence.OwnerId,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!IsExactSourceLeaseExpired(sourceLease, sourceFence))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{targetAuthority.ActorId}' source cleanup is not yet durable.",
                    retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
        }

        var targetAttemptGeneration = canonical.TargetAttemptGeneration;
        var localNode = runtime.TryGetSpotNodeRuntime(targetAuthority.NodeRid);
        var hasCurrentLocalTarget = localNode is not null
            && localNode.Node.MeshStatus().LifecycleGeneration
            == targetAuthority.NodeGeneration;
        ZLinkActorRuntimeState actorState;
        ZLinkObjectRelocationRegistration relocation;
        if (sourceOwned || !hasCurrentLocalTarget)
        {
            var takeoverCoordinator =
                new ZLinkStandaloneActorRelocationTakeoverCoordinator(
                    runtime, actorSessions, registration);
            if (await takeoverCoordinator.HasLiveRemoteRecoveryOwnerAsync(
                    canonical,
                    targetAuthority.MeshName,
                    cancellationToken)
                .ConfigureAwait(false))
                return;
            var takeover = await takeoverCoordinator
                .TakeOverAsync(
                    authority,
                    canonical,
                    candidate.Envelope,
                    participant,
                    recovery,
                    cancellationToken)
                .ConfigureAwait(false);
            authority = authority with { Snapshot = takeover.Authority };
            targetAuthority = takeover.TargetAuthority;
            actorState = takeover.ActorState;
            relocation = takeover.Relocation;
            targetAttemptGeneration = takeover.TargetAttemptGeneration;
            RetainRecoveryPermit(
                new AttemptKey(
                    candidate.Envelope.CanonicalRelocationHigh,
                    candidate.Envelope.CanonicalRelocationLow,
                    targetAttemptGeneration),
                takeover.Permit);
        }
        else
        {
            if (!localNode!.Registration.ActorRelocations.TryGetValue(
                    recovery.StableType,
                    out var registeredRelocation)
                || registeredRelocation.PolicyKind == 0)
                throw DataLost(
                    $"Actor type '{recovery.StableType}' cannot restore the published relocation.");
            relocation = registeredRelocation;
            var recoveryKey = new AttemptKey(
                candidate.Envelope.CanonicalRelocationHigh,
                candidate.Envelope.CanonicalRelocationLow,
                targetAttemptGeneration);
            if (!_recoveryPermits.ContainsKey(recoveryKey))
            {
                if (!runtime.RelocationPermits.TryAcquire(
                        ZLinkRelocationPermitRequest.Inbound(
                            ZLinkRelocationEnvelopeCodec.MeasureEncodedLength(
                                candidate.Envelope),
                            restore: relocation.PolicyKind == 2),
                        out var recoveryPermit))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"Actor '{targetAuthority.ActorId}' recovery admission is busy.",
                        retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
                RetainRecoveryPermit(recoveryKey, recoveryPermit);
            }

            actorState = actorSessions.GetOrCreateState(targetAuthority.ActorId);
            if (actorState.Actor is null)
            {
                await actorSessions.PrepareForTransferredActivationAsync(
                        actorState,
                        cancellationToken)
                    .ConfigureAwait(false);
                await actorSessions.RelocateAndBindActorAsync(
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
                        authority.Snapshot.AuthorityOwnerGeneration,
                        ZLinkActorClaimMode.StagedRelocation,
                        publishActorRef: false,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            else if (!StringComparer.Ordinal.Equals(
                         actorState.ActorType,
                         targetAuthority.StableType)
                     || actorState.NativeActorRef is not { } existingActor
                     || existingActor.NodeRid != targetAuthority.NodeRid
                     || existingActor.Generation != participant.ObjectGeneration)
            {
                throw DataLost(
                    "Standalone Actor recovery found a different active local instance.");
            }
        }

        var handoffId = candidate.Envelope.AggregateId.ToString("N");
        if (actorState.Handoff.IsKnown(handoffId)
            && !actorState.Handoff.IsCanonicalMaintenanceHandoff(handoffId))
        {
            // A regular remote actor handoff already owns this target state.
            // Standalone recovery must not start a second canonical replay for
            // the same journal; the regular completion path owns its replies
            // and source cleanup.
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"standalone_recovery_deferred actor={actorState.ActorId} "
                + $"handoff={handoffId} reason=standard_handoff_active");
            return;
        }
        actorState.StageRelocationSessionRoute(
            handoffId,
            relocating.BoundSessionRoute);
        var frames = DecodeAcceptedRecords(participant.AcceptedJobs)
            .Select(static accepted => accepted.Frame)
            .ToArray();
        actorState.Handoff.BeginCanonicalMaintenanceImport(
            handoffId,
            frames,
            Math.Max(1, frames.Length),
            Math.Max(1L, frames.Aggregate(
                0L,
                static (sum, frame) => checked(
                    sum + frame.CanonicalEncodedLength))));
        var actorRef = actorState.NativeActorRef
                       ?? throw DataLost(
                           "Standalone Actor recovery did not create a target reference.");
        actorState.MarkRelocationSessionAuthorityCommitted(
            handoffId,
            actorRef,
            authority.Snapshot.AuthorityOwnerGeneration,
            targetAuthority.MeshName,
            targetAuthority.NodeGeneration,
            targetAuthority.OwnerLeaseGeneration);
        if (!actorState.Handoff.IsAuthorityCommitted(handoffId))
            actorState.Handoff.MarkAuthorityCommitted(
                handoffId,
                participant.ObjectGeneration,
                actorRef.Generation);

        var targetOwner = runtime.LocationLifecycle?.OwnerToken
                          ?? throw new ZLinkConfigurationException(
                              "Standalone Actor recovery requires a Location owner lease.");
        var progress = new ZLinkStandaloneActorRelocationProgressCoordinator(
            registration.Locations.ResolveStore()
            ?? throw new ZLinkConfigurationException(
                "Location Store is not registered."),
            registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered."),
            new ZLinkStandaloneActorRelocationTargetFence(
                candidate.Envelope.AggregateId,
                targetAttemptGeneration,
                targetAuthority.NodeRid,
                targetAuthority.NodeGeneration,
                targetOwner));
        var durable = await progress.ReadAsync(
                candidate.Envelope,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        if (durable.Phase.RelocationId != candidate.Envelope.AggregateId)
            throw DataLost(
                "Standalone Actor recovery lost its relocation phase.");
        var canonicalPhase = durable.Canonical?.Phase
                             ?? throw DataLost(
                                 "Standalone Actor recovery lost its canonical phase.");
        if (canonicalPhase
            == (byte)ZLinkStandaloneActorCanonicalPhase.Committed)
        {
            durable = await progress.AdvanceCanonicalPhaseAsync(
                    candidate.Envelope,
                    ZLinkStandaloneActorCanonicalPhase.Committed,
                    ZLinkStandaloneActorCanonicalPhase.Activating,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
            canonicalPhase = durable.Canonical!.Phase;
        }
        if (canonicalPhase
            == (byte)ZLinkStandaloneActorCanonicalPhase.Activating)
        {
            await runtime.ActivateStandaloneActorRelocationTargetAsync(
                    actorState,
                    targetAuthority,
                    targetAttemptGeneration,
                    candidate.Envelope,
                    handoffId,
                    cancellationToken)
                .ConfigureAwait(false);
            durable = await progress.AdvanceCanonicalPhaseAsync(
                    candidate.Envelope,
                    ZLinkStandaloneActorCanonicalPhase.Activating,
                    ZLinkStandaloneActorCanonicalPhase.Activated,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
            canonicalPhase = durable.Canonical!.Phase;
        }
        else if (canonicalPhase is >=
                 (byte)ZLinkStandaloneActorCanonicalPhase.Activated and <=
                 (byte)ZLinkStandaloneActorCanonicalPhase.Completed)
        {
            // A restarted target rebuilds its local actor and replays only the
            // durable suffix before it can finish a later phase.
            await runtime.ActivateStandaloneActorRelocationTargetAsync(
                    actorState,
                    targetAuthority,
                    targetAttemptGeneration,
                    candidate.Envelope,
                    handoffId,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        else
        {
            throw DataLost(
                "Standalone Actor recovery phase is not target-owned.");
        }

        durable = await progress.ReadAsync(
                candidate.Envelope,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        canonicalPhase = durable.Canonical!.Phase;
        if (durable.Canonical.SourceCleanupState == 0
            && canonicalPhase >=
               (byte)ZLinkStandaloneActorCanonicalPhase.Activated)
        {
            var authorityStore = registration.Locations.ResolveStore()
                                 ?? throw new ZLinkConfigurationException(
                                     "Location Store is not registered.");
            var sourceLease = await authorityStore.ReadOwnerLeaseAsync(
                    sourceFence.OwnerId,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!IsExactSourceLeaseExpired(sourceLease, sourceFence))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{targetAuthority.ActorId}' source cleanup is not yet durable.",
                    retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
            if (canonicalPhase
                == (byte)ZLinkStandaloneActorCanonicalPhase.Activated)
            {
                durable = await progress.AdvanceCanonicalPhaseAsync(
                        candidate.Envelope,
                        ZLinkStandaloneActorCanonicalPhase.Activated,
                        ZLinkStandaloneActorCanonicalPhase.Cleaning,
                        targetOwner,
                        cancellationToken)
                    .ConfigureAwait(false);
                canonicalPhase = durable.Canonical!.Phase;
            }
            durable = await progress.PublishAdmissionReadyAuthorityAsync(
                    candidate.Envelope,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        else if (durable.Canonical.SourceCleanupState == 1
                 && canonicalPhase
                 == (byte)ZLinkStandaloneActorCanonicalPhase.Activated)
        {
            durable = await progress.AdvanceCanonicalPhaseAsync(
                    candidate.Envelope,
                    ZLinkStandaloneActorCanonicalPhase.Activated,
                    ZLinkStandaloneActorCanonicalPhase.Cleaning,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
            canonicalPhase = durable.Canonical!.Phase;
        }
        if (canonicalPhase
            == (byte)ZLinkStandaloneActorCanonicalPhase.Activated)
            return;
        if (canonicalPhase is not (
                (byte)ZLinkStandaloneActorCanonicalPhase.Cleaning
                or (byte)ZLinkStandaloneActorCanonicalPhase.Completed))
            throw DataLost(
                "Standalone Actor recovery cannot finalize its current phase.");

        var recoveryAttempt = new AttemptKey(
            candidate.Envelope.CanonicalRelocationHigh,
            candidate.Envelope.CanonicalRelocationLow,
            targetAttemptGeneration);
        try
        {
            await runtime.CompleteStandaloneActorRelocationTargetAsync(
                    actorState,
                    targetAuthority,
                    targetAttemptGeneration,
                    candidate.Envelope,
                    handoffId,
                    cancellationToken,
                    normalizeSteady:
                    recovery.OperationRecovery.IsEmpty
                    && sourceFence.LegacyRemoteJoinRecovery.IsEmpty)
                .ConfigureAwait(false);
        }
        catch
        {
            var current = await registration.Locations.ResolveStore()!
                .ReadAuthorityAsync(
                    participant.AuthorityKey,
                    CancellationToken.None)
                .ConfigureAwait(false);
            if (current is ZLinkAuthorityReadResult.Found found
                && !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out _))
                ReleaseRecoveryPermit(recoveryAttempt);
            throw;
        }
        ReleaseRecoveryPermit(recoveryAttempt);
    }

    private void RetainRecoveryPermit(
        AttemptKey key,
        ZLinkRelocationPermitPool.ZLinkRelocationPermitLease permit)
    {
        if (!_recoveryPermits.TryAdd(key, permit))
            permit.Dispose();
    }

    private void ReleaseRecoveryPermit(AttemptKey key)
    {
        if (_recoveryPermits.TryRemove(key, out var permit))
            permit.Dispose();
    }

    internal void ReleaseRetainedPermits()
    {
        foreach (var pair in _targetStages.ToArray())
            if (_targetStages.TryRemove(pair))
                pair.Value.ReleasePermit();
        _targetStageGates.Clear();
        foreach (var pair in _recoveryPermits.ToArray())
            if (_recoveryPermits.TryRemove(pair.Key, out var permit))
                permit.Dispose();
    }

    private async ValueTask StageTargetCoreAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid,
        ulong targetAuthorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        ValidatePrepare(prepare, authenticatedSourceNodeRid);
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (_targetStages.TryGetValue(key, out var existing))
        {
            existing.ValidateRetry(prepare, authenticatedSourceNodeRid);
            existing.ValidateTargetAuthorityOwnerGeneration(
                targetAuthorityOwnerGeneration);
            return;
        }

        var root = prepare.Root
                   ?? throw DataLost("Standalone Actor relocation has no immutable root.");
        var relocationStore = registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Relocation Store is not registered.");
        var tree = await ZLinkRelocationTreeStore.ReadAsync(
                relocationStore,
                root.Reference,
                root.ChecksumCrc32c,
                cancellationToken)
            .ConfigureAwait(false);
        var participant = tree.Envelope.Participants.SingleOrDefault()
                          ?? throw DataLost(
                              "Standalone Actor relocation must contain one participant.");
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        if (ToWireId(tree.Envelope.AggregateId) != prepare.RelocationId
            || tree.Envelope.AggregateGeneration != 1
            || participant.ObjectKind != ZLinkPlacementObjectKind.Actor
            || recovery.ObjectKind != ZLinkPlacementObjectKind.Actor
            || recovery.AuthorityKey != participant.AuthorityKey
            || recovery.ObjectGeneration != participant.ObjectGeneration
            || recovery.AuthorityOwnerGeneration
               != participant.AuthorityOwnerGeneration
            || ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                   prepare.Object.ObjectId) != recovery.AuthorityKey
            || prepare.Object.ObjectGeneration != participant.ObjectGeneration
            || prepare.Object.ExpectedAuthorityOwnerGeneration
               != participant.AuthorityOwnerGeneration
            || !ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                recovery.AuthorityPayload.Span,
                out var relocating)
            || relocating.RelocationId != tree.Envelope.AggregateId
            || relocating.Phase != ZLinkActorRelocationAuthorityPhase.Activated
            || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                relocating.ApplicationPayload.Span,
                out var targetAuthority)
            || targetAuthority.ActorId != prepare.Object.ObjectId
            || targetAuthority.StableType != recovery.StableType
            || targetAuthority.NodeRid != prepare.Candidate.NodeRid
            || targetAuthority.NodeGeneration != prepare.Candidate.NodeGeneration
            || targetAuthority.OwnerId != prepare.Candidate.OwnerId
            || targetAuthority.OwnerLeaseGeneration
               != prepare.Candidate.OwnerLeaseGeneration)
            throw DataLost(
                "Standalone Actor relocation root does not match command 40.");
        if (targetAuthorityOwnerGeneration is 0 or > long.MaxValue
            || targetAuthorityOwnerGeneration
               <= participant.AuthorityOwnerGeneration)
            throw DataLost(
                "Standalone Actor reservation returned an invalid target authority generation.");

        var node = runtime.GetSpotNodeRuntime(prepare.Candidate.NodeRid);
        var targetActivation = ResolveTargetMembership(node, targetAuthority);
        if (!node.Registration.ActorRelocations.TryGetValue(
                recovery.StableType,
                out var relocation)
            || relocation.PolicyKind == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Actor type '{recovery.StableType}' relocation is not registered on the target.");

        var actorState = actorSessions.GetOrCreateState(targetAuthority.ActorId);
        await actorSessions.PrepareForTransferredActivationAsync(
                actorState,
                cancellationToken)
            .ConfigureAwait(false);
        var created = false;
        var attached = false;
        try
        {
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
            if (targetActivation is not null)
            {
                targetActivation.StageRelocatedPerActorMember(
                    creation.Actor,
                    actorState);
                attached = true;
            }
            actorState.StageRelocationSessionRoute(
                tree.Envelope.AggregateId.ToString("N"),
                relocating.BoundSessionRoute);
            var frames = DecodeAcceptedRecords(participant.AcceptedJobs)
                .Select(static accepted => accepted.Frame)
                .ToArray();
            actorState.Handoff.BeginCanonicalMaintenanceImport(
                tree.Envelope.AggregateId.ToString("N"),
                frames,
                Math.Max(1, checked((int)prepare.Participants[0]
                    .AllowanceMessages)),
                Math.Max(1L, checked((long)prepare.Participants[0]
                    .AllowanceBytes)));
            var stage = new TargetStage(
                prepare,
                authenticatedSourceNodeRid,
                tree.Envelope,
                actorState,
                targetAuthority,
                frames,
                targetAuthorityOwnerGeneration,
                targetActivation);
            if (!_targetStages.TryAdd(key, stage))
            {
                var winner = _targetStages[key];
                winner.ValidateRetry(prepare, authenticatedSourceNodeRid);
                if (attached && actorState.Actor is { } attachedActor)
                    targetActivation!.AbortStagedRelocatedPerActorMember(
                        attachedActor,
                        actorState);
                await actorSessions.RollbackTransferredActorAsync(
                        targetAuthority.ActorId,
                        CancellationToken.None)
                    .ConfigureAwait(false);
            }
        }
        catch
        {
            if (attached && actorState.Actor is { } attachedActor)
                targetActivation!.AbortStagedRelocatedPerActorMember(
                    attachedActor,
                    actorState);
            if (created)
                await actorSessions.RollbackTransferredActorAsync(
                        targetAuthority.ActorId,
                        CancellationToken.None)
                    .ConfigureAwait(false);
            throw;
        }
    }

    internal static ZLinkSpotActivation? ResolveTargetMembership(
        ZLinkSpotNodeRuntime node,
        ZLinkActorAuthorityPayload targetAuthority)
    {
        var owner = new ZLinkLocationOwnerToken(
            targetAuthority.OwnerId,
            targetAuthority.OwnerLeaseGeneration);
        if (targetAuthority.CurrentSpotKind == ZLinkSpotKind.Entry)
        {
            if (!StringComparer.Ordinal.Equals(
                    targetAuthority.CurrentSpotId,
                    node.EntrySpotId)
                || targetAuthority.CurrentSpotGeneration
                   != targetAuthority.NodeGeneration)
                throw DataLost(
                    "Standalone Actor Entry Spot destination does not match "
                    + "the exact target node generation.");
            return null;
        }

        if (targetAuthority.CurrentSpotKind != ZLinkSpotKind.User
            || !node.Catalog.TryGetPerActorRelocationShell(
                targetAuthority.CurrentSpotId,
                targetAuthority.CurrentSpotGeneration,
                targetAuthority.NodeRid,
                targetAuthority.NodeGeneration,
                owner,
                out var activation))
            throw DataLost(
                "Standalone Actor PerActor destination does not match an "
                + "exact published target shell.");
        return activation;
    }

    internal void MarkAuthorityPublished(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!_targetStages.TryGetValue(key, out var stage))
            throw DataLost("Standalone Actor target staging is unavailable.");
        if (stage.AuthorityPublished) return;
        var actorRef = stage.ActorState.NativeActorRef
                       ?? throw DataLost(
                           "Standalone Actor target native reference is unavailable.");
        stage.ActorState.MarkRelocationSessionAuthorityCommitted(
            stage.Envelope.AggregateId.ToString("N"),
            actorRef,
            stage.TargetAuthorityOwnerGeneration,
            stage.TargetAuthority.MeshName,
            stage.TargetAuthority.NodeGeneration,
            stage.TargetAuthority.OwnerLeaseGeneration);
        if (!stage.ActorState.Handoff.IsAuthorityCommitted(
                stage.Envelope.AggregateId.ToString("N")))
            stage.ActorState.Handoff.MarkAuthorityCommitted(
                stage.Envelope.AggregateId.ToString("N"),
                stage.Participant.ObjectGeneration,
                actorRef.Generation);
        if (stage.TargetActivation is { } activation
            && stage.ActorState.Actor is { } actor)
            activation.PublishRelocatedPerActorMember(
                actor,
                stage.ActorState);
        stage.AuthorityPublished = true;
    }

    internal bool IsAuthorityPublished(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        return _targetStages.TryGetValue(key, out var stage)
               && stage.AuthorityPublished;
    }

    internal void RetainTargetPermit(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ZLinkRelocationPermitPool.ZLinkRelocationPermitLease permit)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!_targetStages.TryGetValue(key, out var stage)
            || !stage.AuthorityPublished)
            throw DataLost(
                "Standalone Actor target permit lost its published stage.");
        stage.RetainPermit(permit);
    }

    internal async ValueTask ActivatePublishedTargetAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        CancellationToken cancellationToken)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!_targetStages.TryGetValue(key, out var stage)
            || !stage.AuthorityPublished)
            throw DataLost(
                "Standalone Actor target activation lost its published stage.");
        if (!await IsCurrentTargetAttemptAsync(stage, cancellationToken)
                .ConfigureAwait(false))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "A stale standalone Actor target attempt cannot activate.",
                retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);

        var targetOwner = new ZLinkLocationOwnerToken(
            stage.TargetAuthority.OwnerId,
            checked((long)stage.TargetAuthority.OwnerLeaseGeneration));
        var progress = new ZLinkStandaloneActorRelocationProgressCoordinator(
            registration.Locations.ResolveStore()
            ?? throw new ZLinkConfigurationException(
                "Location Store is not registered."),
            registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Relocation Store is not registered."),
            new ZLinkStandaloneActorRelocationTargetFence(
                stage.Envelope.AggregateId,
                prepare.TargetAttemptGeneration,
                stage.TargetAuthority.NodeRid,
                stage.TargetAuthority.NodeGeneration,
                targetOwner));
        var durable = await progress.ReadAsync(
                stage.Envelope,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        if (durable.Canonical?.Phase
            == (byte)ZLinkStandaloneActorCanonicalPhase.Committed)
            durable = await progress.AdvanceCanonicalPhaseAsync(
                    stage.Envelope,
                    ZLinkStandaloneActorCanonicalPhase.Committed,
                    ZLinkStandaloneActorCanonicalPhase.Activating,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
        if (durable.Canonical?.Phase
            == (byte)ZLinkStandaloneActorCanonicalPhase.Activating)
        {
            await runtime.ActivateStandaloneActorRelocationTargetAsync(
                    stage.ActorState,
                    stage.TargetAuthority,
                    stage.Prepare.TargetAttemptGeneration,
                    stage.Envelope,
                    stage.Envelope.AggregateId.ToString("N"),
                    cancellationToken)
                .ConfigureAwait(false);
            _ = await progress.AdvanceCanonicalPhaseAsync(
                    stage.Envelope,
                    ZLinkStandaloneActorCanonicalPhase.Activating,
                    ZLinkStandaloneActorCanonicalPhase.Activated,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        else if (durable.Canonical?.Phase
                 != (byte)ZLinkStandaloneActorCanonicalPhase.Activated)
        {
            throw DataLost(
                "Standalone Actor target activation phase is not recoverable.");
        }
    }

    internal async ValueTask CompleteTargetAsync(
        ZLinkServiceWireCodec.RelocationCompleteRecord complete,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken)
    {
        var key = new AttemptKey(
            complete.RelocationId.High,
            complete.RelocationId.Low,
            complete.TargetAttemptGeneration);
        if (!_targetStages.TryGetValue(key, out var stage))
        {
            //  조용한 return이라 완료가 통째로 생략돼도 흔적이 없다.
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                "relocation_target_complete_skipped reason=no_stage");
            return;
        }
        if (stage.SourceNodeRid != authenticatedSourceNodeRid
            || complete.Coordinator != stage.Prepare.Coordinator
            || !stage.AuthorityPublished)
            throw DataLost("Standalone Actor completion fence changed.");
        if (!await IsCurrentTargetAttemptAsync(stage, cancellationToken)
                .ConfigureAwait(false))
        {
            RemoveTargetStage(key, stage);
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "A stale standalone Actor target attempt cannot publish completion.",
                retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
        }

        var handoffId = stage.Envelope.AggregateId.ToString("N");
        await runtime.CompleteStandaloneActorRelocationTargetAsync(
                stage.ActorState,
                stage.TargetAuthority,
                stage.Prepare.TargetAttemptGeneration,
                stage.Envelope,
                handoffId,
                cancellationToken)
            .ConfigureAwait(false);
        RemoveTargetStage(key, stage);
    }

    internal async ValueTask ReconcilePublishedTargetAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        CancellationToken cancellationToken)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!_targetStages.TryGetValue(key, out var stage)
            || !stage.AuthorityPublished)
            return;
        var store = registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        var read = await store.ReadAuthorityAsync(
                stage.Participant.AuthorityKey,
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found)
            throw DataLost(
                "Standalone Actor target reconciliation lost its authority.");
        if (IsExactSteadyTarget(
                found.Snapshot,
                stage.Participant.ObjectGeneration,
                stage.TargetAuthorityOwnerGeneration,
                stage.TargetAuthority))
        {
            //  Authority가 steady여도 bound session route는 아직 봉인돼 있을 수
            //  있다. 여기서 stage를 지우면 뒤늦게 도착하는 완료 명령은 stage를
            //  못 찾고 조용히 반환하므로, 그 seal을 풀 주체가 사라진다.
            await runtime.FinishRelocationTargetAsync(
                    stage.ActorState,
                    stage.TargetAuthority.MeshName,
                    stage.Envelope.AggregateId.ToString("N"),
                    cancellationToken)
                .ConfigureAwait(false);
            RemoveTargetStage(key, stage);
            return;
        }
        if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var publication)
            || publication.AggregateId != stage.Envelope.AggregateId
            || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                found.Snapshot.Payload.Span,
                out var canonical)
            || !ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                publication.ApplicationPayload.Span,
                out var relocating))
            throw DataLost(
                "Standalone Actor target reconciliation lost its authority.");
        if (!ZLinkStandaloneActorRelocationTakeoverCoordinator.IsCurrentAttempt(
                found.Snapshot,
                stage.Envelope.AggregateId,
                stage.Prepare.TargetAttemptGeneration,
                stage.TargetAuthority))
        {
            RemoveTargetStage(key, stage);
            return;
        }
        if (relocating.Phase == ZLinkActorRelocationAuthorityPhase.Steady)
        {
            RemoveTargetStage(key, stage);
            return;
        }
        if (canonical.Phase is (
                (byte)ZLinkStandaloneActorCanonicalPhase.Committed
                or (byte)ZLinkStandaloneActorCanonicalPhase.Activating))
        {
            await ActivatePublishedTargetAsync(prepare, cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        if (canonical.Phase
            == (byte)ZLinkStandaloneActorCanonicalPhase.Activated)
            return;
        if (canonical.Phase is not (
                (byte)ZLinkStandaloneActorCanonicalPhase.Cleaning
                or (byte)ZLinkStandaloneActorCanonicalPhase.Completed))
            throw DataLost(
                "Standalone Actor target reconciliation phase is invalid.");

        await runtime.CompleteStandaloneActorRelocationTargetAsync(
                stage.ActorState,
                stage.TargetAuthority,
                stage.Prepare.TargetAttemptGeneration,
                stage.Envelope,
                stage.Envelope.AggregateId.ToString("N"),
                cancellationToken)
            .ConfigureAwait(false);
        RemoveTargetStage(key, stage);
    }

    internal static bool IsExactSteadyTarget(
        ZLinkAuthoritySnapshot snapshot,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        ZLinkActorAuthorityPayload targetAuthority)
    {
        if (snapshot.ObjectGeneration != objectGeneration
            || snapshot.AuthorityOwnerGeneration != authorityOwnerGeneration
            || !StringComparer.Ordinal.Equals(
                snapshot.OwnerId,
                targetAuthority.OwnerId)
            || snapshot.OwnerLeaseGeneration
               != checked((long)targetAuthority.OwnerLeaseGeneration)
            || !ZLinkActorAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var steady))
            return false;
        return steady == targetAuthority;
    }

    internal async ValueTask AbortTargetAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!_targetStages.TryGetValue(key, out var stage)
            || stage.AuthorityPublished
            || !_targetStages.TryRemove(
                new KeyValuePair<AttemptKey, TargetStage>(key, stage)))
            return;
        _targetStageGates.TryRemove(key, out _);
        stage.ReleasePermit();
        stage.ActorState.AbortRelocationSessionRoute(
            stage.Envelope.AggregateId.ToString("N"));
        DetachTargetMembership(stage);
        await actorSessions.RollbackTransferredActorAsync(
                stage.ActorState.ActorId,
                CancellationToken.None)
            .ConfigureAwait(false);
    }

    private static void DetachTargetMembership(TargetStage stage)
    {
        if (stage.TargetActivation is not { } activation
            || stage.ActorState.Actor is not { } actor)
            return;
        activation.AbortStagedRelocatedPerActorMember(
            actor,
            stage.ActorState);
    }

    private void RemoveTargetStage(AttemptKey key, TargetStage stage)
    {
        if (!_targetStages.TryRemove(
                new KeyValuePair<AttemptKey, TargetStage>(key, stage)))
            return;
        _targetStageGates.TryRemove(key, out _);
        stage.ReleasePermit();
    }

    private async ValueTask<bool> IsCurrentTargetAttemptAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        var store = registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        var read = await store.ReadAuthorityAsync(
                stage.Participant.AuthorityKey,
                cancellationToken)
            .ConfigureAwait(false);
        return read is ZLinkAuthorityReadResult.Found found
               && ZLinkStandaloneActorRelocationTakeoverCoordinator
                   .IsCurrentAttempt(
                       found.Snapshot,
                       stage.Envelope.AggregateId,
                       stage.Prepare.TargetAttemptGeneration,
                       stage.TargetAuthority);
    }

    internal static ZLinkActorAcceptedRecord[] DecodeAcceptedRecords(
        IReadOnlyList<ZLinkRelocationQueuedJob> jobs)
    {
        try
        {
            return jobs.OrderBy(static job => job.AcceptedSequence)
                .Select(job => ZLinkCanonicalActorAcceptedJournal.Decode(
                        job.Payload.Span,
                        checked((long)job.AcceptedSequence))
                    .Accepted)
                .ToArray();
        }
        catch (Exception exception) when (exception is InvalidDataException
                                          or OverflowException)
        {
            throw DataLost(
                "Standalone Actor accepted queue is malformed.",
                exception);
        }
    }

    internal static IReadOnlyList<ZLinkActorAcceptedRecord>
        CreateAcceptedRecords(IReadOnlyList<ZLinkActorHandoffFrame> frames)
    {
        ArgumentNullException.ThrowIfNull(frames);
        if (frames.Count == 0) return [];
        var accepted = new ZLinkActorAcceptedRecord[frames.Count];
        for (var index = 0; index < frames.Count; index++)
        {
            var frame = frames[index];
            if (frame.RequestSource is not { } requestSource
                || requestSource.NodeRid.IsEmpty
                || requestSource.NodeGeneration == 0
                || requestSource.LeaseGeneration == 0
                || string.IsNullOrWhiteSpace(requestSource.OwnerId)
                || frame.SourceNodeGeneration != requestSource.NodeGeneration
                || !frame.SourceNodeRid.AsSpan().SequenceEqual(
                    requestSource.NodeRid.ToBytes()))
                throw DataLost(
                    "Standalone Actor accepted request lost its ingress source fence.");
            accepted[index] = new ZLinkActorAcceptedRecord(frame, requestSource);
        }
        return accepted;
    }

    internal static bool IsExactSourceLeaseExpired(
        ZLinkOwnerLeaseReadResult lease,
        ZLinkActorRelocationSourceFence source) => lease switch
        {
            ZLinkOwnerLeaseReadResult.Missing => true,
            ZLinkOwnerLeaseReadResult.Found found =>
                source.OwnerLeaseGeneration > long.MaxValue
                || !StringComparer.Ordinal.Equals(
                    found.Token.OwnerId,
                    source.OwnerId)
                || found.Token.LeaseGeneration
                   != checked((long)source.OwnerLeaseGeneration)
                || found.LeaseExpiresAt <= found.StoreNow,
            _ => false
        };

    private static void ValidatePrepare(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid)
    {
        if (prepare.Object.Kind != 1
            || prepare.RoundKind != 1
            || prepare.TargetAttemptGeneration == 0
            || prepare.SourceNodeRid != authenticatedSourceNodeRid
            || prepare.Coordinator.NodeRid != authenticatedSourceNodeRid
            || prepare.Participants.Count != 1
            || prepare.Participants[0].ParticipantId != 1)
            throw DataLost("Standalone Actor command 40 fence is invalid.");
    }

    private static ZLinkFrameworkException DataLost(
        string message,
        Exception? inner = null) => new(
        ZLinkFrameworkErrorKind.DataLost,
        message,
        retryAdvice: ZLinkRetryAdvice.DoNotRetry,
        inner);

    private readonly record struct AttemptKey(
        ulong RelocationHigh,
        ulong RelocationLow,
        ulong AttemptGeneration);

    private sealed class TargetStage(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId sourceNodeRid,
        ZLinkRelocationEnvelope envelope,
        ZLinkActorRuntimeState actorState,
        ZLinkActorAuthorityPayload targetAuthority,
        IReadOnlyList<ZLinkActorHandoffFrame> acceptedFrames,
        ulong targetAuthorityOwnerGeneration,
        ZLinkSpotActivation? targetActivation)
    {
        private readonly object _permitGate = new();
        private ZLinkRelocationPermitPool.ZLinkRelocationPermitLease _permit;
        private bool _permitRetained;
        internal ZLinkServiceWireCodec.RelocationPrepareRecord Prepare { get; } = prepare;
        internal RoutingId SourceNodeRid { get; } = sourceNodeRid;
        internal ZLinkRelocationEnvelope Envelope { get; private set; } = envelope;
        internal ZLinkRelocationParticipantEnvelope Participant
            => Envelope.Participants[0];
        internal ZLinkActorRuntimeState ActorState { get; } = actorState;
        internal ZLinkSpotActivation? TargetActivation { get; } =
            targetActivation;
        internal ZLinkActorAuthorityPayload TargetAuthority { get; } = targetAuthority;
        internal ulong TargetAuthorityOwnerGeneration { get; } =
            targetAuthorityOwnerGeneration;
        internal IReadOnlyList<ZLinkActorHandoffFrame> AcceptedFrames
            { get; private set; } = acceptedFrames;
        internal bool FinalRootApplied { get; private set; }
        internal DateTimeOffset CreatedAt { get; } = TimeProvider.System.GetUtcNow();
        internal bool AuthorityPublished { get; set; }

        internal void RetainPermit(
            ZLinkRelocationPermitPool.ZLinkRelocationPermitLease permit)
        {
            lock (_permitGate)
            {
                if (_permitRetained)
                    throw DataLost(
                        "Standalone Actor target permit was retained more than once.");
                _permit = permit;
                _permitRetained = true;
            }
        }

        internal void ReleasePermit()
        {
            ZLinkRelocationPermitPool.ZLinkRelocationPermitLease permit;
            lock (_permitGate)
            {
                if (!_permitRetained) return;
                permit = _permit;
                _permit = default;
                _permitRetained = false;
            }
            permit.Dispose();
        }

        internal void ApplyFinalRoot(
            ZLinkRelocationEnvelope finalEnvelope,
            IReadOnlyList<ZLinkActorHandoffFrame> acceptedFrames)
        {
            Envelope = finalEnvelope;
            AcceptedFrames = acceptedFrames;
            FinalRootApplied = true;
        }

        internal void ValidateRetry(
            ZLinkServiceWireCodec.RelocationPrepareRecord retry,
            RoutingId authenticatedSourceNodeRid)
        {
            if (SourceNodeRid != authenticatedSourceNodeRid
                || !ZLinkServiceWireCodec.EncodeRelocationPrepare(Prepare)
                    .AsSpan()
                    .SequenceEqual(
                        ZLinkServiceWireCodec.EncodeRelocationPrepare(retry)))
                throw DataLost(
                    "Standalone Actor relocation retry changed its exact attempt.");
        }

        internal void ValidateTargetAuthorityOwnerGeneration(
            ulong candidate)
        {
            if (TargetAuthorityOwnerGeneration != candidate)
                throw DataLost(
                    "Standalone Actor relocation retry changed its target authority generation.");
        }
    }
}

internal sealed partial class ZLinkFrameworkRuntime
{
    private const string DurableActorReplyContentType =
        "application/x-zlink-actor-relocation-reply-v1";
    private const int DurableActorReplyRouteSize = sizeof(ulong);
    internal IReadOnlyList<Task<bool>>
        RelayStandaloneActorRelocationTrailing(
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef sourceActorRef,
        IReadOnlyList<ZLinkActorHandoffFrame> frames)
    {
        if (frames.Count == 0)
            return [];
        if (actorState.Handoff.RouteFrame(
                sourceActorRef,
                sourceActorRef,
                out var messageFollowRoute) != ZLinkActorFrameRoute.MessageFollow
            || messageFollowRoute is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorState.ActorId}' committed Message Follow route is unavailable.");
        var deliveries = new List<Task<bool>>(frames.Count);
        foreach (var frame in frames.OrderBy(static frame => frame.ArrivalIndex))
        {
            using var body = Message.From(frame.Body);
            deliveries.Add(ActorMessageFollower.EnqueueTracked(
                messageFollowRoute.Value,
                frame.SourceNodeRid.Length == 0
                    ? default
                    : RoutingId.From(frame.SourceNodeRid),
                frame.SourceSessionRid.Length == 0
                    ? default
                    : RoutingId.From(frame.SourceSessionRid),
                frame.RequestId,
                frame.Flags,
                frame.RouteContext,
                ZLinkStreamProtocolDefaults.DecodeHeader(frame.Header),
                body,
                frame.SourceNodeGeneration,
                frame.RequestSource));
        }
        return deliveries;
    }

    internal async ValueTask RestoreStandaloneActorRelocationSourceAsync(
        ZLinkActorRuntimeState actorState)
    {
        var frames = actorState.Handoff.BeginAbortCaptureRestore();
        if (frames.Count == 0)
        {
            actorState.Handoff.CompleteAbortCaptureRestore();
            return;
        }
        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' source reference is unavailable during rollback.");
        var pipeline = new ZLinkActorInboundPipeline(
            this,
            new ZLinkEntrySpotActorInboundEndpoint(this));
        var barrier = actorState.ReserveHandoffRestoreBarrier();
        var turn = await barrier.ClaimAsync().ConfigureAwait(false);
        var queued = new List<Task>(frames.Count);
        try
        {
            foreach (var frame in frames)
            {
                var batch = ZLinkActorHandoffFrames.Restore(
                    actorRef,
                    [frame]);
                try
                {
                    queued.Add(pipeline.DispatchSourceRestoreAsync(
                            batch,
                            static () => { },
                            CancellationToken.None)
                        .AsTask());
                }
                catch
                {
                    batch.Dispose();
                    throw;
                }
                actorState.Handoff.AcknowledgeAbortRestoreEnqueued(
                    frame.ArrivalIndex);
            }
            actorState.Handoff.CompleteAbortCaptureRestore();
        }
        finally
        {
            // Releasing this barrier starts only records whose ownership was
            // transferred to the application mailbox above.
            turn.Dispose();
        }
        await Task.WhenAll(queued).ConfigureAwait(false);
    }

    internal async ValueTask CompleteStandaloneActorRelocationSourceAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (actorState.LiveActivation is { } sourceActivation)
            sourceActivation.DetachRelocatedPerActorMember(actor, actorState);
        actorState.BindNativeActorRef(targetActorRef);
        actorState.InvalidateContext();
        await _actorSessionManager.FinalizeMigratedSourceAsync(
                actorState,
                sourceActorRef)
            .ConfigureAwait(false);
    }

    internal async ValueTask ActivateStandaloneActorRelocationTargetAsync(
        ZLinkActorRuntimeState actorState,
        ZLinkActorAuthorityPayload targetAuthority,
        ulong targetAttemptGeneration,
        ZLinkRelocationEnvelope relocationIdentity,
        string handoffId,
        CancellationToken cancellationToken)
    {
        if (actorState.Handoff.IsCanonicalMaintenanceReplayComplete(handoffId))
            return;
        _ = actorState.Actor
            ?? throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{actorState.ActorId}' is not materialized on the relocation target.");
        if (actorState.Handoff.GetCanonicalMaintenanceDrain(handoffId) is not null)
            return;
        _ = actorState.Handoff.PrepareCanonicalMaintenanceReplay(handoffId);
        await QueueCanonicalStandaloneActorFramesAsync(
                actorState,
                targetAuthority,
                targetAttemptGeneration,
                relocationIdentity,
                handoffId,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask CompleteStandaloneActorRelocationTargetAsync(
        ZLinkActorRuntimeState actorState,
        ZLinkActorAuthorityPayload targetAuthority,
        ulong targetAttemptGeneration,
        ZLinkRelocationEnvelope relocationIdentity,
        string handoffId,
        CancellationToken cancellationToken,
        bool normalizeSteady = true)
    {
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_target_complete_entry actor={actorState.ActorId} "
            + $"handoff={handoffId}");
        await ActivateStandaloneActorRelocationTargetAsync(
                actorState,
                targetAuthority,
                targetAttemptGeneration,
                relocationIdentity,
                handoffId,
                cancellationToken)
            .ConfigureAwait(false);
        if (actorState.Handoff.GetCanonicalMaintenanceDrain(handoffId)
            is { } drain)
            await drain.WaitAsync(cancellationToken).ConfigureAwait(false);
        var targetOwner = RequireStandaloneActorTargetOwner();
        var coordinator = CreateStandaloneActorProgressCoordinator(
            targetAuthority,
            targetAttemptGeneration,
            relocationIdentity.AggregateId,
            targetOwner);
        var progress = await coordinator.ReadAsync(
                relocationIdentity,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        var relocationId = Guid.ParseExact(handoffId, "N");
        if (progress.Phase.RelocationId != relocationId)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Actor '{actorState.ActorId}' lost its committed relocation phase.");
        var participant = progress.Root.Participants.Single();
        if (participant.ReplayCursor != participant.AcceptedBoundary
            || progress.Phase.TerminalCompletionCount
               != progress.Phase.AcceptedRequestCount
            || progress.Phase.PendingRelayCount != 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorState.ActorId}' relocation replay is not durably complete.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        if (progress.Phase.Phase
            == ZLinkActorRelocationAuthorityPhase.Cleaning)
        {
            progress = await coordinator.AdvancePhaseAsync(
                    relocationIdentity,
                    ZLinkActorRelocationAuthorityPhase.Cleaning,
                    ZLinkActorRelocationAuthorityPhase.Completed,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        else if (progress.Phase.Phase
                 != ZLinkActorRelocationAuthorityPhase.Completed)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Actor '{actorState.ActorId}' target completion phase is not recoverable.");
        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' target reference is unavailable.");
        await FinishRelocationTargetAsync(
                actorState,
                targetAuthority.MeshName,
                handoffId,
                cancellationToken)
            .ConfigureAwait(false);
        if (normalizeSteady)
            await coordinator.NormalizeSteadyAsync(
                    relocationIdentity,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
        actorState.Handoff.Complete(handoffId);
    }

    private ZLinkStandaloneActorRelocationProgressCoordinator
        CreateStandaloneActorProgressCoordinator(
            ZLinkActorAuthorityPayload targetAuthority,
            ulong targetAttemptGeneration,
            Guid relocationId,
            ZLinkLocationOwnerToken targetOwner) => new(
            Registration.Locations.ResolveStore()
            ?? throw new ZLinkConfigurationException(
                "Standalone Actor relocation requires a Location Store."),
            Registration.Locations.ResolveRelocationStore()
            ?? throw new ZLinkConfigurationException(
                "Standalone Actor relocation requires a Relocation Store."),
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId,
                targetAttemptGeneration,
                targetAuthority.NodeRid,
                targetAuthority.NodeGeneration,
                targetOwner));

    private ZLinkLocationOwnerToken RequireStandaloneActorTargetOwner() =>
        LocationLifecycle?.OwnerToken
        ?? throw new ZLinkConfigurationException(
            "Standalone Actor relocation requires a Location owner lease.");

    private async ValueTask QueueCanonicalStandaloneActorFramesAsync(
        ZLinkActorRuntimeState actorState,
        ZLinkActorAuthorityPayload targetAuthority,
        ulong targetAttemptGeneration,
        ZLinkRelocationEnvelope identity,
        string handoffId,
        CancellationToken cancellationToken)
    {
        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkRelocationDataLostException(
                           "Standalone Actor target reference is unavailable during replay.");
        var initialParticipant = identity.Participants.Single();
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            initialParticipant.RecoveryPayload.Span);
        var source = ZLinkActorRelocationSourceFenceCodec.Decode(
            recovery.MembershipMutation.Span);
        var sourceActor = new ZLinkBackendActorRef(
            source.NodeRid,
            actorState.ActorId,
            initialParticipant.ObjectGeneration);
        var targetOwner = RequireStandaloneActorTargetOwner();
        var coordinator = CreateStandaloneActorProgressCoordinator(
            targetAuthority,
            targetAttemptGeneration,
            identity.AggregateId,
            targetOwner);
        var progress = await coordinator.ReadAsync(
                identity,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        await RelayPendingStandaloneActorRepliesAsync(
                identity,
                targetAuthority,
                progress,
                coordinator,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        progress = await coordinator.ReadAsync(
                identity,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
        actorState.Handoff.AcknowledgeCanonicalReplayThrough(
            progress.Root.Participants.Single().ReplayCursor);

        var pipeline = new ZLinkActorInboundPipeline(
            this,
            new ZLinkEntrySpotActorInboundEndpoint(this));
        var durableReplayCursor = progress.Root.Participants.Single()
            .ReplayCursor;
        var participant = progress.Root.Participants.Single();
        var pendingJobs = participant.AcceptedJobs
            .Where(candidate =>
                candidate.AcceptedSequence > durableReplayCursor)
            .OrderBy(static candidate => candidate.AcceptedSequence)
            .ToArray();
        var barrier = actorState.ReserveHandoffRestoreBarrier();
        var turn = await barrier.ClaimAsync().ConfigureAwait(false);
        var queued = new List<Task>(pendingJobs.Length);
        Task<bool> durableCompletionTail = Task.FromResult(true);
        var queuedThrough = checked((long)Math.Max(
            durableReplayCursor,
            participant.AcceptedBoundary));
        try
        {
            foreach (var job in pendingJobs)
            {
                var accepted = ZLinkStandaloneActorRelocationRuntime
                    .DecodeAcceptedRecords([job])
                    .Single();
                var batch = ZLinkActorHandoffFrames.RestoreCanonical(
                    actorRef,
                    sourceActor,
                    [accepted]);
                try
                {
                    var previousDurableCompletion = durableCompletionTail;
                    var currentDurableCompletion = new TaskCompletionSource<bool>(
                        TaskCreationOptions.RunContinuationsAsynchronously);
                    durableCompletionTail = currentDurableCompletion.Task;
                    queued.Add(pipeline.QueueCanonicalReplayAsync(
                        batch,
                        async (frame, reply, ct) =>
                        {
                            try
                            {
                                if (!await previousDurableCompletion.ConfigureAwait(false))
                                    throw new ZLinkFrameworkException(
                                        ZLinkFrameworkErrorKind.Unavailable,
                                        "Standalone Actor replay is waiting for an earlier durable completion.",
                                        ZLinkRetryAdvice.RetryAfterBackoff);
                                ZLinkCanonicalTerminalCompletion? completion = null;
                                byte[]? replyFrame = null;
                                var request = job.CanonicalRequest;
                                var replyRouteId = request?.ReplyRouteId ?? 0;
                                if (request is not null && replyRouteId != 0)
                                {
                                    if (reply is null)
                                        throw new ZLinkRelocationDataLostException(
                                            "Standalone Actor accepted request replay produced no terminal reply.");
                                    replyFrame = reply.ToFrame(frame.Header);
                                    completion = ZLinkRelocationEnvelopeCodec
                                        .CreateCanonicalTerminalCompletion(
                                            request.OperationHigh,
                                            request.OperationLow,
                                            request.Source.OwnerId,
                                            request.Source.OwnerLeaseGeneration,
                                            request.Source.NodeRid,
                                            request.Source.NodeGeneration,
                                            initialParticipant.CanonicalParticipantId,
                                            job.AcceptedSequence,
                                            0,
                                            0,
                                            0,
                                            new ZLinkCanonicalApplicationPayload(
                                                frame.Header.Name,
                                                DurableActorReplyContentType,
                                                EncodeDurableActorReply(
                                                    request.ReplyRouteId,
                                                    replyFrame)));
                                }
                                var advanced = await coordinator.AdvanceReplayAsync(
                                        identity,
                                        job.AcceptedSequence,
                                        completion,
                                        targetOwner,
                                        ct)
                                    .ConfigureAwait(false);
                                if (completion is not null)
                                    await RelayStandaloneActorReplyAsync(
                                            identity,
                                            targetAuthority,
                                            replyRouteId,
                                            completion,
                                            replyFrame!,
                                            advanced,
                                            coordinator,
                                            targetOwner,
                                            ct)
                                        .ConfigureAwait(false);
                                actorState.Handoff.AcknowledgeCanonicalReplayThrough(
                                    job.AcceptedSequence);
                                currentDurableCompletion.TrySetResult(true);
                            }
                            catch
                            {
                                currentDurableCompletion.TrySetResult(false);
                                throw;
                            }
                        },
                        CancellationToken.None));
                }
                catch
                {
                    batch.Dispose();
                    throw;
                }
            }

            actorState.Handoff
                .ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
                    handoffId,
                    queuedThrough,
                    frame =>
                    {
                        var batch = ZLinkActorHandoffFrames.Restore(
                            actorRef,
                            [frame]);
                        try
                        {
                            queued.Add(pipeline.DispatchReplayAsync(
                                    batch,
                                    arrivalIndex => actorState.Handoff
                                        .AcknowledgeReplayedFrame(arrivalIndex),
                                    CancellationToken.None)
                                .AsTask());
                        }
                        catch
                        {
                            batch.Dispose();
                            throw;
                        }
                    });

            var drainStart = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            var drain = DrainCanonicalStandaloneActorFramesAsync(
                actorState,
                handoffId,
                queued,
                drainStart.Task);
            actorState.Handoff.RegisterCanonicalMaintenanceDrain(
                handoffId,
                drain);
            drainStart.TrySetResult();
        }
        finally
        {
            // All preserved work owns a FIFO mailbox position before direct
            // target admission opens and can append behind it.
            turn.Dispose();
        }
    }

    private static async Task DrainCanonicalStandaloneActorFramesAsync(
        ZLinkActorRuntimeState actorState,
        string handoffId,
        IReadOnlyCollection<Task> queued,
        Task drainStart)
    {
        await drainStart.ConfigureAwait(false);
        await Task.WhenAll(queued).ConfigureAwait(false);
        if (!actorState.Handoff.TryCompleteCanonicalMaintenanceReplay(handoffId))
            throw new ZLinkRelocationDataLostException(
                $"Actor '{actorState.ActorId}' target replay did not drain its preserved backlog.");
    }

    private async ValueTask RelayPendingStandaloneActorRepliesAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkActorAuthorityPayload targetAuthority,
        ZLinkStandaloneActorRelocationProgress progress,
        ZLinkStandaloneActorRelocationProgressCoordinator coordinator,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        var participant = progress.Root.Participants.Single();
        foreach (var completion in participant.TerminalCompletions
                     .Where(static candidate => candidate.DeliveryState == 0))
        {
            var (replyRouteId, payload) = DecodeDurableActorReply(completion);
            await RelayStandaloneActorReplyAsync(
                    identity,
                    targetAuthority,
                    replyRouteId,
                    completion,
                    payload,
                    progress,
                    coordinator,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask RelayStandaloneActorReplyAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkActorAuthorityPayload targetAuthority,
        ulong replyRouteId,
        ZLinkCanonicalTerminalCompletion completion,
        byte[] replyFrame,
        ZLinkStandaloneActorRelocationProgress progress,
        ZLinkStandaloneActorRelocationProgressCoordinator coordinator,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        if (replyRouteId == 0)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor reply lost its durable reply route.");
        var targetAttemptGeneration = progress.Canonical
            ?.TargetAttemptGeneration
            ?? throw new ZLinkRelocationDataLostException(
                "Standalone Actor reply lost its durable target attempt.");
        if (targetAttemptGeneration == 0)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor reply has an invalid durable target attempt.");
        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            new MeshOperationId(completion.OperationHigh, completion.OperationLow),
            replyRouteId,
            new ZLinkServiceWireCodec.RelocationWireId(
                identity.CanonicalRelocationHigh,
                identity.CanonicalRelocationLow),
            targetAttemptGeneration,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                targetOwner.OwnerId,
                checked((ulong)targetOwner.LeaseGeneration),
                targetAuthority.NodeRid,
                targetAuthority.NodeGeneration,
                progress.Authority.StoreVersion),
            completion.ParticipantId,
            completion.AcceptedSequence,
            completion.TerminalResult,
            (ServiceWireConstants.FrameworkErrorCode)completion.ErrorCode);
        ZLinkRelocationReplyAckState acknowledgement;
        try
        {
            acknowledgement = await RelayRelocationReplyAsync(
                    RoutingId.FromHex(completion.SourceNodeRid),
                    relay,
                    new ZLinkServiceWireCodec.RequestSourceFence(
                        completion.SourceOwnerId,
                        completion.SourceOwnerLeaseGeneration,
                        RoutingId.FromHex(completion.SourceNodeRid),
                        completion.SourceNodeGeneration),
                    [replyFrame],
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch
        {
            acknowledgement = ZLinkRelocationReplyAckState.NotAcknowledged;
        }

        byte deliveryState;
        if (acknowledgement is ZLinkRelocationReplyAckState.TerminalReceived)
            deliveryState = 1;
        else if (acknowledgement is ZLinkRelocationReplyAckState.AlreadyTerminal)
            deliveryState = 2;
        else
        {
            var store = Registration.Locations.ResolveStore()
                        ?? throw new ZLinkConfigurationException(
                            "Standalone Actor relocation requires a Location Store.");
            var lease = await store.ReadOwnerLeaseAsync(
                    completion.SourceOwnerId,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!IsExactSourceLeaseExpired(lease, completion))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Standalone Actor reply is waiting for its exact source acknowledgement.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            deliveryState = 3;
        }
        _ = await coordinator.CompleteReplyAsync(
                identity,
                completion,
                deliveryState,
                targetOwner,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal static byte[] EncodeDurableActorReply(
        ulong replyRouteId,
        ReadOnlySpan<byte> replyFrame)
    {
        if (replyRouteId == 0)
            throw new ArgumentOutOfRangeException(nameof(replyRouteId));
        var payload = new byte[DurableActorReplyRouteSize + replyFrame.Length];
        System.Buffers.Binary.BinaryPrimitives.WriteUInt64BigEndian(
            payload,
            replyRouteId);
        replyFrame.CopyTo(payload.AsSpan(DurableActorReplyRouteSize));
        return payload;
    }

    internal static (ulong ReplyRouteId, byte[] ReplyFrame)
        DecodeDurableActorReply(ZLinkCanonicalTerminalCompletion completion)
    {
        var payload = completion.Payload;
        if (payload is null
            || payload.ContentType != DurableActorReplyContentType
            || payload.Payload.Length <= DurableActorReplyRouteSize)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor pending reply payload is missing or malformed.");
        var bytes = payload.Payload.Span;
        var replyRouteId = System.Buffers.Binary.BinaryPrimitives
            .ReadUInt64BigEndian(bytes);
        if (replyRouteId == 0)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor pending reply route is invalid.");
        return (
            replyRouteId,
            bytes[DurableActorReplyRouteSize..].ToArray());
    }
}
