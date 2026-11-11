using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkLocationRuntime
{
    internal Func<ZLinkStoreOutageEpisode, ValueTask>? CaptureRecoveryAuthoritiesAsync { get; set; }
    internal Func<
        object,
        ValueTask<ZLinkAuthoritySnapshot?>
    >? ReadPreservedAuthorityAsync { get; set; }
    internal Func<
        object,
        ZLinkAuthoritySnapshot,
        ValueTask
    >? ApplyRecoveredAuthorityAsync { get; set; }
    internal Func<
        CancellationToken,
        ValueTask<IReadOnlyDictionary<ZLinkMeshNodeDescriptorKey, Task<bool>>>
    >? SubmitRecoveryDescriptorsAsync { get; set; }

    private async ValueTask<bool> CanReclaimExpiredOwnerAsync(
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken
    )
    {
        if (GetHealthSnapshot().StoreOutage is not { } episode)
            return false;
        var deadline = Volatile.Read(ref _ownerAdmissionDeadline);
        // Admission excludes margin; reclaim requires lease lifetime plus margin.
        if (
            deadline is null
            || _time.GetElapsedTime(deadline.LeaseOperationStartedAt)
                < deadline.Lifetime
                    + _options.OwnerLeaseFencingMargin
                    + _options.OwnerLeaseFencingMargin
        )
            return false;
        var exact = await _store
            .ReadOwnerLeaseAsync(owner.OwnerId, cancellationToken)
            .ConfigureAwait(false);
        return exact is ZLinkOwnerLeaseReadResult.Missing
            || exact is ZLinkOwnerLeaseReadResult.Found found
                && found.Token == owner
                && found.LeaseExpiresAt <= found.StoreNow;
    }

    private ValueTask DiscardRecoveryAsync(ZLinkStoreOutageEpisode episode) =>
        _lane.RunAsync(() =>
        {
            if (
                GetHealthSnapshot().StoreOutage is not { Discarded: false } current
                || current.ObservedAt != episode.ObservedAt
            )
                return;
            _ownerAdmissionDeadline = null;
            UpdateHealth(health => health with { StoreOutage = current with { Discarded = true } });
            ScheduleOwnerDeadlinesOnLane();
            ExecutionsDiscarded?.Invoke(current.Executions);
        });

    private bool IsBeforeRecoveryDeadline(
        ZLinkStoreOutageEpisode episode,
        TimeSpan allowance = default
    ) =>
        GetHealthSnapshot().StoreOutage is { Discarded: false } current
        && current.ObservedAt == episode.ObservedAt
        && (
            allowance == TimeSpan.Zero
                ? _time.GetElapsedTime(episode.LeaseOperationStartedAt) < episode.Lifetime
                : _time.GetElapsedTime(episode.LeaseOperationStartedAt) + allowance
                    <= episode.Lifetime
        );

    private async ValueTask RecoverPreservedExecutionsAsync(
        ZLinkStoreOutageEpisode episode,
        ZLinkLocationOwnerToken? renewingOwner
    )
    {
        // A late completion of an operation started before the outage cannot confirm recovery.
        var owner = await _lane.RunAsync(RequireOwnerTokenOnLane).ConfigureAwait(false);
        if (renewingOwner is null && owner == episode.Owner)
            return;
        Volatile.Write(ref episode.Recovering, 1);
        await episode.CaptureCompleted.Task.ConfigureAwait(false);
        using var timeout = new CancellationTokenSource(_options.OwnerLeaseRenewTimeout, _time);
        IReadOnlyDictionary<ZLinkMeshNodeDescriptorKey, Task<bool>> publications =
            new Dictionary<ZLinkMeshNodeDescriptorKey, Task<bool>>();
        if (owner != episode.Owner && SubmitRecoveryDescriptorsAsync is { } submit)
            publications = await submit(timeout.Token).ConfigureAwait(false);
        var executions = await Task.WhenAll(
                episode.Executions.Select(async execution =>
                    (
                        Execution: execution,
                        Preserved: ReadPreservedAuthorityAsync is { } read
                            ? await read(execution).ConfigureAwait(false)
                            : null
                    )
                )
            )
            .ConfigureAwait(false);
        var descriptors =
            owner == episode.Owner
                ? executions
                    .Where(item => item.Preserved is not null)
                    .Select(item => item.Preserved!.Allocation.Descriptor.MeshName)
                    .Distinct(StringComparer.Ordinal)
                    .ToDictionary(
                        mesh => mesh,
                        mesh => _store.ListAllMeshNodesAsync(mesh, timeout.Token).AsTask(),
                        StringComparer.Ordinal
                    )
                : new Dictionary<string, Task<IReadOnlyList<ZLinkMeshNodeDescriptor>>>(
                    StringComparer.Ordinal
                );
        await Task.WhenAll(
                executions.Select(item => RecoverAndConfirmAsync(item.Execution, item.Preserved))
            )
            .ConfigureAwait(false);
        await _lane
            .RunAsync(() =>
            {
                if (!IsBeforeRecoveryDeadline(episode) || episode.Executions.Count != 0)
                    return;
                UpdateHealth(health => health with { StoreOutage = null });
                ScheduleOwnerDeadlinesOnLane();
            })
            .ConfigureAwait(false);

        async Task RecoverAndConfirmAsync(object execution, ZLinkAuthoritySnapshot? preserved)
        {
            var descriptorKey =
                preserved?.Allocation.Descriptor
                ?? (
                    execution is ZLinkSpotActivation spot
                        ? new ZLinkMeshNodeDescriptorKey(spot.MeshName, spot.NodeRid)
                        : (ZLinkMeshNodeDescriptorKey?)null
                );
            var descriptorConfirmation =
                owner == episode.Owner ? Task.FromResult(true)
                : descriptorKey is { } key && publications.TryGetValue(key, out var confirmation)
                    ? confirmation
                : Task.FromResult(false);
            bool recovered;
            try
            {
                recovered =
                    !(
                        descriptorConfirmation.IsCompletedSuccessfully
                        && !await descriptorConfirmation.ConfigureAwait(false)
                    ) && await RecoverExecutionAsync(execution, preserved).ConfigureAwait(false);
            }
            catch (Exception error)
                when (error
                        is TimeoutException
                            or OperationCanceledException
                            or ZLinkDescriptorPublicationUnconfirmedException
                    || error
                        is ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.Unavailable }
                )
            {
                return; // Keep the same snapshot for confirmation on the next heartbeat.
            }
            if (!recovered)
            {
                ExecutionsDiscarded?.Invoke(new HashSet<object> { execution });
                await _lane
                    .RunAsync(() => episode.CompleteExecution(execution))
                    .ConfigureAwait(false);
                return;
            }
            try
            {
                if (!await descriptorConfirmation.ConfigureAwait(false))
                {
                    ExecutionsDiscarded?.Invoke(new HashSet<object> { execution });
                    await _lane
                        .RunAsync(() => episode.CompleteExecution(execution))
                        .ConfigureAwait(false);
                    return;
                }
            }
            catch (Exception error)
                when (error
                        is TimeoutException
                            or OperationCanceledException
                            or ZLinkDescriptorPublicationUnconfirmedException
                    || error
                        is ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.Unavailable }
                )
            {
                return; // The next heartbeat uses the same publication and exact confirmation.
            }
            await _lane
                .RunAsync(() =>
                {
                    if (!IsBeforeRecoveryDeadline(episode) || IsDiscarded(execution))
                        return;
                    episode.CompleteExecution(execution);
                    ExecutionResumed?.Invoke(execution);
                })
                .ConfigureAwait(false);
        }

        async Task<bool> RecoverExecutionAsync(object execution, ZLinkAuthoritySnapshot? preserved)
        {
            if (!IsBeforeRecoveryDeadline(episode) || IsDiscarded(execution))
                return false;
            if (execution is ZLinkSpotActivation { SpotKind: ZLinkSpotKind.Entry })
                return true;
            if (preserved is null)
                return false;
            var key = execution is ZLinkActorRuntimeState actor
                ? ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId)
                : ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
                    ((ZLinkSpotActivation)execution).SpotId
                );
            var current = await _store
                .ReadAuthorityAsync(key, timeout.Token)
                .AsTask()
                .WaitAsync(timeout.Token)
                .ConfigureAwait(false);
            if (current is not ZLinkAuthorityReadResult.Found found)
                return false;
            var snapshot = found.Snapshot;
            if (
                snapshot.ObjectGeneration != preserved.ObjectGeneration
                || snapshot.AuthorityOwnerGeneration != preserved.AuthorityOwnerGeneration
                || snapshot.OwnerId != episode.Owner.OwnerId
                || snapshot.Allocation != preserved.Allocation
                || snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
            )
                return false;
            if (owner == episode.Owner)
            {
                var meshDescriptors = await descriptors[preserved.Allocation.Descriptor.MeshName]
                    .ConfigureAwait(false);
                return snapshot.StoreVersion == preserved.StoreVersion
                    && snapshot.OwnerLeaseGeneration == owner.LeaseGeneration
                    && meshDescriptors.Any(descriptor =>
                        ZLinkAuthorityMutation.MatchesPreservedDescriptor(preserved, descriptor)
                        && descriptor.LeaseGeneration == owner.LeaseGeneration
                    );
            }
            var payload = ReplacementPayload(execution, preserved, owner);
            if (payload.IsEmpty)
                return false;
            // §10: an applied write whose response was lost is confirmed by its exact intended value.
            if (
                snapshot.OwnerLeaseGeneration == owner.LeaseGeneration
                && snapshot.Payload.Span.SequenceEqual(payload.Span)
            )
            {
                if (ApplyRecoveredAuthorityAsync is { } confirm)
                    await confirm(execution, snapshot).ConfigureAwait(false);
                return IsBeforeRecoveryDeadline(episode) && !IsDiscarded(execution);
            }
            if (
                snapshot.StoreVersion != preserved.StoreVersion
                || snapshot.OwnerLeaseGeneration != episode.Owner.LeaseGeneration
                || IsDiscarded(execution)
                || !IsBeforeRecoveryDeadline(episode, _options.OwnerLeaseRenewTimeout)
            )
                return false;
            ZLinkAuthoritySnapshot confirmed;
            try
            {
                var exchanged = await _store
                    .CompareExchangeAuthorityAsync(
                        key,
                        preserved.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            payload,
                            ZLinkAuthorityGenerationTransition.Preserve,
                            owner,
                            null
                        ),
                        timeout.Token
                    )
                    .AsTask()
                    .WaitAsync(timeout.Token)
                    .ConfigureAwait(false);
                if (exchanged is not ZLinkAuthorityCompareExchangeResult.Stored stored)
                    return false;
                confirmed = stored.Snapshot;
            }
            catch (Exception error)
                when (ZLinkLocationStoreFailure.IsIndeterminate(error, timeout.Token))
            {
                var confirmation = await _store
                    .ReadAuthorityAsync(key, timeout.Token)
                    .ConfigureAwait(false);
                if (
                    confirmation is ZLinkAuthorityReadResult.Found unchanged
                    && unchanged.Snapshot.StoreVersion == preserved.StoreVersion
                    && unchanged.Snapshot.OwnerId == preserved.OwnerId
                    && unchanged.Snapshot.OwnerLeaseGeneration == preserved.OwnerLeaseGeneration
                )
                    throw;
                if (
                    confirmation is not ZLinkAuthorityReadResult.Found observed
                    || observed.Snapshot.ObjectGeneration != preserved.ObjectGeneration
                    || observed.Snapshot.AuthorityOwnerGeneration
                        != preserved.AuthorityOwnerGeneration
                    || observed.Snapshot.OwnerId != owner.OwnerId
                    || observed.Snapshot.OwnerLeaseGeneration != owner.LeaseGeneration
                    || observed.Snapshot.Allocation != preserved.Allocation
                    || !observed.Snapshot.Payload.Span.SequenceEqual(payload.Span)
                )
                    return false;
                confirmed = observed.Snapshot;
            }
            if (ApplyRecoveredAuthorityAsync is { } apply)
                await apply(execution, confirmed).ConfigureAwait(false);
            return IsBeforeRecoveryDeadline(episode) && !IsDiscarded(execution);
        }
    }

    private static bool IsDiscarded(object execution) =>
        execution switch
        {
            ZLinkActorRuntimeState actor => actor.ContextInvalidated,
            ZLinkSpotActivation spot => spot.IsDisposed || spot.HasCommittedClose,
            _ => true,
        };

    private static ReadOnlyMemory<byte> ReplacementPayload(
        object execution,
        ZLinkAuthoritySnapshot snapshot,
        ZLinkLocationOwnerToken owner
    )
    {
        if (
            execution is ZLinkActorRuntimeState
            && ZLinkActorAuthorityPayloadCodec.IsSteadyReady(snapshot.Payload.Span)
            && ZLinkActorAuthorityPayloadCodec.TryDecode(snapshot.Payload.Span, out var actor)
        )
            return ZLinkActorOwnershipCoordinator.EncodeAuthorityPayload(
                snapshot.Payload,
                actor with
                {
                    OwnerLeaseGeneration = checked((ulong)owner.LeaseGeneration),
                }
            );
        if (
            execution is ZLinkSpotActivation { SpotKind: ZLinkSpotKind.Instance }
            && ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var instance
            )
            && instance.State == ZLinkInstanceSpotAuthorityState.Ready
        )
            return ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                instance with
                {
                    OwnerLeaseGeneration = checked((ulong)owner.LeaseGeneration),
                }
            );
        if (
            execution is ZLinkSpotActivation
            && ZLinkUserSpotAuthorityPayloadCodec.TryDecode(snapshot.Payload.Span, out var spot)
            && spot.State == ZLinkUserSpotAuthorityState.Ready
        )
            return ZLinkUserSpotAuthorityPayloadCodec.Encode(
                spot with
                {
                    OwnerLeaseGeneration = checked((ulong)owner.LeaseGeneration),
                }
            );
        return default;
    }
}
