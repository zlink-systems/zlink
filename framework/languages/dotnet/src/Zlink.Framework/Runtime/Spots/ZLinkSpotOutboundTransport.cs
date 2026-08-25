namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotOutboundTransport(
    IZLinkBackendSpot nativeSpot,
    TimeSpan? sendTimeout,
    CancellationToken stopToken) : IAsyncDisposable
{
    private readonly TimeSpan _sendTimeout = ValidateTimeout(sendTimeout);

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;

    internal void PublishCurrent(
        string channelName,
        string topic,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata = default)
    {
        nativeSpot.Publish(channelName, topic, parts, SendFlags.None, metadata);
    }

    /// <summary>Performs the non-blocking spot-send admission call.
    /// False leaves accepted async completion to the binding; routing failures
    /// surface as framework exceptions.</summary>
    public bool TrySendToSpotOnce(
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata = default)
    {
        ObserveSpotAuthority(
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        return ZLinkSubmitFailureMapper.AcceptOrThrow(
            nativeSpot.SendToSpot(
                targetNodeRid,
                targetSpotId,
                targetSpotGeneration,
                parts,
                SendFlags.DontWait,
                metadata),
            $"SPOT '{targetSpotId}' on node '{targetNodeRid}'");
    }

    public async ValueTask<ZLinkOneWaySubmitResult> SendToSpotAsync(
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
        ObserveSpotAuthority(
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        try
        {
            await nativeSpot.SendToSpotAsync(
                    targetNodeRid,
                    targetSpotId,
                    targetSpotGeneration,
                    parts,
                    SendFlags.None,
                    cancellationToken,
                    metadata)
                .ConfigureAwait(false);
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Submitted);
        }
        catch (ZlinkSubmitException failure)
        {
            return DirectSubmitFailure(failure);
        }
        catch (ObjectDisposedException)
        {
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Shutdown);
        }
    }

    internal async ValueTask<ZLinkOneWaySubmitResult> SendMessageFollowToSpotAsync(
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        MeshOperationId operationId,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte messageFollowHopCount,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        try
        {
            if (nativeSpot is not IZLinkBackendSpotMessageFollower relay)
                throw new InvalidOperationException(
                    "The Spot backend does not support Message Follow.");

            using var terminal = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                stopToken);
            terminal.CancelAfter(_sendTimeout);
            await relay.MessageFollowSendToSpotAsync(
                    targetNodeRid,
                    targetSpotId,
                    targetSpotGeneration,
                    operationId,
                    targetNodeGeneration,
                    authorityOwnerGeneration,
                    ownerLeaseGeneration,
                    messageFollowHopCount,
                    parts,
                    metadata,
                    terminal.Token)
                .ConfigureAwait(false);
            return new ZLinkOneWaySubmitResult(
                ZLinkOneWaySubmitStatus.Submitted);
        }
        catch (OperationCanceledException) when (stopToken.IsCancellationRequested)
        {
            return new ZLinkOneWaySubmitResult(
                ZLinkOneWaySubmitStatus.Shutdown);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return new ZLinkOneWaySubmitResult(
                ZLinkOneWaySubmitStatus.TimedOut);
        }
        catch (ZlinkSubmitException failure)
        {
            return DirectSubmitFailure(failure);
        }
        catch (ObjectDisposedException)
        {
            return new ZLinkOneWaySubmitResult(
                ZLinkOneWaySubmitStatus.Shutdown);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private void ObserveSpotAuthority(
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"spot_authority_observe target_node={targetNodeRid} "
            + $"spot={targetSpotId} object_gen={targetSpotGeneration} "
            + $"node_gen={targetNodeGeneration} "
            + $"authority_gen={authorityOwnerGeneration} "
            + $"lease_gen={ownerLeaseGeneration}");
        if (targetNodeRid == default
            // Node-control routes and Entry Spot routes do not carry a
            // user-Spot object generation. They are fenced by the node and
            // owner generations, while the backend observer requires a
            // positive object generation for a User Spot authority record.
            || targetSpotGeneration == 0
            || targetNodeGeneration == 0
            || authorityOwnerGeneration == 0
            || ownerLeaseGeneration == 0)
            return;
        if (nativeSpot is not IZLinkBackendAuthorityObserver observer)
            throw new InvalidOperationException(
                "The Spot backend does not support authority fencing.");
        observer.ObserveSpotAuthority(
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
    }

    public async ValueTask<ZLinkOneWaySubmitResult> SendToChannelAsync(
        string channelName,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        try
        {
            await nativeSpot.SendToChannelAsync(
                    channelName, parts, SendFlags.None, cancellationToken, metadata)
                .ConfigureAwait(false);
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Submitted);
        }
        catch (ZlinkSubmitException failure)
        {
            return DirectSubmitFailure(failure);
        }
        catch (ObjectDisposedException)
        {
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Shutdown);
        }
    }

    public async ValueTask<IReadOnlyList<Message>> RequestToChannelAsync(
        string channelName,
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        try
        {
            return await nativeSpot.RequestToChannelAsync(
                    channelName, parts, SendFlags.None, timeout, cancellationToken, metadata)
                .ConfigureAwait(false);
        }
        catch (ZLinkRequestTerminalException terminal)
        {
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                terminal.Result,
                terminal.FailureErrno,
                $"Channel request to '{channelName}' failed with result '{terminal.Result}'.");
        }
        catch (ZlinkRequestException failure)
        {
            throw ZLinkRequestFailureMapper.CreateChannelCompletionException(
                (RequestResult)(int)failure.Result,
                $"Channel request to '{channelName}' failed with result '{failure.Result}'.");
        }
        catch (ZlinkSubmitException failure)
        {
            throw ZLinkRequestFailureMapper.CreateSubmitException(
                failure, $"Channel request to '{channelName}'");
        }
    }

    public async ValueTask<IReadOnlyList<Message>> RequestToSpotAsync(
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
        ObserveSpotAuthority(
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
        try
        {
            return await nativeSpot.RequestToSpotAsync(
                    targetNodeRid,
                    targetSpotId,
                    targetSpotGeneration,
                    parts,
                    SendFlags.None,
                    timeout,
                    cancellationToken,
                    metadata)
                .ConfigureAwait(false);
        }
        catch (ZLinkRequestTerminalException terminal)
        {
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                terminal.Result,
                terminal.FailureErrno,
                $"SPOT request to '{targetSpotId}' on node '{targetNodeRid}' failed with result '{terminal.Result}'.");
        }
        catch (ZlinkRequestException failure)
        {
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                (RequestResult)(int)failure.Result,
                $"SPOT request to '{targetSpotId}' on node '{targetNodeRid}' failed with result '{failure.Result}'.");
        }
        catch (ZlinkSubmitException failure)
        {
            throw ZLinkRequestFailureMapper.CreateSubmitException(
                failure, $"SPOT request to '{targetSpotId}' on node '{targetNodeRid}'");
        }
    }

    private static ZLinkOneWaySubmitResult DirectSubmitFailure(
        ZlinkSubmitException failure) =>
        failure.Result switch
        {
            ZlinkSubmitException.ErrorCode.NotConnected =>
                new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.RouteNotConnected),
            ZlinkSubmitException.ErrorCode.NotFound =>
                new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.TargetNotFound),
            ZlinkSubmitException.ErrorCode.Terminated =>
                new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Shutdown),
            ZlinkSubmitException.ErrorCode.Backpressured =>
                new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Backpressured),
            _ => throw ZLinkRequestFailureMapper.CreateSubmitException(
                failure, "Direct Spot send")
        };

    private static TimeSpan ValidateTimeout(TimeSpan? timeout)
    {
        try
        {
            return ZLinkSocketConfig.NormalizeSendTimeout(timeout)
                   ?? TimeSpan.FromSeconds(1);
        }
        catch (ZLinkConfigurationException error)
        {
            throw new ArgumentOutOfRangeException(
                nameof(timeout), timeout, error.Message);
        }
    }

}
