using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActivationDispatcher
{
    private readonly ZLinkSpotActorJoinDispatcher _actorJoinDispatcher;
    private readonly ZLinkActorInboundPipeline _actorPipeline;
    private readonly ZLinkSpotActorPacketDispatcher _actorPacketDispatcher;
    private readonly ZLinkDispatchErrorReporter _dispatchErrors;
    private readonly ILogger<ZLinkSpotActivationDispatcher> _logger;
    private readonly ZLinkSpotRouteDispatcher _routeDispatcher;
    private readonly ZLinkSpotActorMembership actors;
    private readonly string channelName;
    private readonly Func<ZLinkSpotHandlerInvoker> handlerInvoker;
    private readonly IZLinkBackendSpot nativeSpot;
    private readonly ZLinkFrameworkRuntime runtime;
    private readonly ZLinkSpotSubscriptionRegistry subscriptions;

    public ZLinkSpotActivationDispatcher(
        ZLinkFrameworkRuntime runtime,
        IZLinkBackendSpot nativeSpot,
        string channelName,
        string spotId,
        ZLinkSpotPacketRegistry packets,
        ZLinkSpotActorJoinRegistry actorJoins,
        ZLinkSpotActorMembership actors,
        ZLinkSpotSubscriptionRegistry subscriptions,
        Func<ZLinkSpotActorHandlerRegistry?> actorHandlers,
        Func<ZLinkSpotHandlerInvoker> handlerInvoker,
        Func<IZLinkActor, CancellationToken, ValueTask>? commitAcceptedActorJoin = null,
        ZLinkAsyncSubmitter? replySubmitter = null,
        ZLinkCompletionAdmissionOwner? completionAdmission = null,
        bool acceptActorJoinWithoutHandler = false)
    {
        this.runtime = runtime;
        this.nativeSpot = nativeSpot;
        this.channelName = channelName;
        this.actors = actors;
        this.subscriptions = subscriptions;
        this.handlerInvoker = handlerInvoker;
        var loggerFactory = runtime.Services.GetService<ILoggerFactory>();
        var flowLogger = ZLinkMessageFlowTracer.CreateLogger(loggerFactory);
        _logger = loggerFactory?.CreateLogger<ZLinkSpotActivationDispatcher>()
                  ?? NullLogger<ZLinkSpotActivationDispatcher>.Instance;
        _dispatchErrors = new ZLinkDispatchErrorReporter(
            runtime.Registration.DispatchOptions,
            flowLogger,
            runtime);
        _actorPacketDispatcher = new ZLinkSpotActorPacketDispatcher(
            actorHandlers,
            handlerInvoker,
            _dispatchErrors,
            runtime.Services.GetService<ILoggerFactory>()?.CreateLogger<ZLinkSpotActorPacketDispatcher>()
            ?? NullLogger<ZLinkSpotActorPacketDispatcher>.Instance);
        _actorPipeline = new ZLinkActorInboundPipeline(
            runtime,
            new ZLinkUserSpotActorInboundEndpoint(runtime, actors, _actorPacketDispatcher));
        _actorJoinDispatcher = new ZLinkSpotActorJoinDispatcher(
            runtime,
            nativeSpot,
            channelName,
            actorJoins,
            actors,
            handlerInvoker,
            runtime.Services.GetService<ILoggerFactory>()?.CreateLogger<ZLinkSpotActorJoinDispatcher>(),
            commitAcceptedActorJoin,
            _dispatchErrors,
            acceptActorJoinWithoutHandler);
        _routeDispatcher = new ZLinkSpotRouteDispatcher(
            channelName,
            spotId,
            packets,
            handlerInvoker,
            runtime.Registration.Codecs,
            _dispatchErrors,
            DispatchInternalRoutePacketAsync,
            runtime.Services.GetService<ILoggerFactory>()?.CreateLogger<ZLinkSpotRouteDispatcher>(),
            replySubmitter,
            completionAdmission);
    }

    public ZLinkSpotActorPacketDispatcher ActorPackets => _actorPacketDispatcher;

    public async ValueTask<bool> DispatchActorJoinDrainAsync(
        CancellationToken cancellationToken)
    {
        var startedAt = Stopwatch.GetTimestamp();
        var count = 0;
        long bytes = 0;
        while (!cancellationToken.IsCancellationRequested)
        {
            if (ZLinkReceiveBatchBudget.IsExhausted(count, bytes, startedAt))
                return true;
            ZLinkBackendActorJoinRequest? request;
            try
            {
                request = nativeSpot.RecvActorJoin(RecvFlags.DontWait);
            }
            catch (ZlinkRecvException ex)
                when (ex.Result == ZlinkRecvException.ErrorCode.NoData)
            {
                return false;
            }

            if (request is null) return false;
            bytes = checked(
                bytes + ZLinkReceiveBatchBudget.MeasureParts(request.Parts));
            count++;

            try
            {
                request.DispatchLease?.StartDispatch();
                await _actorJoinDispatcher.DispatchAsync(request, cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(request.Parts);
                request.DispatchLease?.Dispose();
            }
        }
        return false;
    }

    public async ValueTask DispatchActorFramesAsync(
        ZLinkSpotActorFrameBatch frames,
        ZLinkSpotSerialExecutor executor,
        CancellationToken cancellationToken)
    {
        await _actorPipeline.DispatchAsync(frames, executor, cancellationToken)
            .ConfigureAwait(false);
    }

    public ValueTask DispatchActorReplayFramesAsync(
        ZLinkSpotActorFrameBatch frames,
        Action<long> acknowledgeFrame,
        CancellationToken cancellationToken)
    {
        return _actorPipeline.DispatchReplayAsync(frames, acknowledgeFrame, cancellationToken);
    }

    public async ValueTask<bool> DispatchRouteDrainAsync(
        CancellationToken cancellationToken)
    {
        var startedAt = Stopwatch.GetTimestamp();
        var count = 0;
        long bytes = 0;
        while (!cancellationToken.IsCancellationRequested)
        {
            if (ZLinkReceiveBatchBudget.IsExhausted(count, bytes, startedAt))
                return true;
            var received = nativeSpot.RecvRoute(RecvFlags.DontWait);
            if (received is null) return false;

            var recordBytes = ZLinkReceiveBatchBudget.MeasureParts(received.Parts);
            bytes = checked(bytes + recordBytes);
            count++;

            await _routeDispatcher.DispatchAsync(received, cancellationToken).ConfigureAwait(false);
        }
        return false;
    }

    public async ValueTask DispatchRouteAsync(
        ZLinkBackendRouteReceived received,
        CancellationToken cancellationToken)
    {
        await _routeDispatcher.DispatchAsync(received, cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask<bool> DispatchInternalRoutePacketAsync(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader header,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(
                header.MessageName,
                ZLinkRemoteActorJoinPackets.RequestPacketName,
                StringComparison.Ordinal)
            && !string.Equals(
                header.MessageName,
                ZLinkRemoteActorJoinPackets.AdmissionPacketName,
                StringComparison.Ordinal)
            && !string.Equals(
                header.MessageName,
                ZLinkRemoteActorJoinPackets.AdmissionAbortPacketName,
                StringComparison.Ordinal)
            && !string.Equals(
                header.MessageName,
                ZLinkRemoteActorJoinPackets.CommitPacketName,
                StringComparison.Ordinal)
            && !string.Equals(
                header.MessageName,
                ZLinkRemoteActorJoinPackets.HandoffCompletionPacketName,
                StringComparison.Ordinal))
            return false;

        if (received.Parts.Count < 2)
        {
            ReplyInternalRouteError(
                received,
                header,
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    "Remote actor join request body part is missing."));
            return true;
        }

        try
        {
            if (string.Equals(header.MessageName, ZLinkRemoteActorJoinPackets.AdmissionPacketName, StringComparison.Ordinal))
            {
                var admissionRequest = ZLinkRemoteActorJoinPackets.DecodeAdmissionRequest(received.Parts);
                var admissionReply = await runtime.AdmitRoutedActorJoinAsync(
                        ZLinkSpotId.FromNativeRoutingId(nativeSpot.RoutingId),
                        admissionRequest,
                        cancellationToken)
                    .ConfigureAwait(false);
                var admissionReplyParts = ZLinkSpotReplyEnvelope.EncodeResponseParts(
                    channelName,
                    header.MessageName,
                    header.CorrelationId,
                    admissionReply,
                    typeof(ZLinkRemoteActorAdmissionReply));
                ZLinkSpotReplySubmitter.SubmitAndDispose(received, admissionReplyParts);
                return true;
            }

            if (string.Equals(
                    header.MessageName,
                    ZLinkRemoteActorJoinPackets.AdmissionAbortPacketName,
                    StringComparison.Ordinal))
            {
                var abortRequest =
                    ZLinkRemoteActorJoinPackets.DecodeAdmissionAbortRequest(
                        received.Parts);
                await runtime.AbortRoutedActorJoinAdmissionAsync(
                        ZLinkSpotId.FromNativeRoutingId(
                            nativeSpot.RoutingId),
                        abortRequest,
                        cancellationToken)
                    .ConfigureAwait(false);
                var abortReplyParts = ZLinkSpotReplyEnvelope.EncodeResponseParts(
                    channelName,
                    header.MessageName,
                    header.CorrelationId,
                    abortRequest,
                    typeof(ZLinkRemoteActorAdmissionAbortRequest));
                ZLinkSpotReplySubmitter.SubmitAndDispose(
                    received,
                    abortReplyParts);
                return true;
            }

            if (string.Equals(header.MessageName, ZLinkRemoteActorJoinPackets.HandoffCompletionPacketName, StringComparison.Ordinal))
            {
                var completionRequest = ZLinkRemoteActorJoinPackets.DecodeHandoffCompletionRequest(received.Parts);
                await runtime.CompleteRoutedActorHandoffAsync(
                        ZLinkSpotId.FromNativeRoutingId(nativeSpot.RoutingId),
                        completionRequest,
                        cancellationToken)
                    .ConfigureAwait(false);
                var completionReplyParts = ZLinkSpotReplyEnvelope.EncodeResponseParts(
                    channelName,
                    header.MessageName,
                    header.CorrelationId,
                    completionRequest,
                    typeof(ZLinkRemoteActorHandoffCompletionRequest));
                ZLinkSpotReplySubmitter.SubmitAndDispose(received, completionReplyParts);
                return true;
            }

            var joinRequest = ZLinkRemoteActorJoinPackets.DecodeJoinRequest(received.Parts);
            // The commit outlives a single RPC attempt: the joined callback
            // may hold longer than the request timeout, and the source's
            // deduped retry awaits the same preparation. Cancelling the
            // processing with the request would abort the in-flight join.
            var reply = await runtime.JoinRoutedActorAsync(
                ZLinkSpotId.FromNativeRoutingId(nativeSpot.RoutingId),
                joinRequest,
                runtime.ShutdownToken)
            .ConfigureAwait(false);
            var replyParts = ZLinkRemoteActorJoinPackets.EncodeJoinReplyEnvelope(
                channelName,
                header.MessageName,
                header.CorrelationId,
                reply);
            ZLinkSpotReplySubmitter.SubmitAndDispose(received, replyParts);

            return true;
        }
        catch (Exception ex)
        {
            ReplyInternalRouteError(received, header, ex);
            return true;
        }
    }

    private void ReplyInternalRouteError(
        ZLinkBackendRouteReceived received,
        ZLinkEnvelopeHeader header,
        Exception exception)
    {
        var replyParts = ZLinkSpotReplyEnvelope.EncodeErrorParts(
            channelName,
            header.MessageName,
            header.CorrelationId,
            exception);
        ZLinkSpotReplySubmitter.SubmitAndDispose(received, replyParts);
    }

    public async ValueTask DispatchSubscriptionsAsync(CancellationToken cancellationToken)
    {
        await subscriptions
            .DrainAsync(nativeSpot, runtime.Registration.Codecs, _dispatchErrors, _logger, InvokeSubscriptionAsync,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask DiscardSubscriptionsAsync(CancellationToken cancellationToken)
    {
        await subscriptions
            .DrainAsync(
                nativeSpot,
                runtime.Registration.Codecs,
                _dispatchErrors,
                _logger,
                static (_, _, _, _) => ValueTask.CompletedTask,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal static bool IsInfrastructureRoute(ZLinkBackendRouteReceived received)
    {
        if (received.Parts.Count == 0) return false;
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(received.Parts);
            return header.MessageName is ZLinkRemoteActorJoinPackets.RequestPacketName
                or ZLinkRemoteActorJoinPackets.AdmissionPacketName
                or ZLinkRemoteActorJoinPackets.AdmissionAbortPacketName
                or ZLinkRemoteActorJoinPackets.CommitPacketName
                or ZLinkRemoteActorJoinPackets.HandoffCompletionPacketName;
        }
        catch
        {
            return false;
        }
    }

    internal static void RejectApplicationRouteForDrain(
        ZLinkBackendRouteReceived received,
        string channelName,
        ZLinkAcceptedWorkAdmission admission,
        bool localTarget)
    {
        using (received)
        {
            if (!received.CanReply || received.Parts.Count == 0) return;
            try
            {
                var header = ZLinkEnvelopeCodec.DecodeHeader(received.Parts);
                var errorKind = admission switch
                {
                    ZLinkAcceptedWorkAdmission.QueueFull => localTarget
                        ? ZLinkFrameworkErrorKind.CapacityExceeded
                        : ZLinkFrameworkErrorKind.Unavailable,
                    ZLinkAcceptedWorkAdmission.Closed =>
                        ZLinkFrameworkErrorKind.ShuttingDown,
                    ZLinkAcceptedWorkAdmission.RelocationMoving =>
                        ZLinkFrameworkErrorKind.Unavailable,
                    _ => ZLinkFrameworkErrorKind.Rejected
                };
                var reply = ZLinkSpotReplyEnvelope.EncodeErrorParts(
                    channelName,
                    header.MessageName,
                    header.CorrelationId,
                    new ZLinkFrameworkException(
                        errorKind,
                        admission switch
                        {
                            ZLinkAcceptedWorkAdmission.QueueFull =>
                                "SPOT application admission is at capacity.",
                            ZLinkAcceptedWorkAdmission.Closed =>
                                "SPOT application admission is closed.",
                            ZLinkAcceptedWorkAdmission.RelocationMoving =>
                                "SPOT application admission is relocating.",
                            _ => "SPOT application admission was rejected."
                        }));
                ZLinkSpotReplySubmitter.SubmitAndDispose(received, reply);
            }
            catch
            {
            }
        }
    }

    internal static void RejectApplicationRouteForRelocation(
        ZLinkBackendRouteReceived received,
        string channelName)
    {
        using (received)
        {
            if (!received.CanReply || received.Parts.Count == 0) return;
            try
            {
                var header = ZLinkEnvelopeCodec.DecodeHeader(received.Parts);
                var reply = ZLinkSpotReplyEnvelope.EncodeErrorParts(
                    channelName,
                    header.MessageName,
                    header.CorrelationId,
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "SPOT relocation ingress hold is full or sealed.",
                        ZLinkRetryAdvice.RetryAfterBackoff));
                ZLinkSpotReplySubmitter.SubmitAndDispose(received, reply);
            }
            catch
            {
            }
        }
    }

    internal static void RejectApplicationRouteForStaleMessageFollow(
        ZLinkBackendRouteReceived received,
        string channelName)
    {
        using (received)
        {
            if (!received.CanReply || received.Parts.Count == 0) return;
            try
            {
                var header = ZLinkEnvelopeCodec.DecodeHeader(received.Parts);
                var reply = ZLinkSpotReplyEnvelope.EncodeErrorParts(
                    channelName,
                    header.MessageName,
                    header.CorrelationId,
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "The Spot Message Follow route is stale."));
                ZLinkSpotReplySubmitter.SubmitAndDispose(received, reply);
            }
            catch
            {
            }
        }
    }

    private async ValueTask InvokeSubscriptionAsync(
        ZLinkSpotSubscriptionDescriptor descriptor,
        object? message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        try
        {
            await handlerInvoker().InvokeSubscriptionAsync(
                    descriptor,
                    message,
                    context,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            var scope = new ZLinkDispatchFlowScope(
                ZLinkDispatchErrorSurface.SpotSubscription,
                "SpotSubscription",
                ZLinkDispatchMessageKind.Publish,
                "Publish",
                descriptor.MessageName,
                topic: descriptor.Topic);
            scope.HandlerException(
                _logger,
                _dispatchErrors,
                LogLevel.Error,
                ZLinkDispatchErrorAction.Drop,
                ex);
        }
    }
}
