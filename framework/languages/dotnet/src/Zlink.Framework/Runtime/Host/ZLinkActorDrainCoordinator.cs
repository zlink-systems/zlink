using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Host;

/// <summary>
/// Coordinates actor target discovery and managed handoff during runtime drain.
/// </summary>
internal sealed class ZLinkActorDrainCoordinator(
    ZLinkStandaloneActorRelocationRuntime relocation,
    ZLinkActorSessionManager actorSessions,
    IServiceProvider services,
    ZLinkFrameworkRegistration registration)
{
    public async ValueTask<ZLinkFrameworkRelocationReason?> PreflightAsync(
        ZLinkRetirePreflightPlan plan,
        ZLinkRelocationTargetSelection selection,
        CancellationToken cancellationToken)
    {
        var states = StandaloneActors(actorSessions.SnapshotStates());
        if (states.Length == 0)
            return null;

        try
        {
            foreach (var actorType in states
                         .Select(static state => state.ActorType)
                         .Where(static actorType => !string.IsNullOrWhiteSpace(actorType))
                         .Distinct(StringComparer.Ordinal))
            {
                var sourceNode = registration.SpotNodes.Values.Single(node =>
                    node.ActorFactories.ContainsKey(actorType!));
                if (sourceNode.ActorRelocations[actorType!].PolicyKind == 0)
                    return ZLinkFrameworkRelocationReason.RelocationDisabled;
                var targets = await ResolveTargetCandidatesAsync(
                        actorType!,
                        selection,
                        cancellationToken)
                    .ConfigureAwait(false);
                foreach (var state in states.Where(state =>
                             string.Equals(state.ActorType, actorType, StringComparison.Ordinal)))
                {
                    if (state.Actor is null || state.NativeActorRef is not { } actorRef)
                        continue;
                    var capacity = new ZLinkCapacityVector(1, 0, null);
                    if (!targets.Any(target =>
                            target.Target.NodeRid != actorRef.NodeRid
                            && plan.TryReserve(target.Descriptor, capacity)))
                        return ZLinkFrameworkRelocationReason.TargetUnavailable;
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        return null;
    }

    public async ValueTask<ZLinkActorDrainResult> DrainAsync(
        ZLinkRelocationTargetSelection selection,
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken)
    {
        var states = ActorsForDrain(actorSessions.SnapshotStates());
        if (states.Length == 0)
            return new ZLinkActorDrainResult(
                true,
                null,
                0,
                ZLinkRelocationCommitKnowledge.NotCommitted,
                true);

        var targetsByActorType =
            new Dictionary<string, ZLinkActorDrainCandidate[]>(StringComparer.Ordinal);
        foreach (var actorType in states
                     .Select(static state => state.ActorType)
                     .Where(static actorType => !string.IsNullOrWhiteSpace(actorType))
                     .Distinct(StringComparer.Ordinal))
        {
            targetsByActorType[actorType!] = (await ResolveTargetCandidatesAsync(
                    actorType!,
                    selection,
                    cancellationToken)
                .ConfigureAwait(false));
        }

        var nextTarget = -1;
        var moves = states.Select(state => MoveActorAsync(state).AsTask()).ToArray();
        var results = await Task.WhenAll(moves).ConfigureAwait(false);
        var terminal = results.FirstOrDefault(
            static result => result.TerminalReason is not null);
        var committedUnitCount = checked((ulong)results.Sum(static result =>
            checked((long)result.CommittedUnitCount)));
        var commitKnowledge = CombineCommitKnowledge(results);
        var sourceTerminalized = results.All(
            static result => result.SourceTerminalized);
        return new ZLinkActorDrainResult(
            results.All(static result => result.Completed),
            terminal.TerminalReason,
            committedUnitCount,
            commitKnowledge,
            sourceTerminalized);

        async ValueTask<ZLinkActorDrainResult> MoveActorAsync(
            ZLinkActorRuntimeState actorState)
        {
            if (actorState.Handoff.IsSourceMigrationInProgress)
            {
                await actorState.Handoff.WaitForSourceCompletionAsync(cancellationToken)
                    .ConfigureAwait(false);
                if (actorState.Actor is null)
                {
                    return new ZLinkActorDrainResult(
                        true,
                        null,
                        1,
                        ZLinkRelocationCommitKnowledge.Committed,
                        true);
                }
            }

            var actor = actorState.Actor;
            var sourceNode = actorState.NativeActorRef?.NodeRid;
            var actorType = actorState.ActorType;
            if (actor is null || sourceNode is null || string.IsNullOrWhiteSpace(actorType))
                return new ZLinkActorDrainResult(
                    true,
                    null,
                    0,
                    ZLinkRelocationCommitKnowledge.NotCommitted,
                    true);
            //  이유 없이 `Completed=false`만 돌려주면 호출자의 재시도 loop가
            //  빠져나갈 조건이 없어 deadline을 소진하고 `DeadlineExceeded`로
            //  보고된다. 실제 이유는 "옮길 대상 node가 없다"이고 그 이름이
            //  이미 있으므로 그대로 싣는다. Target이 생기기를 기다리는 것은
            //  drain 이전 preflight의 몫이다(PreflightRetireAsync).
            if (!targetsByActorType.TryGetValue(actorType, out var targets))
                return new ZLinkActorDrainResult(
                    false,
                    ZLinkFrameworkRelocationReason.TargetUnavailable,
                    0,
                    ZLinkRelocationCommitKnowledge.NotCommitted,
                    true);
            var shellPlan = actorState.LiveActivation?
                .PerActorShellRelocationPlan;
            var eligible = shellPlan is null
                ? targets.Where(target =>
                    target.Target.NodeRid != sourceNode.Value).ToArray()
                : targets.Where(target =>
                        target.Descriptor.Rid == shellPlan.TargetNodeRid
                        && target.Descriptor.LifecycleGeneration
                        == shellPlan.TargetNodeLifecycleGeneration
                        && target.Descriptor.OwnerId
                        == shellPlan.TargetOwner.OwnerId
                        && target.Descriptor.LeaseGeneration
                        == checked((long)shellPlan.TargetOwner.LeaseGeneration))
                    .ToArray();
            if (eligible.Length == 0)
                return new ZLinkActorDrainResult(
                    false,
                    ZLinkFrameworkRelocationReason.TargetUnavailable,
                    0,
                    ZLinkRelocationCommitKnowledge.NotCommitted,
                    true);

            var start = shellPlan is null
                ? (Interlocked.Increment(ref nextTarget) & int.MaxValue)
                  % eligible.Length
                : 0;
            for (var attempt = 0; attempt < eligible.Length; attempt++)
            {
                var candidate = eligible[(start + attempt) % eligible.Length];
                var target = candidate.Target;
                try
                {
                    var result = await relocation.RelocateSourceAsync(
                            actorState,
                            candidate.Descriptor,
                            absoluteDeadline,
                            cancellationToken)
                        .ConfigureAwait(false);
                    //  Deferred는 "지금은 못 옮긴다"이므로 재시도가 의도된
                    //  경로다. 다만 이 분기만 표시가 없어, 영구 deferred일 때
                    //  호출자가 deadline을 소진하고 `DeadlineExceeded`로 보고할
                    //  뿐 왜 못 옮겼는지 알 수 없었다. TargetRejected 쪽과 같게
                    //  표시를 남긴다.
                    if (result == ZLinkStandaloneActorRelocationResult.Deferred)
                    {
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            "relocation_actor_deferred actor="
                            + actorState.ActorId
                            + " target="
                            + candidate.Descriptor.Rid);
                        return new ZLinkActorDrainResult(
                            false,
                            null,
                            0,
                            ZLinkRelocationCommitKnowledge.NotCommitted,
                            true);
                    }
                    if (result == ZLinkStandaloneActorRelocationResult.TargetRejected)
                    {
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"drain handoff rejected actor={actorState.ActorId} target={target.NodeRid} result=rejected");
                        continue;
                    }
                    return new ZLinkActorDrainResult(
                        true,
                        null,
                        1,
                        ZLinkRelocationCommitKnowledge.Committed,
                        true);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    throw;
                }
                catch (ZLinkActorRelocationFailureException error)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"drain handoff terminal actor={actorState.ActorId} target={target.NodeRid} reason={error.Reason} commit={error.CommitKnowledge} message={error.Message}");
                    return new ZLinkActorDrainResult(
                        false,
                        error.Reason,
                        error.CommitKnowledge
                            == ZLinkRelocationCommitKnowledge.Committed
                            ? 1UL
                            : 0UL,
                        error.CommitKnowledge,
                        error.SourceTerminalized);
                }
                catch (ZLinkFrameworkException error)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"drain handoff rejected actor={actorState.ActorId} target={target.NodeRid} kind={error.Kind} message={error.Message}");
                    if (!IsTargetLocalRetriable(error))
                        return new ZLinkActorDrainResult(
                            false,
                            ZLinkActorRelocationFailureException.MapReason(error),
                            0,
                            ZLinkActorRelocationFailureException.MapCommitKnowledge(error),
                            ZLinkActorRelocationFailureException.GetSourceTerminalized(error));
                }
                catch (ZlinkSubmitException error)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"drain handoff submit deferred actor={actorState.ActorId} target={target.NodeRid} message={error.Message}");
                    // A native route request can be temporarily busy. The
                    // next bounded drain pass retries with a refreshed view.
                }
                catch (TimeoutException error)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"drain handoff timed out actor={actorState.ActorId} target={target.NodeRid} message={error.Message}");
                    // Target availability can change during one request. The
                    // global drain deadline, not one request timeout, owns the
                    // terminal DeadlineExceeded decision.
                }
                catch (ZLinkActorHandoffRejectedException error)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"drain handoff rejected actor={actorState.ActorId} target={target.NodeRid} message={error.Message}");
                    // A completed rollback leaves the source actor eligible
                    // for the next bounded target refresh.
                }
                catch (Exception error)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"drain handoff terminal actor={actorState.ActorId} target={target.NodeRid} message={error.Message}");
                    return new ZLinkActorDrainResult(
                        false,
                        ZLinkActorRelocationFailureException.MapReason(error),
                        0,
                        ZLinkActorRelocationFailureException.MapCommitKnowledge(error),
                        ZLinkActorRelocationFailureException.GetSourceTerminalized(error));
                }
            }

            //  후보를 모두 시도했고 아무도 받지 않았다. 이유 없이 false를
            //  돌려주면 호출자의 재시도 loop가 빠져나갈 조건이 없어 deadline을
            //  소진하고 `DeadlineExceeded`로 보고된다. 실제로는 "쓸 수 있는
            //  target이 없다"이므로 그 이름을 싣는다. 다시 불러도 같은 후보를
            //  같은 이유로 거부하므로 재시도가 상태를 바꾸지 못한다.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                "relocation_actor_no_target_accepted actor=" + actorState.ActorId);
            return new ZLinkActorDrainResult(
                false,
                ZLinkFrameworkRelocationReason.TargetUnavailable,
                0,
                ZLinkRelocationCommitKnowledge.NotCommitted,
                true);
        }
    }

    private static ZLinkRelocationCommitKnowledge CombineCommitKnowledge(
        IReadOnlyList<ZLinkActorDrainResult> results)
    {
        if (results.Any(static result =>
                result.CommitKnowledge
                == ZLinkRelocationCommitKnowledge.Unknown))
            return ZLinkRelocationCommitKnowledge.Unknown;
        return results.Any(static result => result.CommittedUnitCount != 0)
            ? ZLinkRelocationCommitKnowledge.Committed
            : ZLinkRelocationCommitKnowledge.NotCommitted;
    }

    internal static bool IsTargetLocalRetriable(ZLinkFrameworkException error) =>
        error.RetryAdvice != ZLinkRetryAdvice.DoNotRetry
        && error.Kind is ZLinkFrameworkErrorKind.Unavailable
            or ZLinkFrameworkErrorKind.DeadlineExceeded
            or ZLinkFrameworkErrorKind.CapacityExceeded;

    internal static ZLinkActorRuntimeState[] StandaloneActors(
        IEnumerable<ZLinkActorRuntimeState> states) =>
        states.Where(static state => state.LiveActivation is null).ToArray();

    internal static ZLinkActorRuntimeState[] ActorsForDrain(
        IEnumerable<ZLinkActorRuntimeState> states) =>
        states.Where(static state =>
                state.LiveActivation is null
                || state.LiveActivation.ExecutionMode
                   == ZLinkUserSpotExecutionMode.PerActor
                && state.LiveActivation.PerActorShellRelocationPlan is not null)
            .ToArray();

    internal static string? ResolveMeshName(
        ZLinkFrameworkRegistration registration,
        string actorType)
    {
        var actorNode = registration.SpotNodes.Values.SingleOrDefault(
            node => node.ActorFactories.ContainsKey(actorType));
        return actorNode is null
            ? null
            : actorNode.SpotMeshChannelName ?? actorNode.SpotNodeName;
    }

    private async ValueTask<ZLinkActorDrainCandidate[]> ResolveTargetCandidatesAsync(
        string actorType,
        ZLinkRelocationTargetSelection selection,
        CancellationToken cancellationToken)
    {
        if (services.GetService<IZLinkMeshNodeLocationResolver>() is not { } peers)
            return [];

        var meshName = ResolveMeshName(registration, actorType);
        if (meshName is null) return [];
        var sourceNode = registration.SpotNodes.Values.Single(node =>
            node.ActorFactories.ContainsKey(actorType));
        var sourcePolicy = sourceNode.ActorRelocations[actorType];
        var requiredPolicy = sourcePolicy.PolicyKind switch
        {
            0 => ZLinkObjectMaintenancePolicyKind.Disabled,
            1 => ZLinkObjectMaintenancePolicyKind.Recreate,
            2 => ZLinkObjectMaintenancePolicyKind.Snapshot,
            _ => throw new ZLinkConfigurationException(
                $"Unknown relocation policy kind '{sourcePolicy.PolicyKind}'.")
        };
        if (requiredPolicy == ZLinkObjectMaintenancePolicyKind.Disabled)
            return [];
        var descriptors = await peers.ListLiveMeshNodesAsync(meshName, cancellationToken)
            .ConfigureAwait(false);
        var localNodeRids = registration.SpotNodes.Values
            .Select(static node => node.EffectiveRoutingId)
            .ToHashSet();
        var targets = new Dictionary<string, ZLinkActorDrainCandidate>(StringComparer.Ordinal);
        foreach (var descriptor in descriptors)
            if (!localNodeRids.Contains(descriptor.Rid)
                && descriptor.State == ZLinkFrameworkRuntimeState.Serving
                && descriptor.ObjectRole == ZLinkMeshNodeObjectRole.Server
                && descriptor.PlacementWeight > 0
                && descriptor.LeaseGeneration > 0
                && descriptor.Rid is { Size: > 0 }
                && !string.IsNullOrWhiteSpace(descriptor.EntrySpotId)
                && selection.Matches(descriptor)
                && (registration.MaintenanceWave is null
                    || !StringComparer.Ordinal.Equals(
                        registration.MaintenanceWave,
                        descriptor.MaintenanceWave))
                && ZLinkSpotRetireTargetRuntime.HasHeadroom(
                    descriptor.Capacity.Actors,
                    1)
                && descriptor.ActivationConcurrency.Limit
                   - descriptor.ActivationConcurrency.Active >= 1
                && descriptor.ObjectCapabilities.Any(capability =>
                    capability.ObjectKind == ZLinkPlacementObjectKind.Actor
                    && StringComparer.Ordinal.Equals(
                        capability.StableType,
                        actorType)
                    && capability.Policy == requiredPolicy
                    && (requiredPolicy
                        != ZLinkObjectMaintenancePolicyKind.Snapshot
                        || capability.HasSnapshotAdapter)))
                targets[descriptor.Rid.ToHex()] = new ZLinkActorDrainCandidate(
                    descriptor,
                    new ZLinkActorDrainTarget(
                        descriptor.Rid,
                        descriptor.EntrySpotId));
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"drain targets actorType={actorType} mesh={meshName} peers={descriptors.Count} accepting={targets.Count}");
        return targets.Values.ToArray();
    }
}

