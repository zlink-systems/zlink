using System.Diagnostics;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    /// <summary>Starts the runtime when needed and exposes the started
    /// state so the location auto-connect host can wire its per-mesh loops
    /// to the created channel and spot node runtimes.</summary>
    internal ValueTask<ZLinkFrameworkComponentState> EnsureStartedStateAsync(
        CancellationToken cancellationToken
    ) => GetStartedStateAsync(cancellationToken);

    internal async ValueTask<ZLinkResolvedSpotHandle?> ResolveSpotHandleAsync(
        string spotId,
        CancellationToken cancellationToken
    )
    {
        ZLinkSpotId.RequireCallerProvided(spotId, nameof(spotId));
        var resolver =
            Services.GetService(typeof(ZLinkLocationAddressResolvers))
            as ZLinkLocationAddressResolvers;
        return resolver is null
            ? null
            : await resolver
                .ResolveSpotHandleAsync(spotId, cancellationToken)
                .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkResolvedSpotHandle?> ResolveInstanceSpotHandleAsync(
        InstanceSpotIntentAddress address,
        CancellationToken cancellationToken,
        bool instanceIntent = false
    )
    {
        ZLinkSpotId.RequireCallerProvided(address.SpotId, nameof(address.SpotId));
        var rows =
            Services.GetService(typeof(ZLinkStoreLocationResolvers)) as ZLinkStoreLocationResolvers;
        if (rows is null)
            return null;
        var resolution = await rows.ResolveSpotRowWithStatusAsync(
                new ZLinkSpotLocationKey(address.SpotId),
                cancellationToken
            )
            .ConfigureAwait(false);
        if (resolution.Kind == ZLinkLocationResolutionKind.KnownUnavailable)
        {
            var store = Registration.Locations.ResolveStore()!;
            var authority = await store
                .ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(address.SpotId),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (
                instanceIntent
                && authority is ZLinkAuthorityReadResult.Found found
                && found.Snapshot.Allocation.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
                && ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    found.Snapshot.Payload.Span,
                    out var existing
                )
                && existing.State == ZLinkInstanceSpotAuthorityState.Ready
            )
            {
                var owner = await store
                    .ReadOwnerLeaseAsync(found.Snapshot.OwnerId, cancellationToken)
                    .ConfigureAwait(false);
                if (
                    owner is not ZLinkOwnerLeaseReadResult.Found lease
                    || lease.Token.LeaseGeneration != found.Snapshot.OwnerLeaseGeneration
                    || lease.LeaseExpiresAt <= lease.StoreNow
                )
                    return null;
            }
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Instance Spot '{address.SpotId}' is currently unavailable."
            );
        }
        var row = resolution.Row;
        if (
            row is null
            || row.SpotKind != ZLinkSpotKind.Instance
            || (
                !string.IsNullOrEmpty(address.InstanceSpotType)
                && !string.Equals(row.SpotType, address.InstanceSpotType, StringComparison.Ordinal)
            )
        )
            return null;
        return new ZLinkResolvedSpotHandle(
            new ZLinkSpotHandleSnapshot(
                row.MeshName,
                row.OwnerNodeRid,
                row.SpotId,
                row.SpotGeneration,
                row.SpotKind,
                row.AuthorityOwnerGeneration,
                row.OwnerNodeGeneration,
                checked((ulong)row.LeaseGeneration),
                row.OwnerId,
                row.StoreVersion
            ),
            row.AuthorityOwnerGeneration,
            _ => ValueTask.FromResult<(ZLinkSpotHandleSnapshot Snapshot, ulong Version)?>(null),
            () => rows.InvalidateSpotRoute(new ZLinkSpotLocationKey(address.SpotId))
        );
    }

    internal async ValueTask<IReadOnlyList<Message>> ActivateInstanceSpotAsync(
        InstanceSpotIntentAddress address,
        IReadOnlyList<Message> parts,
        bool request,
        TimeSpan timeout,
        ReadOnlyMemory<byte> metadata,
        CancellationToken cancellationToken,
        ulong? activationDeadlineUnixMs = null
    )
    {
        _ =
            Registration.Locations.ResolveStore()
            ?? throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Instance Spot activation requires a Location Store."
            );
        var source = ResolveActorCreationSource(address.MeshName);
        var resolver =
            Services.GetService(typeof(IZLinkMeshNodeLocationResolver))
                as IZLinkMeshNodeLocationResolver
            ?? Services.GetService(typeof(ZLinkStoreLocationResolvers))
                as ZLinkStoreLocationResolvers
            ?? throw new ZLinkConfigurationException(
                "Instance Spot activation requires the live MeshNode resolver."
            );
        var descriptors = await resolver
            .ListLiveMeshNodesAsync(address.MeshName, cancellationToken)
            .ConfigureAwait(false);
        var serving = descriptors.Where(static candidate =>
            candidate.State == ZLinkFrameworkRuntimeState.Serving
            && candidate.ObjectRole == ZLinkMeshNodeObjectRole.Server
        );
        var types = serving
            .SelectMany(static candidate => candidate.ObjectCapabilities)
            .Where(static capability =>
                capability.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
            )
            .Select(static capability => capability.StableType)
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        var stableType = address.InstanceSpotType;
        if (string.IsNullOrEmpty(stableType))
        {
            if (types.Length > 1)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    "InstanceSpot(instanceSpotType) is required when a Mesh serves multiple Instance Spot types."
                );
            stableType = types.SingleOrDefault() ?? string.Empty;
        }
        if (!types.Contains(stableType, StringComparer.Ordinal))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Mesh '{address.MeshName}' does not serve Instance Spot type '{stableType}'."
            );
        address = address with { InstanceSpotType = stableType };
        var eligible = serving
            .Where(candidate => IsEligibleInstanceCandidate(candidate, stableType))
            .OrderBy(static candidate => candidate.Rid, ZLinkRoutingIdOrder.Instance)
            .ToArray();
        var selected =
            ZLinkWeightedSelector.Select(
                eligible,
                static candidate => candidate.PlacementWeight,
                ref _nextInstanceActivationSelection
            )
            ?? throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"No Ready Instance Spot target is available for '{address.InstanceSpotType}'.",
                ZLinkRetryAdvice.RetryAfterBackoff
            );
        var deadlineUnixMs =
            activationDeadlineUnixMs
            ?? checked((ulong)DateTimeOffset.UtcNow.Add(timeout).ToUnixTimeMilliseconds());
        var target = new InstanceSpotActivationTarget(
            address.MeshName,
            selected.Rid,
            selected.LifecycleGeneration,
            address.SpotId,
            address.InstanceSpotType,
            selected.DescriptorRevision.ToString(System.Globalization.CultureInfo.InvariantCulture)
        );
        var sourceStatus = source.Node.MeshStatus();
        var sourceSpotId = ZLinkSpotAmbientContext.CurrentOrDefault?.SpotId ?? string.Empty;
        var state =
            _state
            ?? throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Framework runtime is not started."
            );
        if (state.FindSpotNodeByRoutingId(selected.Rid) is { } localTarget)
        {
            var operationId = source.Node.AllocateOperationId();
            var local = await localTarget
                .ActivateInstanceSpotLocalAsync(
                    target,
                    source.Node.RoutingId,
                    sourceStatus.LifecycleGeneration,
                    operationId,
                    sourceSpotId,
                    parts.Select(static part => (ReadOnlyMemory<byte>)part.ToArray()).ToArray(),
                    request,
                    deadlineUnixMs,
                    metadata.IsEmpty ? null : metadata,
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (local.Result != RequestResult.Ok)
                throw ZLinkRequestFailureMapper.CreateCompletionException(
                    local.Result,
                    (int)local.FailureCode,
                    "Local Instance Spot activation"
                );
            return local.ReplyParts.Select(Message.From).ToArray();
        }
        return await source
            .Node.ActivateInstanceSpotAsync(
                target,
                sourceSpotId,
                parts,
                request,
                deadlineUnixMs,
                timeout,
                metadata,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    private static bool IsEligibleInstanceCandidate(
        ZLinkMeshNodeDescriptor candidate,
        string stableType
    ) =>
        candidate.PlacementWeight > 0
        && (
            candidate.Capacity.Spots.Limit == 0
            || candidate.Capacity.Spots.Active + (long)candidate.Capacity.Spots.Reserved
                < candidate.Capacity.Spots.Limit
        )
        && candidate.ObjectCapabilities.Any(capability =>
            capability.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
            && string.Equals(capability.StableType, stableType, StringComparison.Ordinal)
        )
        && candidate.Capacity.SpotTypes.Any(capacity =>
            capacity.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
            && string.Equals(capacity.StableType, stableType, StringComparison.Ordinal)
            && (capacity.Limit == 0 || capacity.Active + (long)capacity.Reserved < capacity.Limit)
        );
}
