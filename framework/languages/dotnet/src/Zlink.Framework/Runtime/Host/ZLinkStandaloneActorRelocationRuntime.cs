using System.Collections.Concurrent;
using System.Diagnostics;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Identifiers;
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
    internal const ulong InitialTargetAttemptGeneration = 1;
    private static readonly TimeSpan TargetStageTtl = TimeSpan.FromMinutes(5);
    private readonly ConcurrentDictionary<AttemptKey, AttemptSlot> _targetAttempts = new();
    private int _targetAttemptAdmissionSealed;

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
            IZLinkBackendCanonicalRelocation? canonical = null;
            ZLinkServiceWireCodec.RelocationPrepareRecord? prepare = null;
            var precommit = new ZLinkStandaloneActorRelocationPrecommitCoordinator(
                authorityStore);
            ZLinkAuthoritySnapshot? precommitSnapshot = null;
            var acceptedCount = 0;
            var interruption =
                ZLinkRelocationInterruptionOperation.Disabled;
            var sessionRelocationContext =
                ZLinkSessionRelocationContext.Create(
                    relocationId,
                    found.Snapshot.OwnerId,
                    checked((ulong)found.Snapshot.OwnerLeaseGeneration),
                    sourceAuthority.NodeRid,
                    sourceAuthority.NodeGeneration,
                    found.Snapshot.StoreVersion);
            try
            {
                var route = default(ZLinkRemoteActorBoundSessionRoute);
                if (actorState.TryGetBoundSession(out var bound))
                {
                    sealedSession = await SealSessionRouteAsync(
                            actorState,
                            bound,
                            sessionRelocationContext,
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
                ValidateRelocationStateBound(actorState.ActorId, applicationState);
                // Switch ingress to the bounded final hold while the immutable
                // journal is built. The exact commit boundary is frozen only
                // after the source fences in the initial journal are verified.
                var sourceNode = runtime.GetSpotNodeRuntime(
                    sourceRef.Value.NodeRid);
                //  Spec 30 §11: the SafeToShutdown obligation starts at
                //  seal, not at cutover — waiting until CommitMessageFollow
                //  would let a shutdown query race the seal-to-cutover
                //  window. Attaching the token in the same locked call as
                //  the seal closes the window where a status read could
                //  observe the seal without the obligation counted, or an
                //  abort could race the attach.
                actorState.Handoff.SealCapture(runtime.BeginPendingRelocationUnit());
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
                //  Direct transfer (spec 28 §4.2): the captured envelope is
                //  encoded once into source memory — the single handoff
                //  origin — and never written to the Relocation Store.
                var transferPayload = ZLinkRelocationTransferPayload.Create(
                    initialEnvelope,
                    registration.Locations.Options.RelocationPayloadChunkLimit);
                initialPrepared = new ZLinkPreparedRelocation(
                    new ZLinkRelocationStored(
                        string.Empty,
                        transferPayload.ChecksumCrc32c,
                        default,
                        default),
                    initialEnvelope)
                {
                    LogicalLength = transferPayload.TotalLength,
                    LogicalChecksumCrc32c = transferPayload.ChecksumCrc32c,
                    ChunkCount = transferPayload.ChunkCount
                };
                precommitSnapshot = await precommit.CaptureAsync(
                        precommitSnapshot,
                        initialEnvelope,
                        cancellationToken)
                    .ConfigureAwait(false);

                if (sourceNode.Node
                    is not IZLinkBackendCanonicalRelocation backend)
                    throw new ZLinkConfigurationException(
                        "The source MeshNode does not support canonical relocation commands.");
                canonical = backend;
                //  Same pre-precommit Coordinator fence as the remote-join
                //  path (ZLinkActorRemoteJoiner.cs): sessionRelocationContext
                //  above already fences on found.Snapshot (pre-BeginPreparing
                //  StoreVersion) — Prepare must reuse that same baseline, not
                //  the post-Capture precommitSnapshot, whose StoreVersion has
                //  already rotated past what the durable ZLJR/saved-work
                //  recovery carries.
                prepare = CreatePrepare(
                        found.Snapshot,
                        sourceAuthority,
                        target,
                        initialEnvelope,
                        transferPayload,
                        registration.ApplicationVersion);
                _ = await canonical.PrepareCanonicalRelocationAsync(
                        target.Rid,
                        prepare,
                        transferPayload,
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
                var data = acceptedRecords
                    .Skip(semanticSealRecords.Count)
                    .Select(accepted =>
                        new ZLinkServiceWireCodec.RelocationDataRecord(
                            prepare.RelocationId,
                            prepare.TargetAttemptGeneration,
                            prepare.Coordinator,
                            1,
                            prepare.Object,
                            new ZLinkServiceWireCodec.FrozenRecord(
                                ZLinkCanonicalActorAcceptedJournal.Encode(
                                    accepted,
                                    sourceRef.Value))))
                    .ToArray();
                foreach (var record in data)
                    await canonical.SendCanonicalRelocationDataAsync(
                            target.Rid,
                            record,
                            cancellationToken)
                        .ConfigureAwait(false);
                //  Spec 28 §4.4: the cutover carries the boundary record
                //  count and the CRC-32C over the relayed pre-boundary
                //  records so the target can compare its staged relay span.
                await canonical.SendCanonicalRelocationCutoverAsync(
                        target.Rid,
                        new ZLinkServiceWireCodec.RelocationCutoverRecord(
                            prepare.RelocationId,
                            prepare.TargetAttemptGeneration,
                            prepare.Coordinator,
                            1,
                            prepare.Object,
                            checked((ulong)data.Length),
                            ZLinkRelocationBoundaryBatch.ComputeChecksum(
                                data.Select(static record =>
                                    record.FrozenRecord.Encoded))),
                        cancellationToken)
                    .ConfigureAwait(false);
                var committedTarget = await WaitForCommittedTargetAuthorityAsync(
                        authorityStore,
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                            actorState.ActorId),
                        found.Snapshot,
                        initialPrepared.Relocation,
                        relocationId,
                        target,
                        prepare.TargetAttemptGeneration,
                        cancellationToken)
                    .ConfigureAwait(false);
                committed = true;
                await CompleteCommittedSourceAsync(
                        actor,
                        actorState,
                        sourceRef.Value,
                        sourceAuthority,
                        found.Snapshot,
                        target,
                        acceptedCount,
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
                        if (initialPrepared is null
                            || canonical is null
                            || prepare is null
                            || !IsExactCommittedTargetAuthority(
                                authority,
                                found.Snapshot,
                                initialPrepared.Relocation,
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
                                initialPrepared.Relocation,
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
                                acceptedCount,
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
                    if (initialPrepared is not null)
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
                                sessionRelocationContext,
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
               != checked(source.AuthorityOwnerGeneration + 1)
            || found.Snapshot.OwnerId != target.OwnerId
            || found.Snapshot.OwnerLeaseGeneration != target.LeaseGeneration
            || found.Snapshot.Allocation.Descriptor
               != new ZLinkMeshNodeDescriptorKey(target.MeshName, target.Rid)
            || found.Snapshot.Allocation.DescriptorLifecycleGeneration
               != target.LifecycleGeneration)
            return false;

        if (ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                found.Snapshot.Payload.Span,
                out var canonical))
        {
            var pointerMatches = root.Reference.Length == 0
                ? canonical.RelocationReference.Length == 0
                : StringComparer.Ordinal.Equals(
                      canonical.RelocationReference,
                      root.Reference)
                  && canonical.RelocationChecksumCrc32c
                     == root.ChecksumCrc32c;
            return canonical.Phase >= (byte)(requireActivated
                       ? ZLinkStandaloneActorCanonicalPhase.Activated
                       : ZLinkStandaloneActorCanonicalPhase.Committed)
                   && canonical.RelocationHigh == ToWireId(relocationId).High
                   && canonical.RelocationLow == ToWireId(relocationId).Low
                   && canonical.TargetAttemptGeneration
                      == targetAttemptGeneration
                   && pointerMatches
                   && StringComparer.Ordinal.Equals(
                       canonical.TargetOwnerId,
                       target.OwnerId)
                   && canonical.TargetOwnerLeaseGeneration
                      == checked((ulong)target.LeaseGeneration);
        }

        if (ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var publication))
            return publication.AggregateId == relocationId
                   && StringComparer.Ordinal.Equals(
                       publication.Reference,
                       root.Reference)
                   && publication.ChecksumCrc32c == root.ChecksumCrc32c
                   && StringComparer.Ordinal.Equals(
                       publication.TargetOwnerId,
                       target.OwnerId)
                   && publication.TargetOwnerLeaseGeneration
                      == target.LeaseGeneration;

        if (ZLinkActorAuthorityPayloadCodec.TryDecodeDirect(
                found.Snapshot.Payload.Span,
                out var steady))
            return steady.State == ZLinkActorAuthorityState.Ready
                   && steady.NodeRid == target.Rid
                   && steady.NodeGeneration == target.LifecycleGeneration
                   && StringComparer.Ordinal.Equals(steady.OwnerId, target.OwnerId)
                   && steady.OwnerLeaseGeneration
                      == checked((ulong)target.LeaseGeneration);

        // Spec 52 §4.3 makes the target-only authority CAS the commit
        // boundary for a foreign target's opaque progress payload. The exact
        // outer owner and allocation fences above are the common contract.
        return true;
    }

    internal static async ValueTask<ZLinkAuthoritySnapshot>
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
        int acceptedCount,
        ulong targetAuthorityOwnerGeneration,
        ZLinkRelocationInterruptionOperation interruption,
        CancellationToken cancellationToken)
    {
        var targetRef = new ZLinkBackendActorRef(
            target.Rid, actorState.ActorId, sourceRef.Generation);
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
            registration.Locations.Options.MessageFollowDuration,
            registration.Locations.Options.RelocationCutoverWaitTimeout);
        if (trailingDeliveries.Count != 0
            && (await Task.WhenAll(trailingDeliveries)
                    .ConfigureAwait(false)).Any(static delivered => !delivered))
            throw new ZLinkRelocationDataLostException(
                $"Actor '{actorState.ActorId}' could not deliver its pre-cutover Message Follow backlog.");
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
                ZLinkActorAuthorityPayloadCodec.Encode(targetAuthority)));
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
                ReadOnlyMemory<byte>.Empty,
                maintenancePolicy));
        var savedWork = new List<ZLinkRelocationQueuedJob>();
        if (!remoteJoinRecovery.IsEmpty)
            savedWork.Add(ZLinkActorRemoteJoinRecoverySavedWork.Create(
                1, ZLinkActorRelocationSourceFenceCodec.Decode(
                    ZLinkCanonicalParticipantRecoveryCodec.Decode(recovery).MembershipMutation.Span),
                remoteJoinRecovery));
        savedWork.AddRange(acceptedRecords
            .OrderBy(static accepted => accepted.Frame.ArrivalIndex)
            .Select((accepted, index) => new ZLinkRelocationQueuedJob(
                checked((ulong)index + (remoteJoinRecovery.IsEmpty ? 1UL : 2UL)),
                ZLinkCanonicalActorAcceptedJournal.Encode(accepted, sourceActor))));
        var participant = new ZLinkRelocationParticipantEnvelope(
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(sourceAuthority.ActorId),
            ZLinkPlacementObjectKind.Actor,
            sourceSnapshot.ObjectGeneration,
            sourceSnapshot.AuthorityOwnerGeneration,
            applicationState,
            savedWork,
            [],
            RecoveryPayload: recovery)
        {
            CanonicalParticipantId = 1
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
        ZLinkSessionRelocationContext wireContext,
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
        _ = await runtime.SealSessionRelocationAsync(
                normalized.MeshName.Value,
                sessionNode,
                ZLinkSessionRelocationWire.CreateSeal(
                    actorState.ActorId,
                    sourceRef.NodeRid,
                    normalized,
                    wireContext),
                cancellationToken)
            .ConfigureAwait(false);
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
            normalized.AcceptedHighWater,
            normalized.SessionOwnerId,
            normalized.SessionOwnerLeaseGeneration);
        return normalized;
    }

    private async ValueTask AbortSessionRouteBestEffortAsync(
        string actorId,
        ZLinkActorBoundSession session,
        ZLinkSessionRelocationContext wireContext,
        CancellationToken cancellationToken)
    {
        try
        {
            var sessionNode = session.SessionNodeRid!.Value;
            await runtime.RouteSessionRelocationAsync(
                    session.MeshName.Value,
                    sessionNode,
                    ZLinkSessionRelocationWire.CreateAbort(
                        actorId,
                        session,
                        wireContext),
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
        session.MeshName.Value,
        session.TargetNodeGeneration,
        session.OwnerLeaseGeneration,
        session.SessionOwnerNodeGeneration,
        session.AcceptedHighWater,
        session.SessionOwnerId,
        session.SessionOwnerLeaseGeneration);

    private static void ValidateRelocationStateBound(
        string actorId,
        byte[] state)
    {
        if (state.Length
            > ZLinkRemoteActorJoinPackets
                .SnapshotApplicationStateReservationBytes)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Actor '{actorId}' relocation state exceeds 64 MiB.");
    }

    internal static ZLinkServiceWireCodec.RelocationPrepareRecord CreatePrepare(
        ZLinkAuthoritySnapshot sourceSnapshot,
        ZLinkActorAuthorityPayload sourceAuthority,
        ZLinkMeshNodeDescriptor target,
        ZLinkRelocationEnvelope envelope,
        ZLinkRelocationTransferPayload payload,
        long applicationVersion)
    {
        var relocationId = ToWireId(envelope.AggregateId);
        return new ZLinkServiceWireCodec.RelocationPrepareRecord(
            relocationId,
            InitialTargetAttemptGeneration,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                sourceSnapshot.OwnerId,
                checked((ulong)sourceSnapshot.OwnerLeaseGeneration),
                sourceAuthority.NodeRid,
                sourceAuthority.NodeGeneration,
                sourceSnapshot.StoreVersion),
            new ZLinkServiceWireCodec.RelocationTargetRecord(
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
            checked((ulong)payload.TotalLength),
            checked((uint)payload.ChunkCount),
            payload.ChecksumCrc32c,
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
        ZLinkRelocationEnvelope envelope,
        RoutingId authenticatedSourceNodeRid,
        ulong targetAuthorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        await SweepExpiredTargetStagesAsync().ConfigureAwait(false);
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        using var lease = AcquireTargetAttempt(key);
        if (envelope.Participants.Count == 1
            && TryReadRoutedJoinSavedWork(
                envelope.Participants[0],
                out _,
                out var routedJoinRecovery))
            lease.Slot.ObserveRoutedJoinPreparation(
                routedJoinRecovery.Request);
        try
        {
            await lease.Slot.RunAsync(async () => await StageTargetCoreAsync(
                    lease.Slot,
                    prepare,
                    envelope,
                    authenticatedSourceNodeRid,
                    targetAuthorityOwnerGeneration,
                    cancellationToken)
                .ConfigureAwait(false)).ConfigureAwait(false);
        }
        finally
        {
            CloseTargetAttemptIfEmpty(key, lease.Slot);
        }
    }

    internal async ValueTask AppendTargetDataAsync(
        ZLinkServiceWireCodec.RelocationDataRecord data,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken)
    {
        var key = new AttemptKey(
            data.RelocationId.High,
            data.RelocationId.Low,
            data.TargetAttemptGeneration);
        if (!TryAcquireTargetAttempt(key, out var lease))
            throw DataLost(
                "Standalone Actor relocation data has no prepared target.");
        using var attemptLease = lease;
        try
        {
            await lease.Slot.RunAsync(() =>
            {
                if (lease.Slot.Stage is not { } stage)
                    throw DataLost(
                        "Standalone Actor relocation data has no prepared target.");
                stage.ValidateData(data, authenticatedSourceNodeRid);
                stage.TrackRelayRecord(data.FrozenRecord.Encoded.Span);
                var nextArrival = stage.NextArrivalIndex;
                var frame = ZLinkCanonicalActorAcceptedJournal.Decode(
                        data.FrozenRecord.Encoded.Span,
                        nextArrival)
                    .Frame;
                if (stage.RemoteJoinRequest is not null)
                    stage.ActorState.Handoff.AppendPreparedImport(
                        stage.Envelope.AggregateId.ToString("N"),
                        [frame]);
                else
                    stage.ActorState.Handoff.AppendCanonicalMaintenanceImport(
                        stage.Envelope.AggregateId.ToString("N"),
                        [frame]);
                stage.Append(frame);
                return ValueTask.CompletedTask;
            }).ConfigureAwait(false);
        }
        finally
        {
            CloseTargetAttemptIfEmpty(key, lease.Slot);
        }
    }

    internal ValueTask CutoverTargetAsync(
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken) =>
        CutoverTargetAsync(
            cutover,
            authenticatedSourceNodeRid,
            verifyBoundary: true,
            cancellationToken);

    private async ValueTask CutoverTargetAsync(
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
        RoutingId authenticatedSourceNodeRid,
        bool verifyBoundary,
        CancellationToken cancellationToken)
    {
        var key = new AttemptKey(
            cutover.RelocationId.High,
            cutover.RelocationId.Low,
            cutover.TargetAttemptGeneration);
        if (!TryAcquireTargetAttempt(key, out var lease))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                "late_cutover object=actor reason=no_prepared_target");
            return;
        }
        using var attemptLease = lease;
        try
        {
            await lease.Slot.RunAsync(async () =>
            {
            if (lease.Slot.Stage is not { } stage)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    "late_cutover object=actor reason=no_prepared_target");
                return;
            }
            stage.ValidateCutover(cutover, authenticatedSourceNodeRid);
            if (stage.AuthorityPublished)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    "late_cutover object=actor reason=already_committed");
                return;
            }
            if (verifyBoundary)
                stage.ValidateBoundary(cutover);

            var store = registration.Locations.ResolveStore()
                        ?? throw new ZLinkConfigurationException(
                            "Location Store is not registered.");
            var read = await store.ReadAuthorityAsync(
                    stage.Participant.AuthorityKey,
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw DataLost(
                    "Standalone Actor target cutover lost its source authority.");
            if (!ZLinkStandaloneActorRelocationTakeoverCoordinator
                    .IsCurrentAttempt(
                        found.Snapshot,
                        stage.Envelope.AggregateId,
                        stage.Prepare.TargetAttemptGeneration,
                        stage.TargetAuthority))
            {
                var committed = await new
                        ZLinkStandaloneActorRelocationPrecommitCoordinator(store)
                    .CommitTargetAsync(
                        found.Snapshot,
                        stage.Envelope,
                        stage.Prepare,
                        stage.TargetAuthority,
                        stage.TargetAuthorityOwnerGeneration,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (committed.AuthorityOwnerGeneration
                    != stage.TargetAuthorityOwnerGeneration)
                    throw DataLost(
                        "Standalone Actor target CAS returned an unexpected owner generation "
                        + $"(actual={committed.AuthorityOwnerGeneration}, "
                        + $"expected={stage.TargetAuthorityOwnerGeneration}).");
            }

            MarkAuthorityPublished(stage);
            //  Spec 25 §5: target-local S2 (CAS confirmed) → S3 (dispatch
            //  open) resume interval for this unit.
            var resumeStartedTimestamp = Stopwatch.GetTimestamp();
            if (stage.RemoteJoinRequest is { } remoteJoinRequest
                && stage.RemoteJoinRecovery is { } remoteJoinRecovery)
            {
                var lifecycleCompleted = await runtime
                    .CompleteCanonicalRoutedActorJoinLifecycleAsync(
                        remoteJoinRecovery.TargetSpotId,
                        stage.ActorState,
                        remoteJoinRequest,
                        remoteJoinRecovery,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (!lifecycleCompleted)
                {
                    RemoveTargetStage(key, lease.Slot, stage);
                    return;
                }
                await CompleteDirectRemoteJoinTargetAsync(
                        stage,
                        remoteJoinRequest,
                        remoteJoinRecovery,
                        cancellationToken)
                    .ConfigureAwait(false);
                RemoveTargetStage(key, lease.Slot, stage);
            }
            else
            {
                await ActivatePublishedTargetCoreAsync(
                        stage,
                        cancellationToken)
                    .ConfigureAwait(false);
                await FinishTargetStageAsync(
                        key,
                        lease.Slot,
                        stage,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            ZLinkRuntimeMetrics.RecordRelocationTargetResume(
                Stopwatch.GetElapsedTime(resumeStartedTimestamp),
                "actor");
            }).ConfigureAwait(false);
        }
        finally
        {
            CloseTargetAttemptIfEmpty(key, lease.Slot);
        }
    }

    internal bool MarkTargetReadySubmitted(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!TryAcquireTargetAttempt(key, out var lease))
            return false;
        using (lease)
        {
            if (lease.Slot.Stage is not { } stage)
                return false;
            stage.ValidateRetry(prepare, authenticatedSourceNodeRid);
            return stage.TryMarkReadySubmitted(
                () => ReferenceEquals(lease.Slot.Stage, stage));
        }
    }

    internal bool MarkTargetReadySubmissionFailed(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!TryAcquireTargetAttempt(key, out var lease))
            return false;
        using (lease)
        {
            if (lease.Slot.Stage is not { } stage)
                return false;
            stage.ValidateRetry(prepare, authenticatedSourceNodeRid);
            return stage.TryMarkReadySubmissionFailed(
                () => ReferenceEquals(lease.Slot.Stage, stage));
        }
    }

    internal void ScheduleTargetCutoverFallback(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid) =>
        runtime.RunDetached(
            "standalone-actor-cutover-fallback",
            _ => new ValueTask(RunTargetCutoverFallbackAsync(
                prepare,
                authenticatedSourceNodeRid)));

    private async Task RunTargetCutoverFallbackAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid)
    {
        try
        {
            await Task.Delay(
                    registration.Locations.Options.RelocationCutoverWaitTimeout,
                    runtime.ShutdownToken)
                .ConfigureAwait(false);
            var key = new AttemptKey(
                prepare.RelocationId.High,
                prepare.RelocationId.Low,
                prepare.TargetAttemptGeneration);
            if (!TryAcquireTargetAttempt(key, out var lease))
                return;
            using (lease)
                if (lease.Slot.Stage is not { } stage
                    || stage.AuthorityPublished)
                    return;
            ZLinkFrameworkDebugLog.SpotDiscovery(
                "cutover_timeout object=actor");
            //  Spec 28 §4.4/25 §5: the fallback proceeds to the CAS without
            //  boundary completeness verification and is counted.
            ZLinkRuntimeMetrics.RecordRelocationCutoverTimeout("actor");
            await CutoverTargetAsync(
                    new ZLinkServiceWireCodec.RelocationCutoverRecord(
                        prepare.RelocationId,
                        prepare.TargetAttemptGeneration,
                        prepare.Coordinator,
                        prepare.InitiatorRole,
                        prepare.Object,
                        0,
                        0),
                    authenticatedSourceNodeRid,
                    verifyBoundary: false,
                    runtime.ShutdownToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (runtime.ShutdownToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"location_update_failed object=actor error={error.GetType().Name}");
        }
    }

    private async ValueTask SweepExpiredTargetStagesAsync()
    {
        var cutoff = TimeProvider.System.GetUtcNow() - TargetStageTtl;
        foreach (var pair in _targetAttempts)
        {
            if (!TryAcquireTargetAttempt(pair.Key, pair.Value, out var lease))
                continue;
            using (lease)
            {
                try
                {
                    await pair.Value.RunAsync(async () =>
                    {
                    if (pair.Value.Abort is { } abort
                        && abort.IsCompleted
                        && abort.CreatedAt <= cutoff)
                        pair.Value.TryRemoveAbort(abort);
                    if (pair.Value.Stage is { } stage
                        && !stage.AuthorityPublished
                        && stage.CreatedAt <= cutoff
                        && pair.Value.TryRemoveStage(stage))
                    {
                        stage.ActorState.AbortRelocationSessionRoute(
                            stage.Envelope.AggregateId.ToString("N"));
                        DetachTargetMembership(stage);
                        await actorSessions.RollbackTransferredActorAsync(
                                stage.ActorState.ActorId,
                                CancellationToken.None)
                            .ConfigureAwait(false);
                    }
                    }).ConfigureAwait(false);
                }
                finally
                {
                    CloseTargetAttemptIfEmpty(pair.Key, pair.Value);
                }
            }
        }
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

        var targetAttemptGeneration = canonical.TargetAttemptGeneration;
        var localNode = runtime.TryGetSpotNodeRuntime(targetAuthority.NodeRid);
        var hasCurrentLocalTarget = localNode is not null
            && localNode.Node.MeshStatus().LifecycleGeneration
            == targetAuthority.NodeGeneration;
        ZLinkActorRuntimeState actorState;
        ZLinkObjectRelocationRegistration relocation;
        if (sourceOwned || !hasCurrentLocalTarget)
        {
            var recoveryFence =
                new ZLinkStandaloneActorRelocationTakeoverCoordinator(
                    runtime, registration);
            if (await recoveryFence.HasLiveRemoteRecoveryOwnerAsync(
                    canonical,
                    targetAuthority.MeshName,
                    cancellationToken)
                .ConfigureAwait(false))
                return;
            if (sourceOwned)
            {
                // Pre-commit recovery with a dead source: no different-target
                // transition exists in this contract version, so restore the
                // steady source authority through the same precommit abort the
                // source coordinator uses.
                await AbortSourceOwnedRecoveryAsync(
                        candidate,
                        recovery,
                        cancellationToken)
                    .ConfigureAwait(false);
                return;
            }
            // Post-commit target death: the committed target attempt is the
            // only legal owner of this journal. Park until the exact target
            // lifecycle returns; there is no source rollback after commit.
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"standalone_recovery_parked actor={targetAuthority.ActorId} "
                + $"relocation={candidate.Envelope.AggregateId:N} "
                + "reason=committed_target_unavailable");
            return;
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
            relocating.BoundSessionRoute,
            new ZLinkSessionRelocationContext(
                new ZLinkServiceWireCodec.RelocationWireId(
                    candidate.Envelope.CanonicalRelocationHigh,
                    candidate.Envelope.CanonicalRelocationLow),
                new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                    canonical.State.CoordinatorOwnerId,
                    canonical.State.CoordinatorLeaseGeneration,
                    RoutingId.FromHex(
                        canonical.State.CoordinatorNodeRid),
                    canonical.State.CoordinatorNodeGeneration,
                    canonical.State
                        .CoordinatorExpectedAuthorityStoreVersion)));
        var frames = DecodeAcceptedRecords(participant.AcceptedJobs)
            .Select(static accepted => accepted.Frame)
            .ToArray();
        actorState.Handoff.BeginCanonicalMaintenanceImport(
            handoffId,
            frames);
        var actorRef = actorState.NativeActorRef
                       ?? throw DataLost(
                           "Standalone Actor recovery did not create a target reference.");
        actorState.MarkRelocationSessionAuthorityCommitted(
            handoffId,
            actorRef,
            authority.Snapshot.AuthorityOwnerGeneration,
            ZLinkMeshName.FromBoundary(
                targetAuthority.MeshName,
                nameof(targetAuthority.MeshName)),
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

    private async ValueTask AbortSourceOwnedRecoveryAsync(
        ZLinkRelocationRecoveryCandidate candidate,
        ZLinkCanonicalParticipantRecovery recovery,
        CancellationToken cancellationToken)
    {
        var store = registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        _ = await new ZLinkStandaloneActorRelocationPrecommitCoordinator(store)
            .AbortSourceAsync(
                recovery.AuthorityKey,
                candidate.Envelope.AggregateId,
                cancellationToken)
            .ConfigureAwait(false);
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            "standalone_recovery_precommit_aborted "
            + $"relocation={candidate.Envelope.AggregateId:N}");
    }

    private async ValueTask StageTargetCoreAsync(
        AttemptSlot slot,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        ZLinkRelocationEnvelope envelope,
        RoutingId authenticatedSourceNodeRid,
        ulong targetAuthorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        ValidatePrepare(prepare, authenticatedSourceNodeRid);
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (slot.Abort is { } abort)
        {
            abort.Stage.ValidateRetry(prepare, authenticatedSourceNodeRid);
            abort.Stage.ValidateTargetAuthorityOwnerGeneration(
                targetAuthorityOwnerGeneration);
            if (!abort.IsCompleted
                || !slot.TryRemoveAbort(abort))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Standalone Actor target cleanup is still in progress.",
                    retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
        }
        if (slot.Stage is { } existing)
        {
            existing.ValidateRetry(prepare, authenticatedSourceNodeRid);
            existing.ValidateTargetAuthorityOwnerGeneration(
                targetAuthorityOwnerGeneration);
            existing.BeginReadySubmission();
            return;
        }

        //  Spec 28 §4.3: the payload arrived over relocationState chunks on
        //  this connection and was assembled and checksum-verified before the
        //  staging call — the Relocation Store holds no handoff payload.
        var participant = envelope.Participants.SingleOrDefault()
                          ?? throw DataLost(
                              "Standalone Actor relocation must contain one participant.");
        // Canonical Node writers identify the sole Actor participant by its
        // object id.  .NET location APIs use the canonical actor authority
        // key, so normalize that transport spelling before every store fence
        // and target-progress operation below.
        if (StringComparer.Ordinal.Equals(
                participant.AuthorityKey.Value, prepare.Object.ObjectId))
        {
            participant = participant with
            {
                AuthorityKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                    prepare.Object.ObjectId)
            };
            envelope = envelope with { Participants = [participant] };
        }
        ZLinkCanonicalParticipantRecovery recovery;
        ZLinkActorRelocationSourceFence sourceFence;
        ZLinkActorRelocationRecoveryRecord? remoteJoinRecovery = null;
        try
        {
            recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                participant.RecoveryPayload.Span);
            sourceFence = ZLinkActorRelocationSourceFenceCodec.Decode(
                recovery.MembershipMutation.Span);
            if (!recovery.OperationRecovery.IsEmpty
                || !sourceFence.LegacyRemoteJoinRecovery.IsEmpty)
                remoteJoinRecovery = ZLinkActorRemoteJoinRecoveryCodec.Decode(
                    recovery.OperationRecovery.Span,
                    sourceFence.LegacyRemoteJoinRecovery.Span);
        }
        // The canonical kind-1 state body deliberately has no ZLRP field.
        // Decoding that empty legacy field fails at its first read with
        // EndOfStreamException, while malformed non-empty records report
        // InvalidDataException.  Both cases may be the saved-work migration.
        catch (Exception error) when (
            error is InvalidDataException or EndOfStreamException)
        {
            if (TryReadRoutedJoinSavedWork(
                    participant, out sourceFence, out var savedRecovery))
            {
                remoteJoinRecovery = savedRecovery;
                var store = registration.Locations.ResolveStore()
                            ?? throw new ZLinkConfigurationException(
                                "Standalone Actor relocation requires a Location Store.");
                var source = await store.ReadAuthorityAsync(
                        participant.AuthorityKey, cancellationToken)
                    .ConfigureAwait(false);
                if (source is not ZLinkAuthorityReadResult.Found found
                    || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                        found.Snapshot.Payload.Span, out var sourceAuthority))
                    throw DataLost("Standalone Actor routed-join recovery source authority is unavailable.");
                var target = sourceAuthority with
                {
                    State = ZLinkActorAuthorityState.Ready,
                    CurrentSpotId = savedRecovery.TargetSpotId,
                    CurrentSpotGeneration = savedRecovery.TargetSpotGeneration,
                    OwnerId = prepare.Target.OwnerId,
                    OwnerLeaseGeneration = prepare.Target.OwnerLeaseGeneration,
                    NodeRid = prepare.Target.NodeRid,
                    NodeGeneration = prepare.Target.NodeGeneration
                };
                var derivedRelocating = ZLinkActorRelocationAuthorityPayloadCodec.Encode(
                    new ZLinkActorRelocationAuthorityPayload(
                        envelope.AggregateId,
                        ZLinkActorRelocationAuthorityPhase.Activated,
                        ZLinkRemoteActorJoinPackets.DecodeBoundSessionRoute(
                            savedRecovery.Request),
                        ZLinkActorAuthorityPayloadCodec.Encode(target)));
                recovery = new ZLinkCanonicalParticipantRecovery(
                    participant.AuthorityKey, participant.ObjectKind,
                    participant.ObjectGeneration, participant.AuthorityOwnerGeneration,
                    found.Snapshot.StoreVersion, sourceAuthority.StableType, derivedRelocating,
                    ZLinkActorRelocationSourceFenceCodec.Encode(sourceFence),
                    ReadOnlyMemory<byte>.Empty,
                    ZLinkObjectMaintenancePolicyKind.Snapshot);
            }
            else if (participant.RecoveryPayload.IsEmpty)
            {
                // Whole-node relocation has no routed-join record.  Its frozen
                // body is raw state, and command 40 carries the exact source
                // fence while the target entry spot is local and generation
                // fenced.  Reconstruct only that derived recovery projection.
                var targetNode = runtime.GetSpotNodeRuntime(prepare.Target.NodeRid);
                var store = registration.Locations.ResolveStore()
                            ?? throw new ZLinkConfigurationException(
                                "Standalone Actor relocation requires a Location Store.");
                var source = await store.ReadAuthorityAsync(
                        participant.AuthorityKey, cancellationToken)
                    .ConfigureAwait(false);
                if (source is not ZLinkAuthorityReadResult.Found found
                    || string.IsNullOrWhiteSpace(
                        found.Snapshot.Allocation.StableType))
                    throw DataLost(
                        "Standalone Actor canonical recovery source authority is unavailable.");
                var stableType = found.Snapshot.Allocation.StableType;
                sourceFence = new ZLinkActorRelocationSourceFence(
                    prepare.Coordinator.OwnerId,
                    prepare.Coordinator.LeaseGeneration,
                    prepare.SourceNodeRid,
                    prepare.SourceNodeGeneration);
                var target = new ZLinkActorAuthorityPayload(
                    ZLinkActorAuthorityState.Ready,
                    stableType,
                    prepare.Object.ObjectId,
                    targetNode.EntrySpotId,
                    prepare.Target.NodeGeneration,
                    ZLinkSpotKind.Entry,
                    prepare.Target.OwnerId,
                    prepare.Target.OwnerLeaseGeneration,
                    targetNode.Node.MeshStatus().MeshName,
                    prepare.Target.NodeRid,
                    prepare.Target.NodeGeneration);
                var derivedRelocating = ZLinkActorRelocationAuthorityPayloadCodec.Encode(
                    new ZLinkActorRelocationAuthorityPayload(
                        envelope.AggregateId,
                        ZLinkActorRelocationAuthorityPhase.Activated,
                        default,
                        ZLinkActorAuthorityPayloadCodec.Encode(target)));
                recovery = new ZLinkCanonicalParticipantRecovery(
                    participant.AuthorityKey, participant.ObjectKind,
                    participant.ObjectGeneration, participant.AuthorityOwnerGeneration,
                    found.Snapshot.StoreVersion, stableType,
                    derivedRelocating,
                    ZLinkActorRelocationSourceFenceCodec.Encode(sourceFence),
                    ReadOnlyMemory<byte>.Empty,
                    ZLinkObjectMaintenancePolicyKind.Snapshot);
            }
            else
            {
                throw;
            }
        }
        var effectiveTargetAuthorityOwnerGeneration =
            remoteJoinRecovery?.TargetAuthorityOwnerGeneration
            ?? targetAuthorityOwnerGeneration;
        if (ToWireId(envelope.AggregateId) != prepare.RelocationId
            || envelope.AggregateGeneration != 1
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
            || relocating.RelocationId != envelope.AggregateId
            || relocating.Phase != ZLinkActorRelocationAuthorityPhase.Activated
            || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                relocating.ApplicationPayload.Span,
                out var targetAuthority)
            || targetAuthority.ActorId != prepare.Object.ObjectId
            || targetAuthority.StableType != recovery.StableType
            || targetAuthority.NodeRid != prepare.Target.NodeRid
            || targetAuthority.NodeGeneration != prepare.Target.NodeGeneration
            || targetAuthority.OwnerId != prepare.Target.OwnerId
            || targetAuthority.OwnerLeaseGeneration
               != prepare.Target.OwnerLeaseGeneration)
            throw DataLost(
                "Standalone Actor relocation root does not match command 40.");
        if (effectiveTargetAuthorityOwnerGeneration is 0 or > long.MaxValue
            || effectiveTargetAuthorityOwnerGeneration
               <= participant.AuthorityOwnerGeneration)
            throw DataLost(
                "Standalone Actor reservation returned an invalid target authority generation.");

        var node = runtime.GetSpotNodeRuntime(prepare.Target.NodeRid);
        var targetActivation = remoteJoinRecovery is null
            ? ResolveTargetMembership(node, targetAuthority)
            : null;
        if (!node.Registration.ActorRelocations.TryGetValue(
                recovery.StableType,
                out var relocation)
            || relocation.PolicyKind == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Actor type '{recovery.StableType}' relocation is not registered on the target.");

        var frames = DecodeAcceptedRecords(participant.AcceptedJobs)
            .Select(static accepted => accepted.Frame)
            .ToArray();
        ZLinkRemoteActorJoinRequest? remoteJoinRequest = null;
        if (remoteJoinRecovery is not null)
        {
            //  Direct transfer keeps the durable "pending" sentinel: no
            //  immutable store root exists for the handoff payload anymore.
            var candidate = remoteJoinRecovery.Request with
            {
                RelocationAggregateId = envelope.AggregateId,
                RelocationAggregateGeneration = envelope.AggregateGeneration,
                // The direct relocation-envelope-v1 logical stream carries no
                // provider inventory envelope.  Its decoded digest is the
                // pending sentinel; ZLinkActorRelocationRoot reconstructs the
                // derived digest from frozen recovery before using the root.
                RelocationInventoryDigest = envelope.InventoryDigest.ToArray(),
                HandoffFrames = []
            };
            remoteJoinRequest = ZLinkActorRelocationRoot.WithDurableFrames(
                candidate,
                ZLinkActorRelocationRoot.Load(candidate, envelope));
        }

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
                    effectiveTargetAuthorityOwnerGeneration,
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
                envelope.AggregateId.ToString("N"),
                relocating.BoundSessionRoute,
                remoteJoinRequest is not null
                    ? ZLinkRemoteActorJoinPackets
                        .DecodeSessionRelocationContext(remoteJoinRequest)
                    : new ZLinkSessionRelocationContext(
                        prepare.RelocationId,
                        prepare.Coordinator));
            if (remoteJoinRequest is not null)
                await runtime.PrepareCanonicalRoutedActorJoinTargetAsync(
                        remoteJoinRecovery!.TargetSpotId,
                        actorState,
                        remoteJoinRequest,
                        cancellationToken)
                    .ConfigureAwait(false);
            else
                actorState.Handoff.BeginCanonicalMaintenanceImport(
                    envelope.AggregateId.ToString("N"),
                    frames);
            var stage = new TargetStage(
                prepare,
                authenticatedSourceNodeRid,
                envelope,
                actorState,
                targetAuthority,
                frames,
                effectiveTargetAuthorityOwnerGeneration,
                targetActivation,
                remoteJoinRequest,
                remoteJoinRecovery,
                created);
            if (!slot.TrySetStage(stage))
                throw new InvalidOperationException(
                    "Standalone Actor target stage owner changed under its attempt gate.");
            stage.BeginReadySubmission();
        }
        catch
        {
            if (remoteJoinRequest is not null)
                await runtime.AbortCanonicalRoutedActorJoinTargetAsync(
                        remoteJoinRecovery!.TargetSpotId,
                        actorState,
                        remoteJoinRequest,
                        created)
                    .ConfigureAwait(false);
            if (attached && actorState.Actor is { } attachedActor)
                targetActivation!.AbortStagedRelocatedPerActorMember(
                    attachedActor,
                    actorState);
            if (remoteJoinRequest is null && created)
                await actorSessions.RollbackTransferredActorAsync(
                        targetAuthority.ActorId,
                        CancellationToken.None)
                    .ConfigureAwait(false);
            throw;
        }
    }

    internal static bool TryReadRoutedJoinSavedWork(
        ZLinkRelocationParticipantEnvelope participant,
        out ZLinkActorRelocationSourceFence source,
        out ZLinkActorRelocationRecoveryRecord recovery)
    {
        source = default!;
        recovery = default!;
        var matches = participant.AcceptedJobs
            .Where(job => ZLinkActorRemoteJoinRecoverySavedWork.TryDecode(
                job.Payload.Span, out _, out _))
            .ToArray();
        if (matches.Length != 1
            || !ZLinkActorRemoteJoinRecoverySavedWork.TryDecode(
                matches[0].Payload.Span, out source, out recovery))
            return false;
        return true;
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

    private static void MarkAuthorityPublished(TargetStage stage)
    {
        if (stage.AuthorityPublished) return;
        var actorRef = stage.ActorState.NativeActorRef
                       ?? throw DataLost(
                           "Standalone Actor target native reference is unavailable.");
        var aggregateId = stage.Envelope.AggregateId.ToString("N");
        stage.ActorState.MarkRelocationSessionAuthorityCommitted(
            aggregateId,
            actorRef,
            stage.TargetAuthorityOwnerGeneration,
            ZLinkMeshName.FromBoundary(
                stage.TargetAuthority.MeshName,
                nameof(stage.TargetAuthority.MeshName)),
            stage.TargetAuthority.NodeGeneration,
            stage.TargetAuthority.OwnerLeaseGeneration);
        if (!stage.ActorState.Handoff.IsAuthorityCommitted(aggregateId))
            stage.ActorState.Handoff.MarkAuthorityCommitted(
                aggregateId,
                stage.Participant.ObjectGeneration,
                actorRef.Generation);
        if (stage.TargetActivation is { } activation
            && stage.ActorState.Actor is { } actor)
            activation.PublishRelocatedPerActorMember(
                actor,
                stage.ActorState);
        stage.AuthorityPublished = true;
    }

    // Direct transfer owns its verified payload in memory; unlike recovery it
    // has no Relocation Store reference to advance. The stage always carries a
    // remote-join import (Handoff.Import), never a canonical maintenance
    // import, so the canonical activation helper does not apply here — the
    // Runtime helper drives the imported replay itself, then finishes the
    // target route and delivers the public Join completion. The durable
    // canonical phase then advances to Activated so the source's
    // committed-target wait (which requires activation before it completes
    // the source side, spec 15 §4.2 step 7) observes the target opening — without it
    // the source spins on a Committed row until its join deadline and
    // falsely rolls back an already-committed handoff.
    private async ValueTask CompleteDirectRemoteJoinTargetAsync(
        TargetStage stage,
        ZLinkRemoteActorJoinRequest request,
        ZLinkActorRelocationRecoveryRecord recovery,
        CancellationToken cancellationToken)
    {
        await runtime.CompleteDirectCanonicalRoutedActorJoinAsync(
                recovery.TargetSpotId,
                stage.ActorState,
                request,
                recovery,
                stage.TargetAuthority.MeshName,
                token => AdvanceDirectRemoteJoinTargetPhaseAsync(stage, token),
                cancellationToken)
            .ConfigureAwait(false);
    }

    //  The recovery progress coordinator resolves the immutable root through
    //  its Relocation Store pointer, which a direct transfer never writes
    //  (spec 52 §3.1.7), so the direct remote join advances the canonical
    //  phase on the authority row itself — Committed → Activating →
    //  Activated, one durable boundary per CAS, fenced by the exact
    //  relocation identity and target attempt — and then normalizes the row
    //  to its steady target-owned authority payload, mirroring the
    //  store-based recovery release.
    private async ValueTask AdvanceDirectRemoteJoinTargetPhaseAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        var store = registration.Locations.ResolveStore()
                    ?? throw new ZLinkConfigurationException(
                        "Location Store is not registered.");
        var relocationId = ToWireId(stage.Envelope.AggregateId);
        for (var attempt = 0; attempt < 16; attempt++)
        {
            var read = await store.ReadAuthorityAsync(
                    stage.Participant.AuthorityKey,
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw DataLost(
                    "Standalone Actor direct target activation lost its committed authority.");
            if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out var canonical))
                //  Already normalized to the steady target authority.
                return;
            if (canonical.RelocationHigh != relocationId.High
                || canonical.RelocationLow != relocationId.Low
                || canonical.TargetAttemptGeneration
                   != stage.Prepare.TargetAttemptGeneration
                || !StringComparer.Ordinal.Equals(
                    canonical.TargetOwnerId,
                    stage.TargetAuthority.OwnerId)
                || canonical.TargetOwnerLeaseGeneration
                   != stage.TargetAuthority.OwnerLeaseGeneration)
                throw DataLost(
                    "Standalone Actor direct target activation lost its exact attempt fence.");
            ReadOnlyMemory<byte> payload;
            if (canonical.Phase
                >= (byte)ZLinkStandaloneActorCanonicalPhase.Activated)
                payload = canonical.SteadyAuthorityPayload;
            else if (canonical.Phase is
                     (byte)ZLinkStandaloneActorCanonicalPhase.Committed or
                     (byte)ZLinkStandaloneActorCanonicalPhase.Activating)
            {
                var next = canonical.Phase
                           == (byte)ZLinkStandaloneActorCanonicalPhase.Committed
                    ? ZLinkStandaloneActorCanonicalPhase.Activating
                    : ZLinkStandaloneActorCanonicalPhase.Activated;
                payload = ZLinkCanonicalRelocationAuthorityStateCodec
                    .ReplaceRelocationState(
                        found.Snapshot.Payload.Span,
                        canonical.State with { Phase = (byte)next },
                        stage.Envelope);
            }
            else
                throw DataLost(
                    $"Standalone Actor direct target activation phase is '{canonical.Phase}'.");
            var exchanged = await store.CompareExchangeAuthorityAsync(
                    stage.Participant.AuthorityKey,
                    found.Snapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        payload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null),
                    cancellationToken)
                .ConfigureAwait(false);
            if (exchanged is ZLinkAuthorityCompareExchangeResult.Stored
                or ZLinkAuthorityCompareExchangeResult.Conflict)
                continue;
            throw new InvalidOperationException(
                "Authority Store rejected direct standalone Actor phase progress.");
        }
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            "Standalone Actor direct target activation conflicted after the bounded retry limit.",
            retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private async ValueTask ActivatePublishedTargetCoreAsync(
        TargetStage stage,
        CancellationToken cancellationToken)
    {
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
                stage.Prepare.TargetAttemptGeneration,
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

    private async ValueTask FinishTargetStageAsync(
        AttemptKey key,
        AttemptSlot slot,
        TargetStage stage,
        CancellationToken cancellationToken)
    {
        if (!stage.AuthorityPublished)
            throw DataLost(
                "Standalone Actor target cannot open before its owner CAS.");
        if (!await IsCurrentTargetAttemptAsync(stage, cancellationToken)
                .ConfigureAwait(false))
        {
            RemoveTargetStage(key, slot, stage);
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
        RemoveTargetStage(key, slot, stage);
    }

    internal async ValueTask ReconcilePublishedTargetAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        CancellationToken cancellationToken)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!TryAcquireTargetAttempt(key, out var lease))
            return;
        using var attemptLease = lease;
        try
        {
        await lease.Slot.RunAsync(async () =>
        {
        if (lease.Slot.Stage is not { } stage
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
            RemoveTargetStage(key, lease.Slot, stage);
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
            RemoveTargetStage(key, lease.Slot, stage);
            return;
        }
        if (relocating.Phase == ZLinkActorRelocationAuthorityPhase.Steady)
        {
            RemoveTargetStage(key, lease.Slot, stage);
            return;
        }
        if (canonical.Phase is (
                (byte)ZLinkStandaloneActorCanonicalPhase.Committed
                or (byte)ZLinkStandaloneActorCanonicalPhase.Activating))
        {
            await ActivatePublishedTargetCoreAsync(stage, cancellationToken)
                .ConfigureAwait(false);
            await FinishTargetStageAsync(key, lease.Slot, stage, cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        if (canonical.Phase is not (
                (byte)ZLinkStandaloneActorCanonicalPhase.Activated
                or (byte)ZLinkStandaloneActorCanonicalPhase.Cleaning
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
        RemoveTargetStage(key, lease.Slot, stage);
        }).ConfigureAwait(false);
        }
        finally
        {
            CloseTargetAttemptIfEmpty(key, lease.Slot);
        }
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
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid)
    {
        var key = new AttemptKey(
            prepare.RelocationId.High,
            prepare.RelocationId.Low,
            prepare.TargetAttemptGeneration);
        if (!TryAcquireTargetAttempt(key, out var lease))
            return;
        using var attemptLease = lease;
        try
        {
            await lease.Slot.RunAsync(() =>
            {
            if (lease.Slot.Abort is { } existingAbort)
            {
                existingAbort.Stage.ValidateRetry(
                    prepare,
                    authenticatedSourceNodeRid);
                return ValueTask.CompletedTask;
            }
            if (lease.Slot.Stage is not { } stage
                || stage.AuthorityPublished)
                return ValueTask.CompletedTask;
            stage.ValidateRetry(prepare, authenticatedSourceNodeRid);
            _ = BeginTargetAbortLocked(key, lease.Slot, stage);
            return ValueTask.CompletedTask;
            }).ConfigureAwait(false);
        }
        finally
        {
            CloseTargetAttemptIfEmpty(key, lease.Slot);
        }
    }

    /// <summary>
    /// Spec 15 §4.2 newest-attempt-wins boundary for canonical Actor Join.
    /// A later admission has already displaced any pre-PREPARE temporary
    /// queue when it reaches here. If the displaced identity has since
    /// installed a target stage, reuse its normal target-abort path and wait
    /// for the cleanup to finish before the later admission can return
    /// Accepted. AuthorityPublished is the CAS boundary: from that point the
    /// winner is fixed and a later admission must not reopen target staging.
    /// </summary>
    internal async ValueTask AbortSupersededRoutedActorJoinPreparationAsync(
        string actorId,
        ulong actorGeneration,
        string newerHandoffId,
        CancellationToken cancellationToken)
    {
        foreach (var pair in _targetAttempts.ToArray())
        {
            // Spec 42: admission runs under the target Spot application turn.
            // Waiting for an unrelated relocation gate here can retain that
            // turn while the relocation owning the gate waits for another
            // Spot turn, forming a cross-Spot cycle. The routed-join identity
            // is published before target preparation takes its gate, so only
            // the exact Actor candidate needs the serialized recheck below.
            if (!pair.Value.MayOwnSupersededRoutedJoinPreparation(
                    actorId,
                    actorGeneration,
                    newerHandoffId))
                continue;
            if (!TryAcquireTargetAttempt(pair.Key, pair.Value, out var lease))
                continue;

            Task? terminal = null;
            try
            {
                try
                {
                    terminal = await lease.Slot.RunAsync(() =>
                    {
                    var stage = lease.Slot.Stage;
                    var request = stage?.RemoteJoinRequest;
                    if (stage is null
                        || request is null
                        || stage.AuthorityPublished
                        || string.Equals(
                            request.HandoffId,
                            newerHandoffId,
                            StringComparison.Ordinal)
                        || !string.Equals(
                            request.ActorId,
                            actorId,
                            StringComparison.Ordinal)
                        || request.ActorGeneration != actorGeneration)
                        return (Task?)null;

                    return BeginTargetAbortLocked(
                        pair.Key,
                        lease.Slot,
                        stage)?.Terminal;
                    }).ConfigureAwait(false);
                }
                finally
                {
                    CloseTargetAttemptIfEmpty(pair.Key, lease.Slot);
                }
            }
            finally
            {
                lease.Dispose();
            }

            if (terminal is not null)
                await terminal.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
    }

    private TargetAbort? BeginTargetAbortLocked(
        AttemptKey key,
        AttemptSlot slot,
        TargetStage stage)
    {
        if (!stage.TryBeginAbort(() => slot.TryRemoveStage(stage)))
            return null;
        var abort = new TargetAbort(stage);
        if (!slot.TrySetAbort(abort))
            throw new InvalidOperationException(
                "Standalone Actor target abort barrier could not be registered.");
        var forceStopToken = runtime.ForceStopToken;
        if (runtime.TryRunDetached(
                "standalone-actor-target-abort",
                _ => CompleteTargetAbortAsync(key, abort, forceStopToken)))
            return abort;

        slot.TryRemoveAbort(abort);
        stage.RestorePendingAfterAbortSchedulingFailure();
        if (!slot.TrySetStage(stage))
            throw new InvalidOperationException(
                "Standalone Actor target abort scheduling rejection lost its prepared stage.");
        throw new InvalidOperationException(
            "Standalone Actor target abort could not enter the runtime task scope.");
    }

    private async ValueTask CompleteTargetAbortAsync(
        AttemptKey key,
        TargetAbort abort,
        CancellationToken cancellationToken)
    {
        var completed = false;
        try
        {
            var stage = abort.Stage;
            if (stage.RemoteJoinRequest is { } remoteJoinRequest
                && stage.RemoteJoinRecovery is { } remoteJoinRecovery)
            {
                if (!stage.CreatedTransferredActor)
                    DetachTargetMembership(stage);
                await runtime.RollbackCanonicalRoutedActorJoinPreparationAsync(
                        remoteJoinRecovery.TargetSpotId,
                        stage.ActorState,
                        remoteJoinRequest,
                        stage.CreatedTransferredActor,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            else
            {
                DetachTargetMembership(stage);
                stage.ActorState.AbortRelocationSessionRoute(
                    stage.Envelope.AggregateId.ToString("N"));
                await actorSessions.RollbackTransferredActorAsync(
                        stage.ActorState.ActorId,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            abort.Complete();
            completed = true;
        }
        finally
        {
            if (!completed)
            {
                if (TryAcquireTargetAttempt(key, out var lease))
                    using (lease)
                    {
                        lease.Slot.TryRemoveAbort(abort);
                        CloseTargetAttemptIfEmpty(key, lease.Slot);
                    }
            }
            abort.Terminate();
        }
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

    private void RemoveTargetStage(
        AttemptKey key,
        AttemptSlot slot,
        TargetStage stage)
    {
        if (!slot.TryRemoveStage(stage))
            return;
        CloseTargetAttemptIfEmpty(key, slot);
    }

    internal async ValueTask SealAndDrainTargetAttemptsAsync(
        CancellationToken cancellationToken)
    {
        Interlocked.Exchange(ref _targetAttemptAdmissionSealed, 1);
        var slots = _targetAttempts.Values.Distinct().ToArray();
        foreach (var slot in slots) slot.MarkClosing();
        await Task.WhenAll(slots.Select(static slot => slot.Quiesced))
            .WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        await Task.WhenAll(
                slots.Select(static slot => slot.Abort?.Terminal)
                    .OfType<Task>())
            .WaitAsync(cancellationToken)
            .ConfigureAwait(false);
    }

    internal void CompleteTargetGenerationReset()
    {
        foreach (var pair in _targetAttempts)
        {
            pair.Value.Reset();
            pair.Value.MarkClosing();
            TryRemoveClosedTargetAttempt(pair.Key, pair.Value);
        }
        Volatile.Write(ref _targetAttemptAdmissionSealed, 0);
    }

    private AttemptSlotLease AcquireTargetAttempt(AttemptKey key)
    {
        while (true)
        {
            if (Volatile.Read(ref _targetAttemptAdmissionSealed) != 0)
                throw TargetAttemptAdmissionSealed();
            var slot = _targetAttempts.GetOrAdd(
                key,
                static _ => new AttemptSlot());
            if (Volatile.Read(ref _targetAttemptAdmissionSealed) != 0)
            {
                slot.MarkClosing();
                TryRemoveClosedTargetAttempt(key, slot);
                throw TargetAttemptAdmissionSealed();
            }
            if (slot.TryAcquire())
                return new AttemptSlotLease(this, key, slot);
            if (slot.IsEmpty
                && slot.CanRemove
                && _targetAttempts.TryRemove(
                    new KeyValuePair<AttemptKey, AttemptSlot>(key, slot)))
                continue;
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Standalone Actor target attempt is closing.",
                retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
        }
    }

    private static ZLinkFrameworkException TargetAttemptAdmissionSealed() =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            "Standalone Actor target attempt admission is sealed.",
            retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);

    private bool TryAcquireTargetAttempt(
        AttemptKey key,
        out AttemptSlotLease lease)
    {
        if (_targetAttempts.TryGetValue(key, out var slot)
            && slot.TryAcquire())
        {
            lease = new AttemptSlotLease(this, key, slot);
            return true;
        }
        if (slot is not null)
            TryRemoveClosedTargetAttempt(key, slot);
        lease = default;
        return false;
    }

    private bool TryAcquireTargetAttempt(
        AttemptKey key,
        AttemptSlot slot,
        out AttemptSlotLease lease)
    {
        if (_targetAttempts.TryGetValue(key, out var current)
            && ReferenceEquals(current, slot)
            && slot.TryAcquire())
        {
            lease = new AttemptSlotLease(this, key, slot);
            return true;
        }
        TryRemoveClosedTargetAttempt(key, slot);
        lease = default;
        return false;
    }

    private void CloseTargetAttemptIfEmpty(AttemptKey key, AttemptSlot slot)
    {
        if (!slot.IsEmpty) return;
        slot.MarkClosing();
        TryRemoveClosedTargetAttempt(key, slot);
    }

    private void ReleaseTargetAttempt(AttemptKey key, AttemptSlot slot)
    {
        if (slot.Release())
            TryRemoveClosedTargetAttempt(key, slot);
    }

    private void TryRemoveClosedTargetAttempt(AttemptKey key, AttemptSlot slot)
    {
        if (slot.IsEmpty && slot.CanRemove)
            _targetAttempts.TryRemove(
                new KeyValuePair<AttemptKey, AttemptSlot>(key, slot));
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
                .Where(static job => !ZLinkActorRemoteJoinRecoverySavedWork.TryDecode(
                    job.Payload.Span, out _, out _))
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
            || prepare.InitiatorRole != 1
            || prepare.TargetAttemptGeneration == 0
            || prepare.SourceNodeRid != authenticatedSourceNodeRid
            || prepare.Coordinator.NodeRid != authenticatedSourceNodeRid
            || prepare.Coordinator.NodeGeneration
               != prepare.SourceNodeGeneration)
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

    private readonly struct AttemptSlotLease : IDisposable
    {
        private readonly ZLinkStandaloneActorRelocationRuntime _owner;
        private readonly AttemptKey _key;
        internal AttemptSlot Slot { get; }

        internal AttemptSlotLease(
            ZLinkStandaloneActorRelocationRuntime owner,
            AttemptKey key,
            AttemptSlot slot)
        {
            _owner = owner;
            _key = key;
            Slot = slot;
        }

        public void Dispose() => _owner.ReleaseTargetAttempt(_key, Slot);
    }

    private sealed class AttemptSlot
    {
        private readonly ZLinkStateLane _lane = new();
        private readonly ZLinkRelocationAttemptLeaseState _leases = new();
        private RoutedJoinPreparationIdentity? _routedJoinPreparation;
        private TargetStage? _stage;
        private TargetAbort? _abort;

        internal TargetStage? Stage => Volatile.Read(ref _stage);
        internal TargetAbort? Abort => Volatile.Read(ref _abort);
        internal bool IsEmpty => Stage is null && Abort is null;
        internal Task Quiesced => _leases.Quiesced;

        internal bool TryAcquire() => _leases.TryAcquire();

        internal bool Release() => _leases.Release();

        internal void MarkClosing() => _leases.MarkClosing();

        internal bool CanRemove => _leases.CanRemove;

        internal void ObserveRoutedJoinPreparation(
            ZLinkRemoteActorJoinRequest request) =>
            Interlocked.CompareExchange(
                ref _routedJoinPreparation,
                new RoutedJoinPreparationIdentity(
                    request.ActorId,
                    request.ActorGeneration,
                    request.HandoffId),
                null);

        internal bool MayOwnSupersededRoutedJoinPreparation(
            string actorId,
            ulong actorGeneration,
            string newerHandoffId)
        {
            var preparation = Volatile.Read(ref _routedJoinPreparation);
            return preparation is not null
                   && preparation.ActorGeneration == actorGeneration
                   && string.Equals(
                       preparation.ActorId,
                       actorId,
                       StringComparison.Ordinal)
                   && !string.Equals(
                       preparation.HandoffId,
                       newerHandoffId,
                       StringComparison.Ordinal);
        }

        internal bool TrySetStage(TargetStage stage) =>
            Interlocked.CompareExchange(ref _stage, stage, null) is null;

        internal bool TryRemoveStage(TargetStage stage) =>
            ReferenceEquals(
                Interlocked.CompareExchange(ref _stage, null, stage),
                stage);

        internal bool TrySetAbort(TargetAbort abort) =>
            Interlocked.CompareExchange(ref _abort, abort, null) is null;

        internal bool TryRemoveAbort(TargetAbort abort) =>
            ReferenceEquals(
                Interlocked.CompareExchange(ref _abort, null, abort),
                abort);

        internal void Reset()
        {
            Volatile.Write(ref _routedJoinPreparation, null);
            Volatile.Write(ref _stage, null);
            Volatile.Write(ref _abort, null);
        }

        internal ValueTask RunAsync(Func<ValueTask> work)
        {
            _lane.ThrowIfReentrant();
            var completion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            if (!_lane.TryPost(async () =>
                {
                    try
                    {
                        await work().ConfigureAwait(false);
                        completion.TrySetResult();
                    }
                    catch (Exception exception)
                    {
                        completion.TrySetException(exception);
                    }
                }))
                completion.TrySetException(new ObjectDisposedException(nameof(AttemptSlot)));
            return new ValueTask(completion.Task);
        }

        internal async ValueTask<T> RunAsync<T>(Func<T> work) =>
            await _lane.RunAsync(work).ConfigureAwait(false);
    }

    private sealed record RoutedJoinPreparationIdentity(
        string ActorId,
        ulong ActorGeneration,
        string HandoffId);

    private sealed class TargetAbort(TargetStage stage)
    {
        private int _completed;
        private readonly TaskCompletionSource _terminal = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        internal TargetStage Stage { get; } = stage;
        internal DateTimeOffset CreatedAt { get; } = TimeProvider.System.GetUtcNow();
        internal bool IsCompleted => Volatile.Read(ref _completed) != 0;
        internal Task Terminal => _terminal.Task;

        internal void Complete() => Volatile.Write(ref _completed, 1);
        internal void Terminate() => _terminal.TrySetResult();
    }

    private sealed class TargetStage(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId sourceNodeRid,
        ZLinkRelocationEnvelope envelope,
        ZLinkActorRuntimeState actorState,
        ZLinkActorAuthorityPayload targetAuthority,
        IReadOnlyList<ZLinkActorHandoffFrame> acceptedFrames,
        ulong targetAuthorityOwnerGeneration,
        ZLinkSpotActivation? targetActivation,
        ZLinkRemoteActorJoinRequest? remoteJoinRequest,
        ZLinkActorRelocationRecoveryRecord? remoteJoinRecovery,
        bool createdTransferredActor)
    {
        private readonly ZLinkStateLane _lane = new();
        private readonly List<ZLinkActorHandoffFrame> _acceptedFrames =
            [.. acceptedFrames];
        private TargetReadySubmissionPhase _readySubmissionPhase;
        internal ZLinkServiceWireCodec.RelocationPrepareRecord Prepare { get; } = prepare;
        internal RoutingId SourceNodeRid { get; } = sourceNodeRid;
        internal ZLinkRelocationEnvelope Envelope { get; } = envelope;
        internal ZLinkRelocationParticipantEnvelope Participant
            => Envelope.Participants[0];
        internal ZLinkActorRuntimeState ActorState { get; } = actorState;
        internal ZLinkSpotActivation? TargetActivation { get; } =
            targetActivation;
        internal ZLinkRemoteActorJoinRequest? RemoteJoinRequest { get; } =
            remoteJoinRequest;
        internal ZLinkActorRelocationRecoveryRecord? RemoteJoinRecovery { get; } =
            remoteJoinRecovery;
        internal bool CreatedTransferredActor { get; } = createdTransferredActor;
        internal ZLinkActorAuthorityPayload TargetAuthority { get; } = targetAuthority;
        internal ulong TargetAuthorityOwnerGeneration { get; } =
            targetAuthorityOwnerGeneration;
        internal IReadOnlyList<ZLinkActorHandoffFrame> AcceptedFrames
            => _acceptedFrames;
        internal long NextArrivalIndex => _acceptedFrames.Count == 0
            ? 1
            : checked(_acceptedFrames[^1].ArrivalIndex + 1);
        internal DateTimeOffset CreatedAt { get; } = TimeProvider.System.GetUtcNow();
        internal bool AuthorityPublished { get; set; }

        private uint _relayCrcState = uint.MaxValue;
        private ulong _relayRecordCount;

        internal void Append(ZLinkActorHandoffFrame frame) =>
            _acceptedFrames.Add(frame);

        internal void TrackRelayRecord(ReadOnlySpan<byte> encodedFrozenRecord)
        {
            ZLinkCrc32C.Append(ref _relayCrcState, encodedFrozenRecord);
            _relayRecordCount = checked(_relayRecordCount + 1);
        }

        //  Spec 28 §4.4: on the ordered connection the boundary values always
        //  match the staged relay span; a mismatch is an implementation
        //  defect, not a retryable condition.
        internal void ValidateBoundary(
            ZLinkServiceWireCodec.RelocationCutoverRecord cutover)
        {
            if (cutover.BoundaryRecordCount != _relayRecordCount
                || cutover.BoundaryChecksumCrc32c != ~_relayCrcState)
                throw DataLost(
                    "Command 34 boundary values do not match the staged relay span.");
        }

        internal void BeginReadySubmission()
        {
            AwaitStateLane(_lane.RunAsync(() =>
            {
                switch (_readySubmissionPhase)
                {
                    case TargetReadySubmissionPhase.None:
                        _readySubmissionPhase = TargetReadySubmissionPhase.Pending;
                        return;
                    //  An identical-retry Prepare that arrives while a READY
                    //  submission is already in flight (or has landed)
                    //  attaches to that submission instead of failing: READY
                    //  is a one-way submission with no completion reply
                    //  (spec 52 §4.1), duplicate READY records are idempotent
                    //  at the source, and NACKing here would fault the shared
                    //  pending-prepare waiter the original caller still owns.
                    case TargetReadySubmissionPhase.Pending:
                    case TargetReadySubmissionPhase.Submitted:
                        return;
                    default:
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            "Standalone Actor target cleanup is still in progress.",
                            retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
                }
            }));
        }

        internal bool TryMarkReadySubmitted(Func<bool> isCurrent)
        {
            var claimed = AwaitStateLane(_lane.RunAsync(() =>
            {
                if (_readySubmissionPhase == TargetReadySubmissionPhase.Aborting)
                    return false;
                if (_readySubmissionPhase == TargetReadySubmissionPhase.Submitted)
                    return false;
                if (_readySubmissionPhase != TargetReadySubmissionPhase.Pending)
                    throw DataLost(
                        "Standalone Actor READY submission has no prepared owner.");
                _readySubmissionPhase = TargetReadySubmissionPhase.Submitted;
                return true;
            }));
            // The state transition is claimed in the lane before this
            // caller-owned check. It may re-enter the relocation owner.
            return claimed && isCurrent();
        }

        internal bool TryMarkReadySubmissionFailed(Func<bool> isCurrent)
        {
            var claimed = AwaitStateLane(_lane.RunAsync(() =>
            {
                if (_readySubmissionPhase != TargetReadySubmissionPhase.Pending)
                    return false;
                _readySubmissionPhase = TargetReadySubmissionPhase.None;
                return true;
            }));
            return claimed && isCurrent();
        }

        internal bool TryBeginAbort(Func<bool> remove)
        {
            var claimed = AwaitStateLane(_lane.RunAsync(() =>
            {
                if (_readySubmissionPhase != TargetReadySubmissionPhase.Pending)
                    return false;
                _readySubmissionPhase = TargetReadySubmissionPhase.Aborting;
                return true;
            }));
            if (!claimed)
                return false;
            // Removal is caller-owned and can re-enter the attempt owner, so
            // it deliberately runs outside the TargetStage state lane.
            if (remove())
                return true;
            AwaitStateLane(_lane.RunAsync(() =>
            {
                if (_readySubmissionPhase == TargetReadySubmissionPhase.Aborting)
                    _readySubmissionPhase = TargetReadySubmissionPhase.Pending;
            }));
            return false;
        }

        internal void RestorePendingAfterAbortSchedulingFailure()
        {
            AwaitStateLane(_lane.RunAsync(() =>
            {
                if (_readySubmissionPhase != TargetReadySubmissionPhase.Aborting)
                    throw DataLost(
                        "Standalone Actor abort scheduling rejection lost its READY owner.");
                _readySubmissionPhase = TargetReadySubmissionPhase.Pending;
            }));
        }

        private static T AwaitStateLane<T>(ValueTask<T> operation) =>
            operation.GetAwaiter().GetResult();

        private static void AwaitStateLane(ValueTask operation) =>
            operation.GetAwaiter().GetResult();

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

        internal void ValidateData(
            ZLinkServiceWireCodec.RelocationDataRecord data,
            RoutingId authenticatedSourceNodeRid)
        {
            if (AuthorityPublished)
                throw DataLost(
                    "Standalone Actor command 31 changed its prepared attempt.");
            ValidateExactAttemptFence(
                authenticatedSourceNodeRid,
                data.SenderRole,
                data.RelocationId,
                data.TargetAttemptGeneration,
                data.Coordinator,
                data.Object,
                "Standalone Actor command 31 changed its prepared attempt.");
        }

        internal void ValidateCutover(
            ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
            RoutingId authenticatedSourceNodeRid) =>
            ValidateExactAttemptFence(
                authenticatedSourceNodeRid,
                cutover.SenderRole,
                cutover.RelocationId,
                cutover.TargetAttemptGeneration,
                cutover.Coordinator,
                cutover.Object,
                "Standalone Actor command 34 changed its prepared attempt.");

        //  Both command 31 (Data) and command 34 (Cutover) must still carry
        //  the exact identity this target attempt was Prepared with — same
        //  sender, same relocation/attempt/coordinator fence, same object.
        //  Owning the comparison here keeps the two commands from drifting
        //  when the fence grows a field.
        private void ValidateExactAttemptFence(
            RoutingId authenticatedSourceNodeRid,
            byte senderRole,
            ZLinkServiceWireCodec.RelocationWireId relocationId,
            ulong targetAttemptGeneration,
            ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator,
            ZLinkServiceWireCodec.RelocationObjectRecord relocationObject,
            string errorMessage)
        {
            if (SourceNodeRid != authenticatedSourceNodeRid
                || senderRole != 1
                || relocationId != Prepare.RelocationId
                || targetAttemptGeneration != Prepare.TargetAttemptGeneration
                || coordinator != Prepare.Coordinator
                || relocationObject != Prepare.Object)
                throw DataLost(errorMessage);
        }
    }

    private enum TargetReadySubmissionPhase
    {
        None = 0,
        Pending = 1,
        Submitted = 2,
        Aborting = 3
    }
}

internal sealed class ZLinkRelocationAttemptLeaseState
{
    private readonly ZLinkStateLane _lane = new();
    private readonly TaskCompletionSource _quiesced = new(
        TaskCreationOptions.RunContinuationsAsynchronously);
    private int _users;
    private bool _closing;

    internal Task Quiesced => _quiesced.Task;

    internal bool TryAcquire()
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_closing) return false;
            _users++;
            return true;
        }));
    }

    internal bool Release()
    {
        var quiesced = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_users == 0)
                throw new InvalidOperationException(
                    "Relocation attempt lease was released without an owner.");
            _users--;
            return _closing && _users == 0;
        }));
        if (quiesced) _quiesced.TrySetResult();
        return quiesced;
    }

    internal void MarkClosing()
    {
        var quiesced = AwaitStateLane(_lane.RunAsync(() =>
        {
            _closing = true;
            return _users == 0;
        }));
        if (quiesced) _quiesced.TrySetResult();
    }

    internal bool CanRemove
    {
        get => AwaitStateLane(_lane.RunAsync(() => _closing && _users == 0));
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
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
        var resolution = actorState.Handoff.RouteFrame(
            sourceActorRef,
            sourceActorRef);
        if (resolution.Route != ZLinkActorFrameRoute.MessageFollow
            || resolution.MessageFollowRoute is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorState.ActorId}' committed Message Follow route is unavailable.");
        var deliveries = new List<Task<bool>>(frames.Count);
        foreach (var frame in frames.OrderBy(static frame => frame.ArrivalIndex))
        {
            using var body = Message.From(frame.Body);
            deliveries.Add(ActorMessageFollower.EnqueueTracked(
                resolution.MessageFollowRoute.Value,
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
        if (progress.Phase.Phase is not (
                ZLinkActorRelocationAuthorityPhase.Activated
                or ZLinkActorRelocationAuthorityPhase.Cleaning
                or ZLinkActorRelocationAuthorityPhase.Completed))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Actor '{actorState.ActorId}' target completion phase is not recoverable.");
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
        ZLinkActorRelocationSourceFence source;
        if (!initialParticipant.RecoveryPayload.IsEmpty)
        {
            var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                initialParticipant.RecoveryPayload.Span);
            source = ZLinkActorRelocationSourceFenceCodec.Decode(
                recovery.MembershipMutation.Span);
        }
        else if (!ZLinkStandaloneActorRelocationRuntime.TryReadRoutedJoinSavedWork(
                     initialParticipant, out source, out _))
        {
            if (progress.Canonical is not { } canonical)
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor replay has no canonical source fence.");
            source = new ZLinkActorRelocationSourceFence(
                canonical.State.SourceOwnerId,
                canonical.State.SourceOwnerLeaseGeneration,
                RoutingId.From(canonical.State.SourceNodeRid),
                canonical.State.SourceNodeGeneration);
        }
        var sourceActor = new ZLinkBackendActorRef(
            source.NodeRid,
            actorState.ActorId,
            initialParticipant.ObjectGeneration);
        var pipeline = new ZLinkActorInboundPipeline(
            this,
            new ZLinkEntrySpotActorInboundEndpoint(this));
        var participant = progress.Root.Participants.Single();
        var pendingJobs = participant.AcceptedJobs
            .Where(static job => !ZLinkActorRemoteJoinRecoverySavedWork.TryDecode(
                job.Payload.Span, out _, out _))
            .OrderBy(static candidate => candidate.AcceptedSequence)
            .ToArray();
        var barrier = actorState.ReserveHandoffRestoreBarrier();
        var turn = await barrier.ClaimAsync().ConfigureAwait(false);
        // The committed target seal captures direct ingress in the durable
        // tail. Once the lifecycle barrier is reached, replay turns must be
        // able to start so each one can release its shared queued permit and
        // admit the next item incrementally.
        turn.Dispose();
        var queued = new List<Task>(pendingJobs.Length);
        var queuedThrough = checked((long)pendingJobs
            .Select(static job => job.AcceptedSequence)
            .DefaultIfEmpty(0UL)
            .Max());
        await QueueDurableBacklogWithSharedPermitsAsync(
                GetOrStartState().ApplicationJobQueue,
                pendingJobs,
                job =>
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
                        return pipeline.QueueCanonicalReplayAsync(
                            batch,
                            async (frame, reply, ct) =>
                            {
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
                                            new ZLinkCanonicalApplicationPayload(
                                                frame.Header.Name,
                                                DurableActorReplyContentType,
                                                EncodeDurableActorReply(
                                                    request.ReplyRouteId,
                                                    replyFrame)));
                                }
                                if (completion is not null)
                                    await RelayStandaloneActorReplyAsync(
                                            identity,
                                            targetAuthority,
                                            replyRouteId,
                                            completion,
                                            replyFrame!,
                                            progress,
                                            targetOwner,
                                            ct)
                                        .ConfigureAwait(false);
                                actorState.Handoff.AcknowledgeCanonicalReplayThrough(
                                    job.AcceptedSequence);
                            },
                            CancellationToken.None);
                    }
                    catch
                    {
                        batch.Dispose();
                        throw;
                    }
                },
                queued,
                ShutdownToken).ConfigureAwait(false);

        var reservedThrough = queuedThrough;
        while (true)
        {
            var trailing = actorState.Handoff.SnapshotFinalReplay()
                .Where(frame => frame.ArrivalIndex > reservedThrough)
                .OrderBy(static frame =>
                    frame.RouteContext.MessageFollowHopCount == 0 ? 1 : 0)
                .ThenBy(static frame => frame.ArrivalIndex)
                .ToArray();
            if (trailing.Length != 0)
            {
                await QueueDurableBacklogWithSharedPermitsAsync(
                    GetOrStartState().ApplicationJobQueue,
                    trailing,
                    frame =>
                    {
                        var batch = ZLinkActorHandoffFrames.Restore(
                            actorRef,
                            [frame]);
                        try
                        {
                            return pipeline.DispatchReplayAsync(
                                    batch,
                                    arrivalIndex => actorState.Handoff
                                        .AcknowledgeReplayedFrame(arrivalIndex),
                                    CancellationToken.None)
                                .AsTask();
                        }
                        catch
                        {
                            batch.Dispose();
                            throw;
                        }
                    },
                    queued,
                    ShutdownToken).ConfigureAwait(false);
                reservedThrough = Math.Max(
                    reservedThrough,
                    trailing.Max(static frame => frame.ArrivalIndex));
                continue;
            }

            if (actorState.Handoff.TryOpenCanonicalMaintenanceAdmission(
                    handoffId,
                    reservedThrough))
                break;
        }

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

    internal static async ValueTask QueueDurableBacklogWithSharedPermitsAsync<T>(
        ZLinkApplicationJobQueue queue,
        IReadOnlyList<T> backlog,
        Func<T, Task> dispatch,
        ICollection<Task> completions,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(queue);
        ArgumentNullException.ThrowIfNull(backlog);
        ArgumentNullException.ThrowIfNull(dispatch);
        ArgumentNullException.ThrowIfNull(completions);

        foreach (var item in backlog)
        {
            var admission = await queue.AcquireAsync(cancellationToken)
                .ConfigureAwait(false);
            admission.MarkQueued();
            completions.Add(DispatchDurableTurnAsync(
                admission,
                item,
                dispatch));
        }
    }

    private static async Task DispatchDurableTurnAsync<T>(
        ZLinkApplicationJobQueueLease admission,
        T item,
        Func<T, Task> dispatch)
    {
        using (admission)
        using (ZLinkApplicationJobQueueInvocation.Enter(admission))
            await dispatch(item).ConfigureAwait(false);
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


    private async ValueTask RelayStandaloneActorReplyAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkActorAuthorityPayload targetAuthority,
        ulong replyRouteId,
        ZLinkCanonicalTerminalCompletion completion,
        byte[] replyFrame,
        ZLinkStandaloneActorRelocationProgress progress,
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

        if (acknowledgement is not (
                ZLinkRelocationReplyAckState.TerminalReceived
                or ZLinkRelocationReplyAckState.AlreadyTerminal))
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
        }
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
