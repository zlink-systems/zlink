namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotOutboundEndpoint(
    IZLinkCurrentSpotActivation activation,
    ZLinkSpotOutboundTransport outbound,
    ZLinkFrameworkRuntime runtime) : IZLinkSpotOutbound
{
    public IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message)
    {
        activation.EnsureOperationAllowed();
        return new ZLinkInstanceSpotSendCall<TMessage>(runtime, spotId, message);
    }

    public IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId, TRequest request)
    {
        activation.EnsureOperationAllowed();
        return new ZLinkInstanceSpotRequestCall<TRequest>(runtime, spotId, request);
    }

    public IZLinkPublishCall Publish<TEvent>(string channelName, string topic, TEvent message)
    {
        activation.EnsureOperationAllowed();
        return new ZLinkCurrentSpotPublishCall<TEvent>(activation, channelName, topic, message);
    }

    public IZLinkSendCall SendToChannel<TMessage>(string channelName, TMessage message)
    {
        activation.EnsureOperationAllowed();
        return new ZLinkCurrentSpotSendCall<TMessage>(activation, channelName, message);
    }

    public IZLinkRequestCall RequestToChannel<TRequest>(string channelName, TRequest request)
    {
        activation.EnsureOperationAllowed();
        return new ZLinkCurrentSpotRequestCall<TRequest>(activation, channelName, request);
    }

    public async ValueTask<IReadOnlyList<Message>> RequestToChannelAsync(
        string channelName,
        IReadOnlyList<Message> parts,
        TimeSpan? timeout,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        activation.EnsureOperationAllowed();
        using var operation = runtime.EnterOperation(countAsRequest: true);
        var requestTimeout = timeout ?? activation.DefaultRequestTimeout;
        var metric = ZLinkRuntimeMetrics.StartRequest(activation.ChannelName, "channel");
        var outcome = "completed";
        try
        {
            return runtime.Registration.Channels.TryGetValue(
                       channelName,
                       out var channel)
                   && channel.HasClientServerClient
                ? await runtime.RequestToChannelAsync(
                        channelName,
                        parts,
                        requestTimeout,
                        cancellationToken,
                        metadata)
                    .ConfigureAwait(false)
                : await outbound.RequestToChannelAsync(
                        channelName,
                        parts,
                        requestTimeout,
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

    /// <summary>Performs the first non-blocking ChannelName admission attempt
    /// on the current MeshNode. False lets the async submitter wait for
    /// send-ready.</summary>
    public bool TrySendToChannelOnce(
        string channelName,
        IReadOnlyList<Message> parts,
        ReadOnlyMemory<byte> metadata = default)
    {
        activation.EnsureOperationAllowed();
        using var operation = runtime.EnterOperation();
        return outbound.TrySendToChannelOnce(channelName, parts, metadata);
    }

    public async ValueTask<ZLinkOneWaySubmitResult> SendToChannelAsync(
        string channelName,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        activation.EnsureOperationAllowed();
        using var operation = runtime.EnterOperation();
        return runtime.Registration.Channels.TryGetValue(
                   channelName,
                   out var channel)
               && channel.HasClientServerClient
            ? await runtime.SendToChannelAsync(
                    channelName,
                    parts,
                    cancellationToken,
                    metadata)
                .ConfigureAwait(false)
            : await outbound.SendToChannelAsync(
                    channelName,
                    parts,
                    cancellationToken,
                    metadata)
                .ConfigureAwait(false);
    }

    public ValueTask<IReadOnlyList<Message>> RequestToSpotAsync(
        string routerChannelId,
        RoutingId targetNodeRid,
        string targetSpotId,
        ulong targetSpotGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        IReadOnlyList<Message> parts,
        TimeSpan? timeout,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata = default)
    {
        activation.EnsureOperationAllowed();
        return runtime.RequestToSpotViaRouterChannelAsync(
            routerChannelId,
            targetNodeRid,
            targetSpotId,
            targetSpotGeneration,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            parts,
            timeout ?? activation.DefaultRequestTimeout,
            cancellationToken,
            metadata);
    }

    public async ValueTask<SubmitResult> PublishCurrentAsync(
        string channelName,
        string topic,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata,
        Action release,
        IZLinkRuntimeFailureReporter errorSink)
    {
        activation.EnsureOperationAllowed();
        using var operation = runtime.EnterOperation();
        var backgroundOperation = runtime.RetainOperationForBackgroundWork();
        var released = 0;

        void ReleaseWorkerResources()
        {
            if (Interlocked.Exchange(ref released, 1) != 0) return;
            release();
            backgroundOperation.Dispose();
        }

        try
        {
            return await ZLinkLogicalMulticastSubmitter.SubmitAsync(
                    runtime.LogicalMulticastWorkerPool,
                    () => outbound.PublishCurrent(channelName, topic, parts, metadata),
                    cancellationToken,
                    runtime.ShutdownToken,
                    runtime.Registration.DefaultSocketSendTimeout,
                    ReleaseWorkerResources,
                    errorSink)
                .ConfigureAwait(false);
        }
        catch
        {
            ReleaseWorkerResources();
            throw;
        }
    }

    /// <summary>Performs the first non-blocking spot-send admission attempt.
    /// False lets the async submitter wait for send-ready.</summary>
    public bool TrySendToSpotOnce(
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
        activation.EnsureOperationAllowed();
        return runtime.TrySendToSpotViaRouterChannelOnce(
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

    public ValueTask<ZLinkOneWaySubmitResult> SendToSpotAsync(
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
        activation.EnsureOperationAllowed();
        return runtime.SendToSpotViaRouterChannelAsync(
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
    }

}
