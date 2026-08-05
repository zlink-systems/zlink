namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    /// <summary>Starts the runtime when needed and exposes the started
    /// state so the location auto-connect host can wire its per-mesh loops
    /// to the created channel and spot node runtimes.</summary>
    internal ValueTask<ZLinkFrameworkComponentState> EnsureStartedStateAsync(
        CancellationToken cancellationToken) =>
        GetStartedStateAsync(cancellationToken);

    internal async ValueTask<ZLinkResolvedSpotHandle?> ResolveSpotHandleAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        ZLinkSpotId.RequireCallerProvided(spotId, nameof(spotId));
        var resolver = Services.GetService(typeof(ZLinkLocationAddressResolvers))
            as ZLinkLocationAddressResolvers;
        return resolver is null
            ? null
            : await resolver.ResolveSpotHandleAsync(
                    spotId,
                    cancellationToken)
                .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkResolvedSpotHandle?> ResolveInstanceSpotHandleAsync(
        InstanceSpotIntentAddress address,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(address);
        ZLinkSpotId.RequireCallerProvided(
            address.SpotId,
            nameof(address.SpotId));
        var rows = Services.GetService(typeof(ZLinkStoreLocationResolvers))
            as ZLinkStoreLocationResolvers;
        if (rows is null)
            return null;
        var resolution = await rows.ResolveSpotRowWithStatusAsync(
                new ZLinkSpotLocationKey(address.SpotId),
                cancellationToken)
            .ConfigureAwait(false);
        if (resolution.Kind == ZLinkLocationResolutionKind.KnownUnavailable)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Instance Spot '{address.SpotId}' is currently unavailable.");
        var row = resolution.Row;
        if (row is null
            || row.SpotKind != ZLinkSpotKind.Instance
            || (!string.IsNullOrEmpty(address.InstanceSpotType)
                && !string.Equals(
                    row.SpotType,
                    address.InstanceSpotType,
                    StringComparison.Ordinal)))
            return null;
        return new ZLinkResolvedSpotHandle(
            new ZLinkSpotHandleSnapshot(
                row.MeshName,
                row.OwnerNodeRid,
                row.SpotId,
                row.SpotGeneration,
                row.SpotKind,
                row.AuthorityOwnerGeneration,
                row.OwnerNodeGeneration),
            row.AuthorityOwnerGeneration,
            _ => ValueTask.FromResult<
                (ZLinkSpotHandleSnapshot Snapshot, ulong Version)?>(null),
            () => rows.InvalidateSpotRoute(
                new ZLinkSpotLocationKey(address.SpotId)));
    }

    internal async ValueTask<ZLinkResolvedSpotHandle?>
        WaitForInstanceSpotRouteOrMissingAsync(
            InstanceSpotIntentAddress address,
            DateTimeOffset deadline,
            CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(address);
        var rows = Services.GetService(typeof(ZLinkStoreLocationResolvers))
            as ZLinkStoreLocationResolvers;
        var store = Registration.Locations.ResolveStore()
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "Instance Spot routing requires a Location Store.");
        var key = new ZLinkSpotLocationKey(address.SpotId);
        rows?.InvalidateSpotRoute(key);

        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var current = await ResolveInstanceSpotHandleAsync(
                    address,
                    cancellationToken)
                .ConfigureAwait(false);
            if (current is not null) return current;

            var authority = await store.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
                        address.SpotId),
                    cancellationToken)
                .ConfigureAwait(false);
            if (authority is ZLinkAuthorityReadResult.Missing)
                return null;
            if (authority is ZLinkAuthorityReadResult.Found found)
            {
                if (ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                        found.Snapshot.Payload.Span,
                        out _))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.TypeMismatch,
                        $"Spot '{address.SpotId}' is not an Instance Spot.");
                if (ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                        found.Snapshot.Payload.Span,
                        out var instance)
                    && !string.IsNullOrEmpty(address.InstanceSpotType)
                    && !string.Equals(
                        address.InstanceSpotType,
                        instance.StableType,
                        StringComparison.Ordinal))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.TypeMismatch,
                        $"Instance Spot '{address.SpotId}' has another stable type.");
            }

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero)
                throw new TimeoutException(
                    $"Instance Spot '{address.SpotId}' did not finish its authority transition before the request deadline.");
            await Task.Delay(
                    remaining < TimeSpan.FromMilliseconds(10)
                        ? remaining
                        : TimeSpan.FromMilliseconds(10),
                    cancellationToken)
                .ConfigureAwait(false);
            rows?.InvalidateSpotRoute(key);
        }
    }

    internal async ValueTask<IReadOnlyList<Message>> ActivateInstanceSpotAsync(
        InstanceSpotIntentAddress address,
        IReadOnlyList<Message> parts,
        bool request,
        TimeSpan timeout,
        ReadOnlyMemory<byte> metadata,
        CancellationToken cancellationToken)
    {
        var source = ResolveActorCreationSource(address.MeshName);
        var store = Registration.Locations.ResolveStore()
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "Instance Spot activation requires a Location Store.");
        var descriptors = await store.ListAllMeshNodesAsync(
                address.MeshName,
                cancellationToken)
            .ConfigureAwait(false);
        var eligible = descriptors
            .Where(candidate => IsEligibleInstanceCandidate(
                candidate,
                address.InstanceSpotType))
            .OrderBy(static candidate => candidate.Rid.ToHex(), StringComparer.Ordinal)
            .ToArray();
        var selected = ZLinkWeightedSelector.Select(
            eligible,
            static candidate => candidate.PlacementWeight,
            ref _nextInstanceActivationSelection)
            ?? throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.CapacityExceeded,
                $"No Ready Instance Spot target is available for '{address.InstanceSpotType}'.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        var deadlineAt = DateTimeOffset.UtcNow.Add(timeout);
        var deadlineUnixMs = checked((ulong)deadlineAt.ToUnixTimeMilliseconds());
        var target = new InstanceSpotActivationTarget(
            address.MeshName,
            selected.Rid,
            selected.LifecycleGeneration,
            address.SpotId,
            address.InstanceSpotType,
            selected.DescriptorRevision.ToString(
                System.Globalization.CultureInfo.InvariantCulture));
        var sourceStatus = source.Node.MeshStatus();
        var sourceSpotId =
            ZLinkSpotAmbientContext.CurrentOrDefault?.SpotId ?? string.Empty;
        var state = _state
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "Framework runtime is not started.");
        if (state.TryGetSpotNodeByRoutingId(selected.Rid, out var localTarget))
        {
            var operationId = source.Node.AllocateOperationId();
            var local = await localTarget.ActivateInstanceSpotLocalAsync(
                    target,
                    source.Node.RoutingId,
                    sourceStatus.LifecycleGeneration,
                    operationId,
                    sourceSpotId,
                    parts.Select(static part =>
                            (ReadOnlyMemory<byte>)part.ToArray())
                        .ToArray(),
                    request,
                    deadlineUnixMs,
                    metadata.IsEmpty ? null : metadata,
                    cancellationToken)
                .ConfigureAwait(false);
            if (local.Result != RequestResult.Ok)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InternalFailure,
                    "Local Instance Spot activation failed.");
            return local.ReplyParts.Select(Message.From).ToArray();
        }
        return await source.Node.ActivateInstanceSpotAsync(
                target,
                sourceSpotId,
                parts,
                request,
                deadlineUnixMs,
                timeout,
                metadata,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static bool IsEligibleInstanceCandidate(
        ZLinkMeshNodeDescriptor candidate,
        string stableType) =>
        candidate.State == ZLinkFrameworkRuntimeState.Serving
        && candidate.ObjectRole == ZLinkMeshNodeObjectRole.Server
        && candidate.PlacementWeight > 0
        && (candidate.Capacity.Spots.Limit == 0
            || candidate.Capacity.Spots.Active
            + (long)candidate.Capacity.Spots.Reserved
            < candidate.Capacity.Spots.Limit)
        && candidate.ObjectCapabilities.Any(capability =>
            capability.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
            && string.Equals(
                capability.StableType,
                stableType,
                StringComparison.Ordinal))
        && candidate.Capacity.SpotTypes.Any(capacity =>
            capacity.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
            && string.Equals(
                capacity.StableType,
                stableType,
                StringComparison.Ordinal)
            && (capacity.Limit == 0
                || capacity.Active + (long)capacity.Reserved
                < capacity.Limit));
}
