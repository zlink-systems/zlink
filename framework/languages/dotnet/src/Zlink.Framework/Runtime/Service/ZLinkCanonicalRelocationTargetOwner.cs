using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Service;

/// <summary>
/// Routes the exact relocation wire protocol into the target runtime that
/// owns the temporary queue, restore, target-only CAS, and dispatch opening.
/// </summary>
internal sealed class ZLinkCanonicalRelocationTargetOwner(
    RoutingId localNodeRid,
    ulong localNodeGeneration,
    TimeSpan prepareDeadline,
    ZLinkSpotRetireTargetRuntime? targetRuntime = null,
    ZLinkStandaloneActorRelocationRuntime? standaloneActorRuntime = null,
    Func<CancellationToken, ValueTask>? awaitTargetReady = null)
    : ICanonicalRelocationTarget
{
    private readonly Func<CancellationToken, ValueTask> _awaitTargetReady =
        awaitTargetReady ?? (static _ => ValueTask.CompletedTask);

    public async ValueTask<ZLinkServiceWireCodec.RelocationReadyRecord>
        PrepareAsync(
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            ZLinkRelocationEnvelope envelope,
            RoutingId authenticatedSourceNodeRid,
            ZLinkCanonicalRelocationPreparationLease lease,
            CancellationToken cancellationToken)
    {
        ValidateAttempt(prepare, authenticatedSourceNodeRid);
        await WaitForTargetReadinessAsync(cancellationToken)
            .ConfigureAwait(false);

        switch (ObjectKind(prepare.Object.Kind))
        {
            case ZLinkPlacementObjectKind.Actor:
                if (standaloneActorRuntime is null)
                    throw Unavailable(
                        "Standalone Actor target relocation is not configured.");
                await standaloneActorRuntime.StageTargetAsync(
                        prepare,
                        envelope,
                        authenticatedSourceNodeRid,
                        checked(prepare.Object
                            .ExpectedAuthorityOwnerGeneration + 1),
                        cancellationToken)
                    .ConfigureAwait(false);
                break;
            case ZLinkPlacementObjectKind.UserSpot:
            case ZLinkPlacementObjectKind.InstanceSpot:
                if (targetRuntime is null)
                    throw Unavailable(
                        "SPOT target relocation is not configured.");
                await targetRuntime.StageCanonicalInboundAsync(
                        prepare,
                        envelope,
                        authenticatedSourceNodeRid,
                        cancellationToken)
                    .ConfigureAwait(false);
                break;
            default:
                throw new InvalidDataException(
                    "Command 40 object kind is invalid.");
        }

        lease.MarkPrepared();

        return new ZLinkServiceWireCodec.RelocationReadyRecord(
            prepare.RelocationId,
            prepare.TargetAttemptGeneration,
            prepare.Coordinator,
            prepare.Target,
            prepare.Object,
            2);
    }

    public void ReadySubmitted(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid)
    {
        ValidateAttempt(prepare, authenticatedSourceNodeRid);
        switch (ObjectKind(prepare.Object.Kind))
        {
            case ZLinkPlacementObjectKind.Actor when standaloneActorRuntime is not null:
                if (standaloneActorRuntime.MarkTargetReadySubmitted(
                        prepare,
                        authenticatedSourceNodeRid))
                    standaloneActorRuntime.ScheduleTargetCutoverFallback(
                        prepare,
                        authenticatedSourceNodeRid);
                return;
            case ZLinkPlacementObjectKind.UserSpot or
                ZLinkPlacementObjectKind.InstanceSpot when targetRuntime is not null:
                targetRuntime.ScheduleCanonicalCutoverFallback(
                    prepare,
                    authenticatedSourceNodeRid);
                return;
            default:
                throw Unavailable(
                    "The relocation target is not configured.");
        }
    }

    public void ReadySubmissionFailed(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid)
    {
        ValidateAttempt(prepare, authenticatedSourceNodeRid);
        if (ObjectKind(prepare.Object.Kind) == ZLinkPlacementObjectKind.Actor
            && standaloneActorRuntime is not null)
            _ = standaloneActorRuntime.MarkTargetReadySubmissionFailed(
                prepare,
                authenticatedSourceNodeRid);
    }

    public ValueTask AbortPreparedAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid)
    {
        ValidateAttempt(prepare, authenticatedSourceNodeRid);
        return ObjectKind(prepare.Object.Kind) switch
        {
            ZLinkPlacementObjectKind.Actor when standaloneActorRuntime is not null =>
                standaloneActorRuntime.AbortTargetAsync(
                    prepare,
                    authenticatedSourceNodeRid),
            ZLinkPlacementObjectKind.UserSpot or
                ZLinkPlacementObjectKind.InstanceSpot when targetRuntime is not null =>
                targetRuntime.AbortCanonicalPreparedTargetAsync(
                    prepare,
                    authenticatedSourceNodeRid),
            _ => ValueTask.CompletedTask
        };
    }

    public ValueTask StageDataAsync(
        ZLinkServiceWireCodec.RelocationDataRecord data,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken)
    {
        ValidateAttempt(data, authenticatedSourceNodeRid);
        return ObjectKind(data.Object.Kind) switch
        {
            ZLinkPlacementObjectKind.Actor when standaloneActorRuntime is not null =>
                standaloneActorRuntime.AppendTargetDataAsync(
                    data,
                    authenticatedSourceNodeRid,
                    cancellationToken),
            ZLinkPlacementObjectKind.UserSpot or
                ZLinkPlacementObjectKind.InstanceSpot
                when targetRuntime is not null =>
                targetRuntime.AppendCanonicalInboundDataAsync(
                    data,
                    authenticatedSourceNodeRid,
                    cancellationToken),
            _ => throw Unavailable(
                "The relocation data target is not configured.")
        };
    }

    public ValueTask CutoverAsync(
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken)
    {
        ValidateAttempt(cutover, authenticatedSourceNodeRid);
        return ObjectKind(cutover.Object.Kind) switch
        {
            ZLinkPlacementObjectKind.Actor when standaloneActorRuntime is not null =>
                standaloneActorRuntime.CutoverTargetAsync(
                    cutover,
                    authenticatedSourceNodeRid,
                    cancellationToken),
            ZLinkPlacementObjectKind.UserSpot or
                ZLinkPlacementObjectKind.InstanceSpot
                when targetRuntime is not null =>
                targetRuntime.CutoverCanonicalInboundAsync(
                    cutover,
                    authenticatedSourceNodeRid,
                    cancellationToken),
            _ => throw Unavailable(
                "The relocation cutover target is not configured.")
        };
    }

    private async ValueTask WaitForTargetReadinessAsync(
        CancellationToken cancellationToken)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        deadline.CancelAfter(
            prepareDeadline > TimeSpan.Zero
                ? prepareDeadline
                : TimeSpan.FromSeconds(30));
        try
        {
            await _awaitTargetReady(deadline.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw Unavailable(
                "The relocation target RouteMesh is not ready.");
        }
    }

    private void ValidateAttempt(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid)
    {
        if (prepare.InitiatorRole != 1
            || prepare.SourceNodeRid != authenticatedSourceNodeRid
            || prepare.Coordinator.NodeRid != authenticatedSourceNodeRid
            || prepare.Coordinator.NodeGeneration
            != prepare.SourceNodeGeneration
            || prepare.Target.NodeRid != localNodeRid
            || prepare.Target.NodeGeneration != localNodeGeneration)
            throw new InvalidDataException(
                "Command 40 does not match its authenticated physical route.");
    }

    private static void ValidateAttempt(
        ZLinkServiceWireCodec.RelocationDataRecord data,
        RoutingId authenticatedSourceNodeRid)
    {
        if (data.SenderRole != 1
            || data.Coordinator.NodeRid != authenticatedSourceNodeRid)
            throw new InvalidDataException(
                "Command 31 does not match its authenticated source.");
    }

    private static void ValidateAttempt(
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
        RoutingId authenticatedSourceNodeRid)
    {
        if (cutover.SenderRole != 1
            || cutover.Coordinator.NodeRid != authenticatedSourceNodeRid)
            throw new InvalidDataException(
                "Command 34 does not match its authenticated source.");
    }

    private static ZLinkPlacementObjectKind ObjectKind(byte kind) => kind switch
    {
        1 => ZLinkPlacementObjectKind.Actor,
        2 => ZLinkPlacementObjectKind.UserSpot,
        3 => ZLinkPlacementObjectKind.InstanceSpot,
        _ => throw new InvalidDataException(
            "The relocation object kind is invalid.")
    };

    private static ZLinkFrameworkException Unavailable(string message) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            message,
            ZLinkRetryAdvice.RetryAfterBackoff);
}
