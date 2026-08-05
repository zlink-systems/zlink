namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal ZLinkChannelRuntimeBundle GetPublisherBundle(string channelName)
    {
        return _channels.GetPublisherBundle(GetOrStartState(), channelName);
    }

    internal ZLinkChannelRuntimeBundle GetClientServerClientBundle(string channelName)
    {
        return _channels.GetClientServerClientBundle(GetOrStartState(), channelName);
    }

    internal ZLinkClientServerClientRuntime GetClientServerClientRuntime(
        string channelName) =>
        _channels.GetClientServerClientRuntime(
            GetOrStartState(),
            channelName);

    internal async ValueTask<ZLinkOneWaySubmitResult> SendToChannelAsync(
        string channelName,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        if (Registration.Channels.TryGetValue(channelName, out var channel)
            && channel.HasClientServerClient)
        {
            if (!metadata.IsEmpty)
            {
                ZLinkMessageParts.DisposeAll(parts);
                throw ZLinkClassicCallSupport.MetadataNotSupported();
            }
            return await GetClientServerClientRuntime(channelName)
                .SendAsync(parts, cancellationToken)
                .ConfigureAwait(false);
        }

        var meshName = ResolveRouteMeshForChannel(channelName);
        return await GetMeshNodeRuntime(meshName).EntryOutbound
            .SendToChannelAsync(channelName, parts, cancellationToken, metadata)
            .ConfigureAwait(false);
    }

    internal async ValueTask<IReadOnlyList<Message>> RequestToChannelAsync(
        string channelName,
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        if (Registration.Channels.TryGetValue(channelName, out var channel)
            && channel.HasClientServerClient)
        {
            if (!metadata.IsEmpty)
            {
                ZLinkMessageParts.DisposeAll(parts);
                throw ZLinkClassicCallSupport.MetadataNotSupported();
            }
            return await GetClientServerClientRuntime(channelName)
                .RequestAsync(
                    parts,
                    timeout,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        var meshName = ResolveRouteMeshForChannel(channelName);
        return await GetMeshNodeRuntime(meshName).EntryOutbound
            .RequestToChannelAsync(channelName, parts, timeout, cancellationToken, metadata)
            .ConfigureAwait(false);
    }

    private string ResolveRouteMeshForChannel(string channelName)
    {
        var matches = Registration.SpotNodes.Values
            .Where(node => node.ChannelMemberships.Any(
                membership => StringComparer.Ordinal.Equals(
                    membership.ChannelName,
                    channelName)))
            .Select(node => node.SpotNodeName)
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        return matches.Length switch
        {
            1 => matches[0],
            //  Spec 08 §7: a ChannelName that is not registered in this
            //  process ends the call as NotFound and is not sent along any
            //  other path. Naming a channel this host does not serve is a
            //  routing outcome the caller can act on, not a startup defect.
            0 => throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"No process-local RouteMesh or ClientServer client is registered for ChannelName '{channelName}'."),
            _ => throw new ZLinkConfigurationException(
                $"ChannelName '{channelName}' resolves to more than one process-local RouteMesh.")
        };
    }

    private ZLinkRouteMeshTargetClassification ClassifyAutomaticRouteMeshTarget(
        ZLinkSpotNodeRuntime nodeRuntime,
        string meshName,
        RoutingId targetNodeRid)
    {
        var classification = _topologyQuery?.ClassifyRouteMeshTarget(
                meshName,
                targetNodeRid)
            ?? ZLinkRouteMeshTargetClassification.Unknown;

        // A location row can become visible before the local reconciler has
        // completed its first full descriptor snapshot. The target is then
        // known to the placement layer but not yet classifiable on this send
        // path. Keep that convergence window distinct from a RID that is
        // absent from a completed snapshot.
        if (classification == ZLinkRouteMeshTargetClassification.Unknown
            && _topologyQuery is not null
            && _topologyQuery.GetCompleteRouteMeshPeers(meshName) is null)
            classification = ZLinkRouteMeshTargetClassification.RequiredNotConnected;

        if (classification
            != ZLinkRouteMeshTargetClassification.RequiredNotConnected)
            return classification;

        return nodeRuntime.Node.MeshPeers().Any(peer =>
            peer.RoutingId == targetNodeRid
            && peer.State == MeshPeerState.Admitted)
                ? ZLinkRouteMeshTargetClassification.ReadyEligible
                : classification;
    }

    internal void EnsureKnownRouteMeshPeer(
        string routerChannelId,
        RoutingId targetNodeRid,
        string targetDescription)
    {
        var nodeRuntime = GetMeshNodeRuntime(routerChannelId);
        if (nodeRuntime.Node.RoutingId == targetNodeRid)
        {
            if (nodeRuntime.Registration.ObjectRole
                == ZLinkMeshNodeObjectRole.Client)
                throw CreateUnknownRouteTargetException(
                    routerChannelId,
                    targetNodeRid,
                    targetDescription);
            return;
        }

        if (nodeRuntime.UsesManualRouterAcquisition)
        {
            switch (nodeRuntime.ClassifyManualRouterTarget(targetNodeRid))
            {
                case ZLinkRouteMeshTargetClassification.ReadyEligible:
                    return;
                case ZLinkRouteMeshTargetClassification.RequiredNotConnected:
                    throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Route channel '{routerChannelId}' is not connected to node '{targetNodeRid}' for {targetDescription}.",
                    retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
                case ZLinkRouteMeshTargetClassification.ObjectClientTarget:
                case ZLinkRouteMeshTargetClassification.Unknown:
                    throw CreateUnknownRouteTargetException(
                        routerChannelId,
                        targetNodeRid,
                        targetDescription);
                default:
                    throw new InvalidOperationException(
                        "Unknown RouteMesh target classification.");
            }
        }

        switch (ClassifyAutomaticRouteMeshTarget(
                    nodeRuntime,
                    routerChannelId,
                    targetNodeRid))
        {
            case ZLinkRouteMeshTargetClassification.ReadyEligible:
                return;
            case ZLinkRouteMeshTargetClassification.RequiredNotConnected:
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Route channel '{routerChannelId}' is not connected to node '{targetNodeRid}' for {targetDescription}.",
                    retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
            case ZLinkRouteMeshTargetClassification.ObjectClientTarget:
            case ZLinkRouteMeshTargetClassification.Unknown:
                throw CreateUnknownRouteTargetException(
                    routerChannelId,
                    targetNodeRid,
                    targetDescription);
            default:
                throw new InvalidOperationException(
                    "Unknown RouteMesh target classification.");
        }
    }

    internal async ValueTask<ZLinkOneWaySubmitResult> SendToSpotViaRouterChannelAsync(
        string routerChannelId,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        using var operation = EnterOperation();
        var handedOff = false;
        try
        {
            EnsureKnownRouteMeshPeer(routerChannelId, targetNodeRid, $"SPOT '{targetSpotId}'");

            var accepted = _spotRouteRouter.SendAsync(
                routerChannelId,
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                parts,
                cancellationToken,
                metadata);
            handedOff = true;
            return await accepted.ConfigureAwait(false);
        }
        catch
        {
            if (!handedOff) ZLinkMessageParts.DisposeAll(parts);
            throw;
        }
    }

    internal RoutingId ResolveAcceptedSpotRouteNodeRid(string targetSpotNodeChannelName)
    {
        return _spotRouteRouter.ResolveAcceptedSpotRouteNodeRid(targetSpotNodeChannelName);
    }

    /// <summary>Performs the first non-blocking spot-send admission attempt
    /// after the route-mesh peer check.</summary>
    internal bool TrySendToSpotViaRouterChannelOnce(
        string routerChannelId,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata = default)
    {
        using var operation = EnterOperation();
        EnsureKnownRouteMeshPeer(routerChannelId, targetNodeRid, $"SPOT '{targetSpotId}'");
        return _spotRouteRouter.TrySendOnce(
            routerChannelId,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            parts,
            metadata);
    }

    internal async ValueTask<IReadOnlyList<Message>> RequestToSpotViaRouterChannelAsync(
        string routerChannelId,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        try
        {
            using var operation = EnterOperation(countAsRequest: true);
            var metric = ZLinkRuntimeMetrics.StartRequest(routerChannelId, "spot");
            var outcome = "completed";
            try
            {
                EnsureKnownRouteMeshPeer(routerChannelId, targetNodeRid, $"SPOT '{targetSpotId}'");

                return await _spotRouteRouter.RequestAsync(
                        routerChannelId,
                        targetNodeRid,
                        targetSpotId,
                        targetSpotGeneration,
                        targetNodeGeneration,
                        authorityOwnerGeneration,
                        ownerLeaseGeneration,
                        parts,
                        timeout,
                        cancellationToken,
                        metadata)
                    .ConfigureAwait(false);
            }
            catch (TimeoutException)
            {
                outcome = "timed_out";
                throw;
            }
            catch (OperationCanceledException)
            {
                outcome = "cancelled";
                throw;
            }
            catch
            {
                outcome = "failed";
                throw;
            }
            finally
            {
                metric.Complete(outcome);
            }
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static ZLinkFrameworkException CreateUnknownRouteTargetException(
        string routerChannelId,
        RoutingId targetNodeRid,
        string targetDescription,
        Exception? innerException = null)
    {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            $"Route channel '{routerChannelId}' does not know node '{targetNodeRid}' for {targetDescription}.",
            innerException: innerException);
    }

    internal IZLinkBackendSocket GetMonitoringSocket(string sourceName)
    {
        return _channels.GetMonitoringSocket(GetOrStartState(), sourceName);
    }
}
