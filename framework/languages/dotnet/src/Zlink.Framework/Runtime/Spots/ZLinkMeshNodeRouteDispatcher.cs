using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.Runtime.Spots;

// RouteMesh 10.0.0 node-level inbound dispatch for the MeshNode builder's
// registered handlers (spec server 21-mesh-node + dotnet 02-handler-interfaces):
//   NodeSend / NodeRequest       -> AddRouteMesh(...).AddRouteSendHandler /
//                                   AddRouteRequestHandler
//                                   (IZLinkRouteSendHandler / IZLinkRouteRequestHandler)
//   ChannelSend / ChannelRequest -> ChannelName(...).AddSendHandler /
//                                   AddRequestHandler
//                                   (IZLinkSendHandler / IZLinkRequestHandler),
//                                   selected by the addressed channel name.
//
// Records arrive from the node dispatch pump (ready-record OwnerKind == Node) via
// IZLinkBackendSpotNode.OnNodeRoute. Requests reply through the record's held reply
// token exactly like the per-spot route plane; the reply envelope is the standard
// Response envelope (ZLinkEnvelopeCodec) the route/channel client decodes. Handler
// invocation reuses the existing route/channel handler-invocation paths
// (ZLinkRouteHandlerInvoker for route handlers, the channel dispatch pipelines for
// channel handlers) rather than a parallel implementation.
internal sealed class ZLinkMeshNodeRouteDispatcher
{
    // Node RID-direct route handlers are not channel-scoped; they share one
    // internal registry channel key so the inbound envelope's channel name is not
    // required to match a configured channel.
    private const string NodeRouteChannel = "";

    private static readonly IReadOnlySet<string> EmptyGroups =
        new HashSet<string>(StringComparer.Ordinal);

    private readonly ZLinkRouteHandlerRegistry _routeHandlers;
    private readonly ZLinkRouteHandlerInvoker _routeInvoker;
    private readonly string _meshName;
    private readonly ZLinkChannelCommandDispatchPipeline _channelCommandPipeline;
    private readonly ZLinkChannelRequestDispatchPipeline _channelRequestPipeline;
    private readonly ZLinkCodecRegistryBuilder _codecs;
    private readonly ZLinkDispatchErrorReporter _dispatchErrors;
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly ZLinkRuntimeTaskRunner _taskRunner;
    private readonly ZLinkCompletionAdmissionOwner _completionAdmission;
    private readonly ZLinkAsyncSubmitter? _replySubmitter;
    private readonly ILogger _logger;
    private readonly object _orderedActorRelayGate = new();
    private readonly Dictionary<string, TaskCompletionSource> _orderedActorRelayTails =
        new(StringComparer.Ordinal);

    private ZLinkMeshNodeRouteDispatcher(
        string meshName,
        ZLinkRouteHandlerRegistry routeHandlers,
        ZLinkRouteHandlerInvoker routeInvoker,
        ZLinkChannelCommandDispatchPipeline channelCommandPipeline,
        ZLinkChannelRequestDispatchPipeline channelRequestPipeline,
        ZLinkCodecRegistryBuilder codecs,
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkFrameworkRuntime runtime,
        ZLinkRuntimeTaskRunner taskRunner,
        ZLinkCompletionAdmissionOwner completionAdmission,
        ZLinkAsyncSubmitter? replySubmitter,
        ILogger logger)
    {
        _meshName = meshName;
        _routeHandlers = routeHandlers;
        _routeInvoker = routeInvoker;
        _channelCommandPipeline = channelCommandPipeline;
        _channelRequestPipeline = channelRequestPipeline;
        _codecs = codecs;
        _dispatchErrors = dispatchErrors;
        _runtime = runtime;
        _taskRunner = taskRunner;
        _completionAdmission = completionAdmission;
        _replySubmitter = replySubmitter;
        _logger = logger;
    }

