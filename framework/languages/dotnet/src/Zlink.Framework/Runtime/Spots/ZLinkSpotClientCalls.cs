using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotOutboundService : IZLinkSpotOutbound
{
    public IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message) =>
        ZLinkSpotAmbientContext.RequireCurrent().Outbound.SendToSpot(spotId, message);

    public IZLinkSpotRequestCall RequestToSpot<TMessage>(string spotId, TMessage request) =>
        ZLinkSpotAmbientContext.RequireCurrent().Outbound.RequestToSpot(spotId, request);

    public IZLinkPublishCall Publish<TEvent>(string channelName, string topic, TEvent message)
    {
        return ZLinkSpotAmbientContext.RequireCurrent().Outbound.Publish(channelName, topic, message);
    }

    public IZLinkSendCall SendToChannel<TMessage>(string channelName, TMessage message)
    {
        return ZLinkSpotAmbientContext.RequireCurrent().Outbound.SendToChannel(channelName, message);
    }

    public IZLinkRequestCall RequestToChannel<TMessage>(string channelName, TMessage request)
    {
        return ZLinkSpotAmbientContext.RequireCurrent().Outbound.RequestToChannel(channelName, request);
    }

}

internal sealed class ZLinkInstanceSpotSendCall<TMessage>(
    ZLinkFrameworkRuntime runtime,
    InstanceSpotIntentAddress target,
    TMessage message) : IZLinkSendCall, IZLinkSpotSendCall
{
    private readonly ZLinkCallMetadata _metadata = new();
    private readonly ZLinkOneWayCallGate _submission = new("Instance Spot send");
    private bool _instanceIntent = true;
    private bool _exactSpotIdCall;
    private bool _meshSelected;

    internal ZLinkInstanceSpotSendCall(
        ZLinkFrameworkRuntime runtime,
        string spotId,
        TMessage message)
        : this(runtime, new InstanceSpotIntentAddress(string.Empty, string.Empty, spotId), message)
    {
        _instanceIntent = false;
        _exactSpotIdCall = true;
    }

    public IZLinkSpotSendCall InstanceSpot()
    {
        _instanceIntent = true;
        return this;
    }

    public IZLinkSpotSendCall InstanceSpot(string instanceSpotType)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(instanceSpotType);
        target = target with { InstanceSpotType = instanceSpotType };
        _instanceIntent = true;
        return this;
    }

    public IZLinkSpotSendCall InMesh(string meshName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        target = target with { MeshName = meshName };
        _meshSelected = true;
        return this;
    }

    IZLinkSpotSendCall IZLinkMetadataCall<IZLinkSpotSendCall>.Metadata(
        string key,
        string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    IZLinkSpotSendCall IZLinkMetadataCall<IZLinkSpotSendCall>.Metadata(
        ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    public IZLinkSendCall Metadata(string key, string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    public IZLinkSendCall Metadata(ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    public async ValueTask Async(
        CancellationToken cancellationToken = default)
    {
        _submission.Claim();
        if (_meshSelected && !_instanceIntent)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "InMesh is valid only for an Instance Spot intent.");
        var handle = _exactSpotIdCall
            ? await runtime.ResolveSpotHandleAsync(target.SpotId, cancellationToken)
                .ConfigureAwait(false)
            : await runtime.ResolveInstanceSpotHandleAsync(target, cancellationToken)
            .ConfigureAwait(false);
        if (handle is null)
        {
            if (!_instanceIntent)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    $"Spot '{target.SpotId}' was not found.");
            target = runtime.ResolveInstanceSpotIntent(target);
            var header = ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Command,
                target.MeshName,
                ZLinkMessageNameResolver.ResolveFromMessage(message));
            var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
                header,
                message,
                runtime.Registration.Codecs);
            _ = await runtime.ActivateInstanceSpotAsync(
                    target,
                    parts,
                    request: false,
                    runtime.Registration.DefaultRequestTimeout,
                    _metadata.Encode(),
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        var call = new ZLinkRouteSpotSendCall<TMessage>(runtime, handle, message);
        call.Metadata(new ZLinkMessageMetadata(_metadata.Snapshot()));
        await call.Async(cancellationToken).ConfigureAwait(false);
    }
}

internal sealed class ZLinkInstanceSpotRequestCall<TRequest>(
    ZLinkFrameworkRuntime runtime,
    InstanceSpotIntentAddress target,
    TRequest request) : IZLinkRequestCall, IZLinkSpotRequestCall
{
    private readonly ZLinkCallMetadata _metadata = new();
    private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;
    private TimeSpan? _timeout;
    private bool _instanceIntent = true;
    private bool _exactSpotIdCall;
    private bool _meshSelected;

    internal ZLinkInstanceSpotRequestCall(
        ZLinkFrameworkRuntime runtime,
        string spotId,
        TRequest request)
        : this(runtime, new InstanceSpotIntentAddress(string.Empty, string.Empty, spotId), request)
    {
        _instanceIntent = false;
        _exactSpotIdCall = true;
    }

    public IZLinkSpotRequestCall InstanceSpot()
    {
        _instanceIntent = true;
        return this;
    }

    public IZLinkSpotRequestCall InstanceSpot(string instanceSpotType)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(instanceSpotType);
        target = target with { InstanceSpotType = instanceSpotType };
        _instanceIntent = true;
        return this;
    }

    public IZLinkSpotRequestCall InMesh(string meshName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        target = target with { MeshName = meshName };
        _meshSelected = true;
        return this;
    }

    IZLinkSpotRequestCall IZLinkMetadataCall<IZLinkSpotRequestCall>.Metadata(
        string key,
        string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    IZLinkSpotRequestCall IZLinkMetadataCall<IZLinkSpotRequestCall>.Metadata(
        ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    IZLinkSpotRequestCall IZLinkSpotRequestCall.Timeout(TimeSpan timeout)
    {
        Timeout(timeout);
        return this;
    }

    public IZLinkRequestCall Timeout(TimeSpan timeout)
    {
        ZLinkRequestTimeoutValidation.Validate(timeout, nameof(timeout));
        _timeout = timeout;
        return this;
    }

    public IZLinkRequestCall Metadata(string key, string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    public IZLinkRequestCall Metadata(ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default) =>
        ExecuteAsync<TReply>(cancellationToken);

    public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default) =>
        ZLinkApplicationExecutionContext
            .RequireYieldTurn(_turn, "Instance Spot request")
            .YieldFrameworkCallAsync(ExecuteAsync<TReply>, cancellationToken);

    private async ValueTask<TReply> ExecuteAsync<TReply>(CancellationToken cancellationToken)
    {
        if (_meshSelected && !_instanceIntent)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "InMesh is valid only for an Instance Spot intent.");
        var operationTimeout = _timeout ?? runtime.Registration.DefaultRequestTimeout;
        var deadline = DateTimeOffset.UtcNow + operationTimeout;
        var handle = _exactSpotIdCall
            ? await runtime.ResolveSpotHandleAsync(target.SpotId, cancellationToken)
                .ConfigureAwait(false)
            : await runtime.ResolveInstanceSpotHandleAsync(target, cancellationToken)
            .ConfigureAwait(false);
        while (handle is not null)
        {
            try
            {
                return await RequestExistingAsync<TReply>(
                        handle,
                        Remaining(deadline),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (ZLinkFrameworkException error)
                when (_instanceIntent && IsAuthorityTransitionConflict(error))
            {
                handle.InvalidateRoute();
                handle = await runtime.WaitForInstanceSpotRouteOrMissingAsync(
                    target,
                    deadline,
                    cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (ZLinkFrameworkException error)
                when (_instanceIntent
                      && ZLinkSpotHandleRequestExecution.IsStaleRoute(error))
            {
                // Idle eviction can remove the native activation while its
                // location row is still being released. The durable Instance
                // Spot operation must refresh the route before deciding
                // whether to cold-activate a replacement.
                handle.InvalidateRoute();
                handle = await runtime.WaitForInstanceSpotRouteOrMissingAsync(
                        target,
                        deadline,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (ZLinkFrameworkException error)
                when (!_instanceIntent
                      && ZLinkSpotHandleRequestExecution.IsStaleRoute(error))
            {
                // A global Spot ID can retain a cached route after its owner
                // has stopped. Refresh only the location row to distinguish a
                // removed Spot (NotFound) from a still-existing Spot whose
                // target route is temporarily unavailable. Do not resubmit
                // the application request after a stale-route response: the
                // target may have accepted it before the response was lost.
                handle.InvalidateRoute();
                handle = await runtime.ResolveSpotHandleAsync(
                        target.SpotId,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (handle is not null)
                    throw;
            }
        }

        if (!_instanceIntent)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Spot '{target.SpotId}' was not found.");
        target = runtime.ResolveInstanceSpotIntent(target);
        var activationTimeout = Remaining(deadline);
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            target.MeshName,
            ZLinkMessageNameResolver.ResolveFromMessage(request),
            activationTimeout);
        var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
            header,
            request,
            runtime.Registration.Codecs);
        var reply = await runtime.ActivateInstanceSpotAsync(
                target,
                parts,
                request: true,
                activationTimeout,
                _metadata.Encode(),
                cancellationToken)
            .ConfigureAwait(false);
        return ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<TReply>(
            reply,
            "Instance Spot request reply is empty.",
            "Instance Spot request failed.",
            runtime.Registration.Codecs);
    }

    private async ValueTask<TReply> RequestExistingAsync<TReply>(
        ZLinkResolvedSpotHandle handle,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var call = new ZLinkRouteSpotRequestCall<TRequest>(runtime, handle, request);
        call.Timeout(timeout);
        call.Metadata(new ZLinkMessageMetadata(_metadata.Snapshot()));
        return await call.Async<TReply>(cancellationToken).ConfigureAwait(false);
    }

    private static bool IsAuthorityTransitionConflict(
        ZLinkFrameworkException error) =>
        error.InnerException is ZlinkRequestException
        {
            Result: ZlinkRequestException.ErrorCode.Conflict
        };

    private static TimeSpan Remaining(DateTimeOffset deadline)
    {
        var remaining = deadline - DateTimeOffset.UtcNow;
        if (remaining <= TimeSpan.Zero)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                "Instance Spot request deadline elapsed.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        return remaining;
    }
}

internal sealed class ZLinkRoutedSpotSendCall<TMessage>(
    IZLinkCurrentSpotActivation activation,
    ZLinkResolvedSpotHandle target,
    TMessage message) : IZLinkSendCall
{
    private readonly ZLinkCallMetadata _metadata = new();
    private readonly ZLinkOneWayCallGate _submission = new("Spot send");

    public IZLinkSendCall Metadata(string key, string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    public IZLinkSendCall Metadata(ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    public async ValueTask Async(
        CancellationToken cancellationToken = default)
    {
        _submission.Claim();
        cancellationToken.ThrowIfCancellationRequested();
        // One-way sends use the current snapshot once and never retry; a
        // retry could duplicate a packet that was already delivered.
        var snapshot = target.Snapshot;
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Command,
            activation.ChannelName,
            ZLinkMessageNameResolver.ResolveFromMessage(message));
        var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(header, message, activation.Codecs);
        try
        {
            var result = await activation.OutboundEndpoint.SendToSpotAsync(
                    snapshot.RouterChannelId,
                    snapshot.NodeRid,
                    snapshot.SpotId,
                    (ulong)snapshot.Generation,
                    snapshot.NodeGeneration,
                    snapshot.AuthorityOwnerGeneration,
                    snapshot.OwnerLeaseGeneration,
                    parts,
                    cancellationToken,
                    _metadata.Encode())
                .ConfigureAwait(false);
            ZLinkOneWaySubmitOutcome.EnsureAccepted(
                result,
                "Spot send",
                ZLinkFrameworkErrorKind.NotFound);
        }
        catch (ZLinkFrameworkException error)
            when (ZLinkSpotHandleRequestExecution.IsStaleRoute(error))
        {
            target.InvalidateRoute();
            throw;
        }
    }
}

internal sealed class ZLinkRoutedSpotRequestCall<TRequest>(
    IZLinkCurrentSpotActivation activation,
    ZLinkResolvedSpotHandle target,
    TRequest request) : IZLinkRequestCall
{
    private readonly ZLinkCallMetadata _metadata = new();
    private readonly ZLinkApplicationExecutionScope? _executionScope =
        ZLinkApplicationExecutionContext.Current;
    private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;
    private TimeSpan? _timeout;

    public IZLinkRequestCall Timeout(TimeSpan timeout)
    {
        ZLinkRequestTimeoutValidation.Validate(timeout, nameof(timeout));
        _timeout = timeout;
        return this;
    }

    public IZLinkRequestCall Metadata(string key, string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    public IZLinkRequestCall Metadata(ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default)
    {
        ZLinkApplicationExecutionContext.RejectSpotRequestWhenSameGate(
            target.Snapshot.SpotId,
            _executionScope);
        return ExecuteAsync<TReply>(cancellationToken);
    }

    public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default)
    {
        ZLinkApplicationExecutionContext.RejectSpotRequestWhenSameGate(
            target.Snapshot.SpotId,
            _executionScope);
        return ZLinkApplicationExecutionContext
            .RequireYieldTurn(_turn, "Spot request")
            .YieldFrameworkCallAsync(ExecuteAsync<TReply>, cancellationToken);
    }

    private async ValueTask<TReply> ExecuteAsync<TReply>(CancellationToken cancellationToken)
    {
        var timeout = _timeout ?? activation.DefaultRequestTimeout;
        var reply = await ZLinkSpotHandleRequestExecution.ExecuteAsync(
                target,
                snapshot =>
                {
                    var header = ZLinkClientCallCodec.CreateEnvelope(
                        ZLinkMessageKind.Request,
                        activation.ChannelName,
                        ZLinkMessageNameResolver.ResolveFromMessage(request),
                        timeout);
                    var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
                        header,
                        request,
                        activation.Codecs);
                    return activation.OutboundEndpoint.RequestToSpotAsync(
                        snapshot.RouterChannelId,
                        snapshot.NodeRid,
                        snapshot.SpotId,
                        (ulong)snapshot.Generation,
                        snapshot.NodeGeneration,
                        snapshot.AuthorityOwnerGeneration,
                        snapshot.OwnerLeaseGeneration,
                        parts,
                        timeout,
                        cancellationToken,
                        _metadata.Encode());
                },
                cancellationToken)
            .ConfigureAwait(false);
        return ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<TReply>(
            reply,
            "SPOT request reply is empty.",
            "SPOT request failed.",
            activation.Codecs);
    }


}

internal sealed class ZLinkCurrentSpotSendCall<TMessage>(
    IZLinkCurrentSpotActivation activation,
    string channelName,
    TMessage message) : IZLinkSendCall
{
    private readonly ZLinkCallMetadata _metadata = new();
    private readonly ZLinkOneWayCallGate _submission = new("Channel send");

    public IZLinkSendCall Metadata(string key, string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    public IZLinkSendCall Metadata(ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    public async ValueTask Async(
        CancellationToken cancellationToken = default)
    {
        _submission.Claim();
        cancellationToken.ThrowIfCancellationRequested();
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Command,
            channelName,
            ZLinkMessageNameResolver.ResolveFromMessage(message));
        var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(header, message, activation.Codecs);
        var result = await activation.OutboundEndpoint
            .SendToChannelAsync(channelName, parts, cancellationToken, _metadata.Encode())
            .ConfigureAwait(false);
        ZLinkOneWaySubmitOutcome.EnsureAccepted(result, "Channel send");
    }
}

internal sealed class ZLinkCurrentSpotRequestCall<TMessage>(
    IZLinkCurrentSpotActivation activation,
    string channelName,
    TMessage request) : IZLinkRequestCall
{
    private readonly ZLinkCallMetadata _metadata = new();
    private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;
    private TimeSpan? _timeout;

    public IZLinkRequestCall Timeout(TimeSpan timeout)
    {
        ZLinkRequestTimeoutValidation.Validate(timeout, nameof(timeout));
        _timeout = timeout;
        return this;
    }

    public IZLinkRequestCall Metadata(string key, string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    public IZLinkRequestCall Metadata(ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default)
    {
        return ExecuteAsync<TReply>(cancellationToken);
    }

    public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default)
    {
        return ZLinkApplicationExecutionContext
            .RequireYieldTurn(_turn, "Channel request")
            .YieldFrameworkCallAsync(ExecuteAsync<TReply>, cancellationToken);
    }

    private async ValueTask<TReply> ExecuteAsync<TReply>(CancellationToken cancellationToken)
    {
        var timeout = _timeout ?? activation.DefaultRequestTimeout;
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            channelName,
            ZLinkMessageNameResolver.ResolveFromMessage(request),
            timeout);
        var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(header, request, activation.Codecs);
        var reply = await activation.OutboundEndpoint.RequestToChannelAsync(
            channelName,
            parts,
            timeout,
            cancellationToken,
            _metadata.Encode());
        return ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<TReply>(
            reply,
            "SPOT channel request reply is empty.",
            "SPOT channel request failed.",
            activation.Codecs);
    }


}
