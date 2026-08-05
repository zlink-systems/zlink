using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorJoinDispatcher(
    ZLinkFrameworkRuntime runtime,
    IZLinkBackendSpot nativeSpot,
    string channelName,
    ZLinkSpotActorJoinRegistry actorJoins,
    ZLinkSpotActorMembership actors,
    Func<ZLinkSpotHandlerInvoker> handlerInvoker,
    ILogger<ZLinkSpotActorJoinDispatcher>? logger = null,
    Func<IZLinkActor, CancellationToken, ValueTask>? commitAcceptedActorJoin = null,
    ZLinkDispatchErrorReporter? dispatchErrors = null,
    bool acceptActorJoinWithoutHandler = false)
{
    private readonly ILogger<ZLinkSpotActorJoinDispatcher> _logger =
        logger ?? NullLogger<ZLinkSpotActorJoinDispatcher>.Instance;

    public async ValueTask DispatchAsync(
        ZLinkBackendActorJoinRequest joinRequest,
        CancellationToken cancellationToken)
    {
        var payload = DecodeJoinPayload(joinRequest);
        using var currentFlow = ZLinkFlowContext.Enter(
            payload.FlowId,
            payload.FlowOrigin,
            runtime.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound);
        var hasHandler = actorJoins.TryResolve(out var descriptor)
                         && descriptor is not null;
        if (!hasHandler && !acceptActorJoinWithoutHandler)
        {
            ReplyRejected(joinRequest, payload.MessageName, "no-join-handler", LogLevel.Debug);
            return;
        }

        if (!actors.TryGetActor(joinRequest.TargetActor.ActorId, out var actor) || actor is null)
            actor = runtime.GetOrCreateActorState(joinRequest.TargetActor.ActorId).Actor;

        if (actor is null)
        {
            ReplyRejected(joinRequest, payload.MessageName, "no-target-actor", LogLevel.Debug);
            return;
        }

        if (payload.Error is { } payloadError)
        {
            ReplyRejected(
                joinRequest,
                payload.MessageName,
                "payload-decode-failed",
                LogLevel.Warning,
                payloadError,
                descriptor?.ActorType);
            return;
        }

        ZLinkSpotActorJoinResult result;
        try
        {
            result = hasHandler
                ? await handlerInvoker()
                    .InvokeActorJoinAsync(
                        descriptor!,
                        actor.Context.ActorId,
                        payload.Request,
                        cancellationToken)
                    .ConfigureAwait(false)
                : ZLinkSpotActorJoinResult.Accept();
        }
        catch (Exception ex)
        {
            ReplyRejected(
                joinRequest,
                payload.MessageName,
                "handler-exception",
                LogLevel.Warning,
                ex,
                descriptor?.ActorType);
            return;
        }

        if (result.Accepted && commitAcceptedActorJoin is not null)
        {
            try
            {
                await commitAcceptedActorJoin(actor, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                ReplyRejected(
                    joinRequest,
                    payload.MessageName,
                    "join-commit-failed",
                    LogLevel.Warning,
                    ex,
                    descriptor?.ActorType);
                return;
            }
        }

        if (!payload.UsesEnvelope)
        {
            if (result.Reply is { } reply)
            {
                var encodedReply = reply.Encode(runtime.Registration.Codecs);
                using var replyMessage = Message.From(encodedReply.Payload.Bytes.Span);
                nativeSpot.ReplyActorJoin(
                    joinRequest,
                    result.Accepted ? 0 : 1,
                    replyMessage);
                return;
            }

            using var emptyReply = Message.From(ReadOnlySpan<byte>.Empty);
            nativeSpot.ReplyActorJoin(
                joinRequest,
                result.Accepted ? 0 : 1,
                emptyReply);
            return;
        }

        var replyParts = ZLinkSpotReplyEnvelope.EncodeActorJoinReplyParts(
            channelName,
            payload.MessageName,
            result.Reply,
            typeof(ZLinkMessage),
            runtime.Registration.Codecs);
        try
        {
            nativeSpot.ReplyActorJoin(
                joinRequest,
                result.Accepted ? 0 : 1,
                replyParts);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    private JoinPayload DecodeJoinPayload(ZLinkBackendActorJoinRequest joinRequest)
    {
        if (joinRequest.Parts.Count == 1)
        {
            try
            {
                var envelope = ZLinkEnvelopeCodec.DecodePart<ZLinkActorJoinSinglePartEnvelope>(joinRequest.Parts[0]);
                using var request = Message.From(envelope.Payload);
                return new JoinPayload(
                    true,
                    typeof(ZLinkMessage).Name,
                    ZLinkMessage.FromEnvelopePayload(
                        envelope.ContentType,
                        request,
                        runtime.Registration.Codecs),
                    null,
                    null,
                    null);
            }
            catch
            {
            }

            return new JoinPayload(
                false,
                typeof(Message).Name,
                ZLinkMessage.FromEnvelopePayload(
                    ZLinkEnvelopeCodec.DefaultContentType,
                    joinRequest.Parts[0],
                    runtime.Registration.Codecs),
                null,
                null,
                null);
        }

        ZLinkEnvelopeHeader? header = null;
        try
        {
            header = ZLinkEnvelopeCodec.DecodeHeader(joinRequest.Parts);
            if (joinRequest.Parts.Count <= 1)
                throw new InvalidOperationException("Actor join request body part is missing.");

            return new JoinPayload(
                true,
                header.MessageName,
                ZLinkMessage.FromEnvelopePayload(
                    header.ContentType,
                    joinRequest.Parts[1],
                    runtime.Registration.Codecs),
                null,
                header.FlowId,
                header.FlowOrigin);
        }
        catch (Exception ex)
        {
            var validFlow = header is null
                ? (FlowId: (string?)null, FlowOrigin: (ZLinkFlowOrigin?)null)
                : ZLinkEnvelopeCodec.ValidFlow(header);
            return new JoinPayload(
                true,
                typeof(Message).Name,
                ZLinkMessage.Empty,
                ex,
                validFlow.FlowId,
                validFlow.FlowOrigin);
        }
    }

    private void ReplyRejected(
        ZLinkBackendActorJoinRequest joinRequest,
        string messageName,
        string reason,
        LogLevel level,
        Exception? exception = null,
        Type? actorType = null)
    {
        var errorReason = reason switch
        {
            "payload-decode-failed" => ZLinkDispatchErrorReason.PayloadDecodeFailed,
            "handler-exception" or "join-commit-failed" => ZLinkDispatchErrorReason.HandlerException,
            _ => ZLinkDispatchErrorReason.HandlerMissing
        };
        if (dispatchErrors is null
            || dispatchErrors.Flow.Enabled(ZLinkMessageFlowOutcome.Error))
            ZLinkMessageFlowLogger.Rejected(
                _logger,
                level,
                new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.Error,
                    ZLinkDispatchErrorSurface.SpotActor,
                    ZLinkDispatchMessageKind.Request,
                    messageName,
                    channelName,
                    ActorId: joinRequest.TargetActor.ActorId,
                    SpotId: joinRequest.TargetSpotId),
                reason,
                "EntrySpot",
                "Request",
                exception,
                actorType?.Name,
                writeLog: dispatchErrors is null);
        dispatchErrors?.Report(new ZLinkDispatchFailure(
            ZLinkDispatchErrorSurface.SpotActor,
            ZLinkDispatchMessageKind.Request,
            errorReason,
            ZLinkDispatchErrorAction.ReplyError,
            messageName,
            channelName,
            SpotId: joinRequest.TargetSpotId,
            ActorId: joinRequest.TargetActor.ActorId,
            Exception: exception));
        using var emptyReply = Message.From(ReadOnlySpan<byte>.Empty);
        nativeSpot.ReplyActorJoin(joinRequest, 1, emptyReply);
    }

    private readonly record struct JoinPayload(
        bool UsesEnvelope,
        string MessageName,
        ZLinkMessage Request,
        Exception? Error,
        string? FlowId,
        ZLinkFlowOrigin? FlowOrigin);
}