internal readonly record struct ZLinkActorDrainTarget(
    RoutingId NodeRid,
    string EntrySpotId);

internal readonly record struct ZLinkActorDrainCandidate(
    ZLinkMeshNodeDescriptor Descriptor,
    ZLinkActorDrainTarget Target);

internal readonly record struct ZLinkActorDrainResult(
    bool Completed,
    ZLinkFrameworkRelocationReason? TerminalReason,
    ulong CommittedUnitCount,
    ZLinkRelocationCommitKnowledge CommitKnowledge =
        ZLinkRelocationCommitKnowledge.NotCommitted,
    bool SourceTerminalized = false)
{
    internal bool HasCommitted => CommittedUnitCount != 0;

    internal bool HasUnknownCommit =>
        CommitKnowledge == ZLinkRelocationCommitKnowledge.Unknown;
}

internal sealed class ZLinkActorRelocationFailureException : Exception
{
    internal ZLinkActorRelocationFailureException(
        ZLinkFrameworkRelocationReason reason,
        ZLinkRelocationCommitKnowledge commitKnowledge,
        bool sourceTerminalized,
        Exception innerException)
        : base(
            $"Actor relocation failed. reason={reason} commit={commitKnowledge}.",
            innerException)
    {
        Reason = reason;
        CommitKnowledge = commitKnowledge;
        SourceTerminalized = sourceTerminalized;
    }

