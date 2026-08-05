using System.Diagnostics;
using Systems.Zlink.Stream.Connector.Runtime;
namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorClient(
    ZLinkFrameworkRuntime runtime) : IZLinkActorClient
{
    public IZLinkActorSendCall SendToActor<TMessage>(
        string actorId,
        TMessage message)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        return new ZLinkActorSendCall<TMessage>(this, actorId, message);
    }

    public IZLinkActorRequestCall RequestToActor<TRequest>(
        string actorId,
        TRequest request)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        return new ZLinkActorRequestCall<TRequest>(this, actorId, request);
    }

    internal IZLinkActorRequestCall RequestToActorExact<TRequest>(
        string meshName,
        ActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        TRequest request) =>
        new ZLinkActorRequestCall<TRequest>(
            this,
            actor.ActorId,
            request,
            new ResolvedActorRoute(
                meshName,
                actor,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration));

    private async ValueTask<ZLinkOneWaySubmitResult> SubmitSendAsync<TMessage>(
        string actorId,
        string packetName,
        TMessage message,
        ZLinkCallMetadata metadata,
        CancellationToken cancellationToken,
        ResolvedActorRoute? fixedRoute = null)
    {
        using var operation = runtime.EnterOperation();
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            runtime.Flow.CaptureEnabled);
        cancellationToken.ThrowIfCancellationRequested();
        var route = fixedRoute
            ?? await ResolveActorRouteAsync(actorId, cancellationToken)
                .ConfigureAwait(false);
        var actor = route.ActorRef;
        var meshName = route.MeshName;
        var targetNodeGeneration = route.TargetNodeGeneration;
        var authorityOwnerGeneration = route.AuthorityOwnerGeneration;
        var ownerLeaseGeneration = route.OwnerLeaseGeneration;
        var nodeRuntime = runtime.GetMeshNodeRuntime(meshName);
        if (authorityOwnerGeneration != 0)
            nodeRuntime.ObserveActorAuthority(
                actor.ToBackend(),
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration);
        EnsureRouteAvailable(nodeRuntime, actor);
        var parts = CreatePacketParts(
            ZlinkStreamMessageKind.Send,
            null,
            packetName,
            message,
            metadata);
        try
        {
            TraceSent(actor, packetName, parts, ZLinkDispatchMessageKind.ActorSend);
            var result = await nodeRuntime.SendToActorAsync(
                    actor.ToBackend(),
                    parts,
                    cancellationToken)
                .ConfigureAwait(false);
            if (result.Status == ZLinkOneWaySubmitStatus.TargetNotFound)
                InvalidateActorRoute(actorId);
            return result;
        }
        catch (ZLinkFrameworkException failure)
            when (ZLinkMeshCallSupport.TryMapSubmitFailure(failure, out var failed))
        {
            if (IsStaleRoute(failure))
                InvalidateActorRoute(actorId);
            return failed;
        }
    }

    private async ValueTask<TReply> RequestAsync<TRequest, TReply>(
        string actorId,
        string packetName,
        TRequest request,
        TimeSpan? timeout,
        ZLinkCallMetadata metadata,
        CancellationToken cancellationToken,
        ResolvedActorRoute? fixedRoute = null)
    {
        var started = timeout is null ? 0 : Stopwatch.GetTimestamp();
        using var operation = runtime.EnterOperation(countAsRequest: true);
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            runtime.Flow.CaptureEnabled);
        var route = fixedRoute
            ?? await ResolveActorRouteAsync(actorId, cancellationToken)
                .ConfigureAwait(false);
        var actor = route.ActorRef;
        var meshName = route.MeshName;
        var targetNodeGeneration = route.TargetNodeGeneration;
        var authorityOwnerGeneration = route.AuthorityOwnerGeneration;
        var ownerLeaseGeneration = route.OwnerLeaseGeneration;
        var nodeRuntime = await GetActorSpotNodeAsync(meshName, cancellationToken).ConfigureAwait(false);
        if (authorityOwnerGeneration != 0)
            nodeRuntime.ObserveActorAuthority(
                actor.ToBackend(),
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration);
        EnsureRouteAvailable(nodeRuntime, actor);
        var node = nodeRuntime.Node;
        var remainingTimeout = RemainingTimeout(timeout, started);
        var parts = CreatePacketParts(
            ZlinkStreamMessageKind.Request,
            new ZlinkStreamRequestSeq(1),
            packetName,
            request,
            metadata);
        TraceSent(actor, packetName, parts, ZLinkDispatchMessageKind.ActorRequest);
        try
        {
            return await SubmitActorRequestAsync<TReply>(
                    node,
                    actor.ToBackend(),
                    parts,
                    remainingTimeout,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZLinkFrameworkException failure) when (IsStaleRoute(failure))
        {
            InvalidateActorRoute(actorId);
            throw;
        }
    }

    private async ValueTask<ResolvedActorRoute> ResolveActorRouteAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        var rows = runtime.Services.GetService(typeof(ZLinkStoreLocationResolvers))
            as ZLinkStoreLocationResolvers;
        if (rows is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Actor direct messaging requires a Location Store.");
        var resolution = await rows.ResolveActorRowWithStatusAsync(
                new ZLinkActorLocationKey(actorId),
                cancellationToken)
            .ConfigureAwait(false);
        if (resolution.Row is null)
        {
            var kind = resolution.Kind == ZLinkLocationResolutionKind.KnownUnavailable
                ? ZLinkFrameworkErrorKind.Unavailable
                : ZLinkFrameworkErrorKind.NotFound;
            throw new ZLinkFrameworkException(
                kind,
                kind == ZLinkFrameworkErrorKind.Unavailable
                    ? $"Actor route '{actorId}' is currently unavailable."
                    : $"Actor route '{actorId}' was not found.");
        }
        var row = resolution.Row;
        return new ResolvedActorRoute(
            row.MeshName,
            row.ActorRef,
            row.OwnerNodeGeneration,
            row.AuthorityOwnerGeneration,
            checked((ulong)row.LeaseGeneration));
    }

    private static TimeSpan? RemainingTimeout(
        TimeSpan? timeout,
        long started)
    {
        if (timeout is null) return null;
        var remaining = timeout.Value
                        - Stopwatch.GetElapsedTime(started);
        if (remaining <= TimeSpan.Zero)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                "Actor request timed out before transport delivery.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        return remaining;
    }

    private void InvalidateActorRoute(string actorId)
    {
        if (runtime.Services.GetService(typeof(ZLinkStoreLocationResolvers))
            is ZLinkStoreLocationResolvers rows)
            rows.InvalidateActorRoute(new ZLinkActorLocationKey(actorId));
    }

    private static bool IsStaleRoute(ZLinkFrameworkException failure) =>
        failure.Kind is ZLinkFrameworkErrorKind.NotFound
            or ZLinkFrameworkErrorKind.Unavailable;

    private void TraceSent(
        ActorRef actor,
        string packetName,
        IReadOnlyList<Message> parts,
        ZLinkDispatchMessageKind messageKind)
    {
        if (!runtime.Flow.Enabled(ZLinkMessageFlowOutcome.Sent)) return;

        var header = ZLinkStreamProtocolDefaults.DecodeHeader(parts[0].AsReadOnlyMemory());
        runtime.Flow.Trace(new ZLinkMessageFlowEvent(
            ZLinkMessageFlowOutcome.Sent,
            ZLinkDispatchErrorSurface.SpotActor,
            messageKind,
            packetName,
            CorrelationId: header.CorrelationId,
            ActorId: actor.ActorId));
    }

    private async ValueTask<TReply> SubmitActorRequestAsync<TReply>(
        IZLinkBackendSpotNode node,
        ZLinkBackendActorRef actor,
        IReadOnlyList<Message> parts,
        TimeSpan? timeout,
        CancellationToken cancellationToken)
    {
        IReadOnlyList<Message> reply;
        try
        {
            reply = await node.RequestToActorAsync(
                    actor,
                    parts,
                    timeout,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZlinkSubmitException error)
        {
            throw MapSubmitException(error, "Actor request");
        }
        catch (ZlinkRequestException error)
        {
            throw MapRequestException(error, "Actor request");
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }

        try
        {
            return ZLinkActorReplyDecoder.Decode<TReply>(reply);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(reply);
        }
    }

    private async ValueTask<ZLinkSpotNodeRuntime> GetActorSpotNodeAsync(
        string meshName,
        CancellationToken cancellationToken)
    {
        await runtime.EnsureStartedStateAsync(cancellationToken).ConfigureAwait(false);
        return runtime.GetMeshNodeRuntime(meshName);
    }

    private static void EnsureRouteAvailable(
        ZLinkSpotNodeRuntime node,
        ActorRef actor)
    {
        if (actor.NodeRid == node.Node.RoutingId
            || !node.IsExplicitManualRouterRouteDisconnected(actor.NodeRid))
            return;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            $"Actor route to node '{actor.NodeRid}' is not connected.",
            ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private static IReadOnlyList<Message> CreatePacketParts<TMessage>(
        ZlinkStreamMessageKind kind,
        ZlinkStreamRequestSeq? requestSeq,
        string packetName,
        TMessage message,
        ZLinkCallMetadata? callMetadata = null)
    {
        var metadata = callMetadata?.ToStreamMetadata() ?? ZlinkStreamMetadata.Empty;
        var flags = requestSeq is null
            ? ZlinkStreamHeaderFlags.None
            : ZlinkStreamHeaderFlags.HasRequestSeq;
        if (metadata.Count != 0) flags |= ZlinkStreamHeaderFlags.HasMetadata;
        var header = new ZlinkStreamHeader(
            kind,
            ZlinkStreamCodec.Json,
            flags,
            requestSeq,
            packetName,
            metadata,
            ZlinkStreamCorrelation.Next());
        var payload = ZLinkEnvelopeCodec.EncodeJsonBytes(message, message?.GetType() ?? typeof(TMessage));
        return
        [
            Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
            Message.From(payload)
        ];
    }

    private static Exception MapSubmitException(
        ZlinkSubmitException error,
        string operationName)
    {
        return error.Result switch
        {
            ZlinkSubmitException.ErrorCode.NotConnected => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} failed because the target route is not connected.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                error),
            ZlinkSubmitException.ErrorCode.NotFound => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"{operationName} failed because the actor route was not found.",
                innerException: error),
            _ => ZLinkRequestFailureMapper.CreateSubmitException(error, operationName)
        };
    }

    private static Exception MapRequestException(
        ZlinkRequestException error,
        string operationName)
    {
        return error.Result switch
        {
            ZlinkRequestException.ErrorCode.NotConnected => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} failed because the target route is not connected.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                error),
            ZlinkRequestException.ErrorCode.NotFound => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"{operationName} failed because the actor route was not found.",
                innerException: error),
            ZlinkRequestException.ErrorCode.Conflict => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} failed because the actor location is stale.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                error),
            _ => ZLinkRequestFailureMapper.CreateCompletionException(
                (RequestResult)(int)error.Result,
                operationName)
        };
    }

    private sealed class ZLinkActorSendCall<TMessage>(
        ZLinkActorClient client,
        string actorId,
        TMessage message) : IZLinkActorSendCall
    {
        private readonly ZLinkCallMetadata _metadata = new();
        private readonly ZLinkOneWayCallGate _submission = new("Actor send");

        public IZLinkActorSendCall Metadata(string key, string value)
        {
            _metadata.Set(key, value);
            return this;
        }

        public IZLinkActorSendCall Metadata(ZLinkMessageMetadata metadata)
        {
            _metadata.Merge(metadata);
            return this;
        }

        public ValueTask Async(
            CancellationToken cancellationToken = default)
        {
            _submission.Claim();
            return client.SubmitSendAsync(
                actorId,
                ZLinkMessageNameResolver.ResolveFromMessage(message),
                message,
                _metadata,
                cancellationToken).EnsureAcceptedAsync(
                    "Actor send",
                    ZLinkFrameworkErrorKind.NotFound);
        }
    }

    private sealed class ZLinkActorRequestCall<TRequest>(
        ZLinkActorClient client,
        string actorId,
        TRequest request,
        ResolvedActorRoute? fixedRoute = null) : IZLinkActorRequestCall
    {
        private readonly ZLinkCallMetadata _metadata = new();
        private readonly ZLinkApplicationExecutionScope? _executionScope =
            ZLinkApplicationExecutionContext.Current;
        private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;
        private TimeSpan? _timeout;

        public IZLinkActorRequestCall Timeout(TimeSpan timeout)
        {
            ZLinkRequestTimeoutValidation.Validate(timeout, nameof(timeout));
            _timeout = timeout;
            return this;
        }

        public IZLinkActorRequestCall Metadata(string key, string value)
        {
            _metadata.Set(key, value);
            return this;
        }

        public IZLinkActorRequestCall Metadata(ZLinkMessageMetadata metadata)
        {
            _metadata.Merge(metadata);
            return this;
        }

        public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default)
        {
            ZLinkApplicationExecutionContext.RejectActorRequestWhenSameClaim(
                actorId,
                _executionScope);
            return ExecuteAsync<TReply>(cancellationToken);
        }

        public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default)
        {
            ZLinkApplicationExecutionContext.RejectActorRequestWhenSameClaim(
                actorId,
                _executionScope);
            return ZLinkApplicationExecutionContext
                .RequireYieldTurn(_turn, "Actor request")
                .YieldFrameworkCallAsync(ExecuteAsync<TReply>, cancellationToken);
        }

        private ValueTask<TReply> ExecuteAsync<TReply>(CancellationToken cancellationToken)
        {
            return client.RequestAsync<TRequest, TReply>(
                actorId,
                ZLinkMessageNameResolver.ResolveFromMessage(request),
                request,
                _timeout,
                _metadata,
                cancellationToken,
                fixedRoute);
        }
    }

    private readonly record struct ResolvedActorRoute(
        string MeshName,
        ActorRef ActorRef,
        ulong TargetNodeGeneration,
        ulong AuthorityOwnerGeneration,
        ulong OwnerLeaseGeneration);
}