    // Builds a dispatcher from the SpotNode's registered node-route and
    // channel-membership handlers, or null when the node has none (nothing to wire).
    public static ZLinkMeshNodeRouteDispatcher? Create(
        IServiceProvider services,
        ZLinkFrameworkRegistration registration,
        ZLinkSpotNodeRegistration spotNode,
        ZLinkFrameworkRuntime runtime,
        ZLinkRuntimeTaskRunner taskRunner,
        ZLinkCompletionAdmissionOwner completionAdmission,
        ZLinkAsyncSubmitter? replySubmitter = null)
    {
        var descriptors = BuildRouteDescriptors(spotNode);
        // Router-capable nodes always host the framework's internal
        // remote-session push relay consumer (spec 31 §6): an actor that moved
        // to another node pushes to its bound session through this packet.
        if (spotNode.Router is not null)
            descriptors = descriptors
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkRemoteSessionBindRouteHandler),
                        typeof(IZLinkRouteRequestHandler<
                            ZLinkRemoteSessionBindRequest,
                            ZLinkRemoteSessionBindResponse>),
                        ZLinkMessageKind.Request,
                        ZLinkRemoteSessionBindingProtocol.PacketName)))
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkRemoteSessionUnbindRouteHandler),
                        typeof(IZLinkRouteRequestHandler<
                            ZLinkRemoteSessionUnbindRequest,
                            ZLinkRemoteSessionUnbindResponse>),
                        ZLinkMessageKind.Request,
                        ZLinkRemoteSessionBindingProtocol.UnbindPacketName)))
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkRemoteSessionOwnerTombstoneRouteHandler),
                        typeof(IZLinkRouteRequestHandler<
                            ZLinkRemoteSessionOwnerTombstoneRequest,
                            ZLinkRemoteSessionOwnerTombstoneResponse>),
                        ZLinkMessageKind.Request,
                        ZLinkRemoteSessionBindingProtocol
                            .SessionOwnerTombstonePacketName)))
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkRemoteSessionPushRelayHandler),
                        typeof(IZLinkRouteSendHandler<ZLinkRemoteSessionPushRelay>),
                        ZLinkMessageKind.Command,
                        ZLinkRemoteSessionPushProtocol.PacketName)))
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkRemoteActorFrameRelayHandler),
                        typeof(IZLinkRouteSendHandler<ZLinkRemoteActorFrameRelay>),
                        ZLinkMessageKind.Command,
                        ZLinkRemoteActorFrameProtocol.PacketName)))
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkRemoteActorReplyRelayHandler),
                        typeof(IZLinkRouteSendHandler<ZLinkRemoteActorReplyRelay>),
                        ZLinkMessageKind.Command,
                        ZLinkRemoteActorReplyProtocol.PacketName)))
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkSessionRouteSealHandler),
                        typeof(IZLinkRouteRequestHandler<
                            ZLinkSessionRouteSealRequest,
                            ZLinkSessionRouteSealReply>),
                        ZLinkMessageKind.Request,
                        ZLinkSessionRouteCommitProtocol.SealPacketName)))
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkSessionRouteAbortHandler),
                        typeof(IZLinkRouteRequestHandler<
                            ZLinkSessionRouteAbortRequest,
                            ZLinkSessionRouteSealReply>),
                        ZLinkMessageKind.Request,
                        ZLinkSessionRouteCommitProtocol.AbortPacketName)))
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkSessionRouteCommitHandler),
                        typeof(IZLinkRouteRequestHandler<
                            ZLinkSessionRouteCommitRequest,
                            ZLinkSessionRouteCommitReply>),
                        ZLinkMessageKind.Request,
                        ZLinkSessionRouteCommitProtocol.PacketName)))
                .Append(ToRouteDescriptor(
                    ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                        typeof(ZLinkSessionRouteUnsealHandler),
                        typeof(IZLinkRouteRequestHandler<
                            ZLinkSessionRouteUnsealRequest,
                            ZLinkSessionRouteCommitReply>),
                        ZLinkMessageKind.Request,
                        ZLinkSessionRouteCommitProtocol.UnsealPacketName)))
                ;
        var routeDescriptors = descriptors.ToArray();
        var channelEndpoints = BuildChannelEndpoints(registration, spotNode).ToArray();
        if (routeDescriptors.Length == 0 && channelEndpoints.Length == 0)
            return null;

        var loggerFactory = runtime.Services.GetService<ILoggerFactory>();
        var logger = loggerFactory?.CreateLogger(typeof(ZLinkMeshNodeRouteDispatcher).FullName!)
                     ?? (ILogger)NullLogger.Instance;
        var dispatchErrors = new ZLinkDispatchErrorReporter(
            registration.DispatchOptions,
            ZLinkMessageFlowTracer.CreateLogger(loggerFactory, logger),
            runtime);

        var routeHandlers = new ZLinkRouteHandlerRegistry(routeDescriptors);

        // The channel pipelines are always built (registry may be empty) so a
        // channel record addressed to a channel with no matching handler yields a
        // proper "handler not registered" error reply instead of a silent drop.
        var handlerRegistry = new ZLinkHandlerRegistry(channelEndpoints);
        var handlerDispatcher = new ZLinkHandlerDispatcher(
            services.GetRequiredService<IServiceScopeFactory>(),
            registration);
        var routeInvoker = new ZLinkRouteHandlerInvoker(
            handlerDispatcher,
            registration.Codecs);
        var commandPipeline = new ZLinkChannelCommandDispatchPipeline(
            spotNode.SpotNodeName,
            handlerRegistry,
            handlerDispatcher,
            static _ => EmptyGroups,
            LogLevel.Warning,
            dispatchErrors,
            registration.Codecs,
            logger);
        var requestPipeline = new ZLinkChannelRequestDispatchPipeline(
            spotNode.SpotNodeName,
            handlerRegistry,
            handlerDispatcher,
            static _ => EmptyGroups,
            registration.Codecs,
            dispatchErrors,
            logger);

        return new ZLinkMeshNodeRouteDispatcher(
            spotNode.SpotNodeName,
            routeHandlers,
            routeInvoker,
            commandPipeline,
            requestPipeline,
            registration.Codecs,
            dispatchErrors,
            runtime,
            taskRunner,
            completionAdmission,
            replySubmitter,
            logger);
    }

    // Pump entry point (invoked on the single node drain loop). Dispatch runs on a
    // detached runtime task, mirroring the per-spot route plane; the record retains
    // its own message parts, so the pump can release the Core claim immediately.
    public void Dispatch(ZLinkBackendRouteReceived received)
    {
        if (!TryDispatch(received)) received.Dispose();
    }

    internal bool TryDispatch(ZLinkBackendRouteReceived received)
    {
        if (TryGetOrderedActorRelayKey(received, out var actorId))
        {
            return TryDispatchOrderedActorRelay(actorId, received);
        }

        return _taskRunner.TryRunDetached(
            "mesh-node-route-dispatch",
            ct => DispatchAsync(received, ct));
    }

    private bool TryGetOrderedActorRelayKey(
        ZLinkBackendRouteReceived received,
        out string actorId)
    {
        actorId = string.Empty;
        if (received.ChannelName is not null || received.Parts.Count < 2)
            return false;

        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(received.Parts);
            if (header.Kind != ZLinkMessageKind.Command
                || !string.Equals(
                    header.MessageName,
                    ZLinkRemoteActorFrameProtocol.PacketName,
                    StringComparison.Ordinal))
                return false;

            var relay = ZLinkEnvelopeCodec.DecodeBody(
                    received.Parts,
                    typeof(ZLinkRemoteActorFrameRelay),
                    _codecs)
                as ZLinkRemoteActorFrameRelay;
            if (string.IsNullOrWhiteSpace(relay?.ActorId))
                return false;

            actorId = relay.ActorId;
            return true;
        }
        catch
        {
            // Normal dispatch owns malformed-frame reporting.
            return false;
        }
    }

    private bool TryDispatchOrderedActorRelay(
        string actorId,
        ZLinkBackendRouteReceived received)
    {
        Task prior;
        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_orderedActorRelayGate)
        {
            prior = _orderedActorRelayTails.TryGetValue(actorId, out var tail)
                ? tail.Task
                : Task.CompletedTask;
            _orderedActorRelayTails[actorId] = completion;
        }

        if (_taskRunner.TryRunDetached(
                "mesh-node-actor-relay-dispatch",
                ct => DispatchOrderedActorRelayAsync(
                    actorId,
                    received,
                    prior,
                    completion,
                    ct)))
            return true;

        CompleteOrderedActorRelay(actorId, completion);
        return false;
    }

    private async ValueTask DispatchOrderedActorRelayAsync(
        string actorId,
        ZLinkBackendRouteReceived received,
        Task prior,
        TaskCompletionSource completion,
        CancellationToken cancellationToken)
    {
        try
        {
            await prior.ConfigureAwait(false);
            await DispatchAsync(received, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            CompleteOrderedActorRelay(actorId, completion);
        }
    }

    private void CompleteOrderedActorRelay(
        string actorId,
        TaskCompletionSource completion)
    {
        completion.TrySetResult();
        lock (_orderedActorRelayGate)
            if (_orderedActorRelayTails.TryGetValue(actorId, out var current)
                && ReferenceEquals(current, completion))
                _orderedActorRelayTails.Remove(actorId);
    }

    private async ValueTask DispatchAsync(
        ZLinkBackendRouteReceived received,
        CancellationToken cancellationToken)
    {
        received.StartDispatch();
        using (received)
        using (var completionPermit = received.CanReply
                   && !IsInfrastructureRelay(received)
                   ? await _completionAdmission.AcquireResponderAsync(cancellationToken)
                       .ConfigureAwait(false)
                   : null)
        {
            if (received.Parts.Count == 0)
            {
                HandleProtocolError(received, ZLinkEnvelopeCodec.MissingHeader());
                return;
            }

            ZLinkEnvelopeHeader header;
            try
            {
                header = ZLinkEnvelopeCodec.DecodeHeader(received.Parts);
                ZLinkEnvelopeCodec.ValidateDispatchHeader(header);
            }
            catch (ZLinkEnvelopeProtocolException protocolError)
            {
                HandleProtocolError(received, protocolError);
                return;
            }

            using var flow = ZLinkFlowContext.Enter(
                header.FlowId,
                header.FlowOrigin,
                _dispatchErrors.Flow.CaptureEnabled,
                ZLinkFlowOrigin.Inbound);

            var infrastructure = IsInfrastructureRelay(received, header);
            ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease operation;
            if (infrastructure)
            {
                operation = new ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease();
            }
            //  This dispatcher only invokes registered channel and node
            //  route handlers. Neither is object work, so an expired owner
            //  lease must not turn them away (spec 21 §4).
            else if (!_runtime.TryEnterInboundOperation(
                         header.Kind == ZLinkMessageKind.Request,
                         out operation,
                         ownsObjectWork: false))
            {
                //  With ownsObjectWork false the only refusal here is the drain
                //  seal, which spec 06 §13.1 classifies as `ShuttingDown` -
                //  new admission closed by host shutdown - rather than
                //  `Rejected`, which is an application policy decision. The
                //  distinction is what lets a select-one caller reselect.
                if (header.Kind == ZLinkMessageKind.Request)
                    await ReplyErrorAsync(
                        received,
                        header,
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.ShuttingDown,
                            "MeshNode application admission is sealed for drain."), completionPermit!, cancellationToken).ConfigureAwait(false);
                return;
            }

            using (operation)
            {
                if (received.ChannelName is { } channelName)
                    await DispatchChannelAsync(received, channelName, header, completionPermit, cancellationToken)
                        .ConfigureAwait(false);
                else
                    await DispatchNodeRouteAsync(received, header, completionPermit, cancellationToken)
                        .ConfigureAwait(false);
            }
        }
    }

    private static bool IsInfrastructureRelay(
        ZLinkBackendRouteReceived received)
    {
        if (received.Parts.Count == 0) return false;
        try
        {
            return IsInfrastructureRelay(
                received, ZLinkEnvelopeCodec.DecodeHeader(received.Parts));
        }
        catch
        {
            return false;
        }
    }

    private static bool IsInfrastructureRelay(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader header) =>
        received.ChannelName is null
        && ((header.Kind == ZLinkMessageKind.Command
             && header.MessageName is ZLinkRemoteSessionPushProtocol.PacketName
                 or ZLinkRemoteActorFrameProtocol.PacketName
                 or ZLinkRemoteActorReplyProtocol.PacketName)
            || (header.Kind == ZLinkMessageKind.Request
                && header.MessageName is ZLinkSessionRouteCommitProtocol.PacketName
                    or ZLinkSessionRouteCommitProtocol.SealPacketName
                    or ZLinkSessionRouteCommitProtocol.AbortPacketName
                    or ZLinkSessionRouteCommitProtocol.UnsealPacketName));

    private async ValueTask DispatchNodeRouteAsync(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader header,
        ZLinkCompletionAdmissionOwner.ResponderLease? completionPermit,
        CancellationToken cancellationToken)
    {
        var isRequest = header.Kind == ZLinkMessageKind.Request;
        var sourceRid = received.SourceNodeRid ?? default;
        var scope = CreateScope(header, isRequest);
        scope.Trace(_dispatchErrors, ZLinkMessageFlowOutcome.Received);

        if (!_routeHandlers.TryGet(
                NodeRouteChannel,
                isRequest ? ZLinkMessageKind.Request : ZLinkMessageKind.Command,
                header.MessageName,
                out var descriptor)
            || descriptor is null)
        {
            if (isRequest)
            {
                var error = new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    $"No node route request handler is registered for '{header.MessageName}'.");
                scope.HandlerMissing(
                    _logger,
                    _dispatchErrors,
                    LogLevel.Error,
                    ZLinkDispatchErrorAction.ReplyError,
                    error);
                await ReplyErrorAsync(received, header, error, completionPermit!, cancellationToken)
                    .ConfigureAwait(false);
            }
            else
            {
                scope.Dropped(_logger, _dispatchErrors, LogLevel.Warning);
            }

            return;
        }

        if (!isRequest)
        {
            try
            {
                await _routeInvoker.InvokeSendAsync(
                        descriptor,
                        _meshName,
                        sourceRid,
                        header,
                        received.Parts,
                        cancellationToken,
                        received.Metadata)
                    .ConfigureAwait(false);
                scope.Trace(_dispatchErrors, ZLinkMessageFlowOutcome.Dispatched);
            }
            catch (Exception ex)
            {
                scope.HandlerException(
                    _logger,
                    _dispatchErrors,
                    LogLevel.Error,
                    ZLinkDispatchErrorAction.Drop,
                    ex);
            }

            return;
        }

        ZLinkRouteHandlerReply reply;
        try
        {
            reply = await _routeInvoker.InvokeRequestAsync(
                    descriptor,
                    _meshName,
                    sourceRid,
                    header,
                    received.Parts,
                    cancellationToken,
                    received.Metadata)
                .ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            await ReplyErrorAsync(
                    received, header, ex, completionPermit!, cancellationToken)
                .ConfigureAwait(false);
            scope.HandlerException(
                _logger,
                _dispatchErrors,
                null,
                ZLinkDispatchErrorAction.ReplyError,
                ex);
            return;
        }

        try
        {
            await ReplyResponseAsync(
                    received,
                    header,
                    reply.Message,
                    reply.MessageType,
                    completionPermit!,
                    cancellationToken)
                .ConfigureAwait(false);
            }
        catch (Exception)
        {
            throw;
        }
        scope.Trace(_dispatchErrors, ZLinkMessageFlowOutcome.Replied);
    }

    private async ValueTask DispatchChannelAsync(
        ZLinkBackendRouteReceived received,
        string channelName,
        ZLinkEnvelopeHeader header,
        ZLinkCompletionAdmissionOwner.ResponderLease? completionPermit,
        CancellationToken cancellationToken)
    {
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.Channel,
            "Channel",
            header.Kind == ZLinkMessageKind.Request
                ? ZLinkDispatchMessageKind.Request
                : ZLinkDispatchMessageKind.Send,
            header.Kind == ZLinkMessageKind.Request ? "Request" : "Send",
            header.MessageName,
            channelName,
            header.ContentType,
            header.CorrelationId);
        scope.Trace(_dispatchErrors, ZLinkMessageFlowOutcome.Received);

        switch (header.Kind)
        {
            case ZLinkMessageKind.Request:
                await _channelRequestPipeline.DispatchAsync(
                        channelName,
                        received.Parts,
                        header,
                        (replyHeader, reply, replyType) =>
                            SubmitEnvelopeAsync(received, replyHeader, reply, replyType, completionPermit!, cancellationToken),
                        errorHeader => SubmitEnvelopeAsync(received, errorHeader, null, null, completionPermit!, cancellationToken),
                        cancellationToken,
                        received.Metadata,
                        received.SourceNodeRid)
                    .ConfigureAwait(false);
                return;
            case ZLinkMessageKind.Command:
                await _channelCommandPipeline.DispatchAsync(
                        channelName,
                        received.Parts,
                        header,
                        cancellationToken,
                        received.Metadata,
                        received.SourceNodeRid)
                    .ConfigureAwait(false);
                return;
        }
    }

    private ValueTask ReplyResponseAsync(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader requestHeader,
        object? reply,
        Type? replyType,
        ZLinkCompletionAdmissionOwner.ResponderLease completionPermit,
        CancellationToken cancellationToken) =>
        SubmitEnvelopeAsync(
            received,
            ZLinkChannelReplyWriter.CreateReplyHeader(
                ZLinkMessageKind.Response,
                requestHeader.ChannelName,
                requestHeader),
            reply,
            replyType,
            completionPermit,
            cancellationToken);

    private ValueTask ReplyErrorAsync(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader requestHeader,
        Exception exception,
        ZLinkCompletionAdmissionOwner.ResponderLease completionPermit,
        CancellationToken cancellationToken) =>
        SubmitEnvelopeAsync(
            received,
            ZLinkChannelReplyWriter.CreateErrorHeader(
                requestHeader.ChannelName,
                requestHeader,
                exception),
            null,
            null,
            completionPermit,
            cancellationToken);

    private ValueTask SubmitEnvelopeAsync(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader header,
        object? body,
        Type? bodyType,
        ZLinkCompletionAdmissionOwner.ResponderLease completionPermit,
        CancellationToken cancellationToken)
    {
        if (!received.CanReply)
            return ValueTask.CompletedTask;

        var replyParts = ZLinkEnvelopeCodec.EncodeParts(
            header, body, bodyType, _codecs);
        if (_replySubmitter is null)
            return ZLinkSpotReplySubmitter.SubmitDirectAsync(
                received, replyParts, completionPermit, cancellationToken);
        return ZLinkSpotReplySubmitter.SubmitAsync(
            _replySubmitter,
            received,
            replyParts,
            completionPermit,
            cancellationToken);
    }

    private void SubmitEnvelopeUnreserved(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader header,
        object? body,
        Type? bodyType)
    {
        if (!received.CanReply) return;
        var replyParts = ZLinkEnvelopeCodec.EncodeParts(
            header, body, bodyType, _codecs);
        ZLinkSpotReplySubmitter.SubmitAndDispose(received, replyParts);
    }

    private void HandleProtocolError(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeProtocolException protocolError)
    {
        var header = protocolError.Header;
        var isRequest = received.RequestSeq.HasValue || received.CanReply;
        var canReply = isRequest
                       && received.CanReply
                       && ZLinkEnvelopeCodec.CanCorrelateReply(header);
        var validFlow = ZLinkEnvelopeCodec.ValidFlow(header);
        using var flow = ZLinkFlowContext.Enter(
            validFlow.FlowId,
            validFlow.FlowOrigin,
            _dispatchErrors.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound);
        _dispatchErrors.Report(new ZLinkDispatchFailure(
            ZLinkDispatchErrorSurface.RouteMeshChannel,
            isRequest
                ? ZLinkDispatchMessageKind.Request
                : ZLinkDispatchMessageKind.Send,
            ZLinkDispatchErrorReason.InvalidFrame,
            canReply
                ? ZLinkDispatchErrorAction.ReplyError
                : ZLinkDispatchErrorAction.Drop,
            header.MessageName,
            received.ChannelName ?? string.Empty,
            CorrelationId: header.CorrelationId,
            Exception: protocolError));
        if (!canReply) return;

        SubmitEnvelopeUnreserved(
            received,
            ZLinkChannelReplyWriter.CreateProtocolErrorHeader(
                received.ChannelName ?? string.Empty,
                header,
                protocolError.Message),
            null,
            null);
    }

    private ZLinkDispatchFlowScope CreateScope(ZLinkEnvelopeHeader header, bool isRequest)
    {
        return new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.RouteMeshChannel,
            "RouteMeshChannel",
            isRequest ? ZLinkDispatchMessageKind.Request : ZLinkDispatchMessageKind.Send,
            isRequest ? "Request" : "Send",
            header.MessageName,
            header.ChannelName,
            header.ContentType,
            header.CorrelationId);
    }

    private static IEnumerable<ZLinkRouteHandlerDescriptor> BuildRouteDescriptors(
        ZLinkSpotNodeRegistration spotNode)
    {
        foreach (var handler in spotNode.RouteSendHandlers)
        {
            var handlerInterface = typeof(IZLinkRouteSendHandler<>).MakeGenericType(handler.MessageType);
            yield return ToRouteDescriptor(
                ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                    handler.HandlerType,
                    handlerInterface,
                    ZLinkMessageKind.Command,
                    handler.PacketName));
        }

        foreach (var handler in spotNode.RouteRequestHandlers)
        {
            var handlerInterface = typeof(IZLinkRouteRequestHandler<,>).MakeGenericType(
                handler.MessageType,
                handler.ReplyType!);
            yield return ToRouteDescriptor(
                ZLinkHandlerScanner.CreateExplicitRouteInterfaceDescriptor(
                    handler.HandlerType,
                    handlerInterface,
                    ZLinkMessageKind.Request,
                    handler.PacketName));
        }
    }

    private static ZLinkRouteHandlerDescriptor ToRouteDescriptor(
        ZLinkRouteHandlerEndpointDescriptor endpoint)
    {
        return new ZLinkRouteHandlerDescriptor(
            endpoint.Kind,
            NodeRouteChannel,
            endpoint.MessageName,
            endpoint.DeclaringType,
            endpoint.MessageType,
            endpoint.ReplyType,
            endpoint.Invoker);
    }

    private static IEnumerable<ZLinkHandlerEndpointDescriptor> BuildChannelEndpoints(
        ZLinkFrameworkRegistration registration,
        ZLinkSpotNodeRegistration spotNode)
    {
        foreach (var membership in spotNode.ChannelMemberships)
        {
            if (!membership.IsServer)
                continue;

            foreach (var endpoint in registration.ScannedHandlerCatalog.ChannelEndpoints)
                if (endpoint.Groups.Any(membership.HandlerGroups.Contains)
                    && endpoint.Kind is ZLinkMessageKind.Command or ZLinkMessageKind.Request)
                    yield return endpoint with { ExplicitChannelName = membership.ChannelName };

            foreach (var handler in membership.SendHandlers)
            {
                var handlerInterface = typeof(IZLinkSendHandler<>).MakeGenericType(handler.MessageType);
                yield return ZLinkHandlerScanner.CreateExplicitInterfaceDescriptor(
                    handler.HandlerType,
                    handlerInterface,
                    ZLinkMessageKind.Command,
                    membership.ChannelName,
                    handler.PacketName);
            }

            foreach (var handler in membership.RequestHandlers)
            {
                var handlerInterface = typeof(IZLinkRequestHandler<,>).MakeGenericType(
                    handler.MessageType,
                    handler.ReplyType!);
                yield return ZLinkHandlerScanner.CreateExplicitInterfaceDescriptor(
                    handler.HandlerType,
                    handlerInterface,
                    ZLinkMessageKind.Request,
                    membership.ChannelName,
                    handler.PacketName);
            }
        }
    }
}