    internal ZLinkFrameworkRelocationReason Reason { get; }

    internal ZLinkRelocationCommitKnowledge CommitKnowledge { get; }

    internal bool SourceTerminalized { get; }

    internal static bool IsRetryableTargetFailure(Exception error) =>
        error switch
        {
            ZLinkFrameworkException framework =>
                framework.RetryAdvice != ZLinkRetryAdvice.DoNotRetry,
            ZlinkSubmitException => true,
            TimeoutException => true,
            ZLinkActorHandoffRejectedException => true,
            _ => false
        };

    internal static ZLinkFrameworkRelocationReason MapReason(Exception error) =>
        error switch
        {
            ZLinkActorRelocationFailureException failure => failure.Reason,
            OperationCanceledException => ZLinkFrameworkRelocationReason.DeadlineExceeded,
            ZLinkRelocationDataLostException => ZLinkFrameworkRelocationReason.StateIncompatible,
            ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.DeadlineExceeded } =>
                ZLinkFrameworkRelocationReason.DeadlineExceeded,
            ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.Unavailable
                or ZLinkFrameworkErrorKind.NotFound } =>
                ZLinkFrameworkRelocationReason.StoreUnavailable,
            ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.DataLost
                or ZLinkFrameworkErrorKind.ProtocolError
                or ZLinkFrameworkErrorKind.TypeMismatch
                or ZLinkFrameworkErrorKind.InvalidOperation
                or ZLinkFrameworkErrorKind.Rejected } =>
                ZLinkFrameworkRelocationReason.StateIncompatible,
            ZLinkConfigurationException => ZLinkFrameworkRelocationReason.StateIncompatible,
            _ => ZLinkFrameworkRelocationReason.RelocationFailed
        };

    internal static ZLinkRelocationCommitKnowledge MapCommitKnowledge(
        Exception error) =>
        error is ZLinkActorRelocationFailureException failure
            ? failure.CommitKnowledge
            : ZLinkRelocationCommitKnowledge.Unknown;

    internal static bool GetSourceTerminalized(Exception error) =>
        error is ZLinkActorRelocationFailureException failure
            ? failure.SourceTerminalized
            : false;
}

internal readonly record struct ZLinkRelocationWorkloadDrainResult(
    bool Completed,
    ZLinkFrameworkRelocationReason? TerminalReason,
    ulong CommittedUnitCount,
    bool SourceTerminalized = false,
    ZLinkRelocationCommitKnowledge CommitKnowledge =
        ZLinkRelocationCommitKnowledge.NotCommitted)
{
    internal bool HasCommitted => CommittedUnitCount != 0;

    internal bool HasUnknownCommit =>
        CommitKnowledge == ZLinkRelocationCommitKnowledge.Unknown;
}
