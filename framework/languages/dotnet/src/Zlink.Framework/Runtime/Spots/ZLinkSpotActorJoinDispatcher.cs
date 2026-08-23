using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Identifiers;

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
    private readonly ZLinkDispatchErrorReporter _dispatchErrors = dispatchErrors ?? new(
        runtime.Registration.DispatchOptions,
        logger ?? NullLogger<ZLinkSpotActorJoinDispatcher>.Instance,
        runtime);

    public async ValueTask DispatchAsync(
        ZLinkBackendActorJoinRequest joinRequest,
        CancellationToken cancellationToken)
    {
        using var applicationAdmission =
            joinRequest.ApplicationJobAdmission is { } admission
                ? ZLinkApplicationJobQueueInvocation.Enter(admission)
                : null;
        var payload = DecodeJoinPayload(joinRequest);
        using var currentFlow = ZLinkFlowContext.Enter(
            payload.FlowId,
            payload.FlowOrigin,
            runtime.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound);
        if (joinRequest.Canonical is { } canonical)
        {
            try
            {
                var canonicalAdmission = await runtime.AdmitCanonicalActorJoinAsync(
                        joinRequest.TargetSpotId,
                        canonical.Request.Request,
                        canonical.Payload,
                        cancellationToken)
                    .ConfigureAwait(false);
                ReplyCanonicalAdmission(joinRequest, canonicalAdmission);
            }
            catch (Exception ex)
            {
                ReplyCanonicalTerminal(joinRequest, ex);
            }
            return;
        }
        var hasHandler = actorJoins.TryResolve(out var descriptor)
                         && descriptor is not null;
        if (!hasHandler && !acceptActorJoinWithoutHandler)
        {
            ReplyRejected(joinRequest, payload.MessageName, "no-join-handler");
            return;
        }

        if (!actors.TryGetActor(
                ZLinkActorId.FromBoundary(
                    joinRequest.TargetActor.ActorId,
                    nameof(joinRequest)),
                out var actor)
            || actor is null)
            actor = runtime.GetOrCreateActorState(joinRequest.TargetActor.ActorId).Actor;

        if (actor is null)
        {
            ReplyRejected(joinRequest, payload.MessageName, "no-target-actor");
            return;
        }

        if (payload.Error is { } payloadError)
        {
            ReplyRejected(
                joinRequest,
                payload.MessageName,
                "payload-decode-failed",
                payloadError);
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
                ex);
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
                    ex);
                return;
            }
        }

        if (joinRequest.Canonical is not null)
        {
            if (result.Reply is null)
            {
                nativeSpot.ReplyActorJoin(
                    joinRequest,
                    result.Accepted ? 0 : 1,
                    Array.Empty<Message>());
                return;
            }

            var encoded = result.Reply.Encode(runtime.Registration.Codecs);
            var application = ZLinkApplicationPayloadEnvelopeCodec.Encode(
                ZLinkMessageNameResolver.ResolveFromMessage(result.Reply),
                encoded.ContentType,
                encoded.Payload.Bytes.Span);
            using var reply = Message.From(application);
            nativeSpot.ReplyActorJoin(
                joinRequest,
                result.Accepted ? 0 : 1,
                reply);
            return;
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
        if (joinRequest.Canonical is { } canonical)
        {
            if (canonical.Payload is not { } application)
                return new JoinPayload(
                    false,
                    typeof(ZLinkMessage).Name,
                    ZLinkMessage.Empty,
                    null,
                    null,
                    null);
            try
            {
                using var request = Message.From(application.Payload);
                return new JoinPayload(
                    false,
                    application.PacketName,
                    ZLinkMessage.FromEnvelopePayload(
                        application.ContentType,
                        request,
                        runtime.Registration.Codecs),
                    null,
                    null,
                    null);
            }
            catch (Exception ex)
            {
                return new JoinPayload(
                    false,
                    application.PacketName,
                    ZLinkMessage.Empty,
                    ex,
                    null,
                    null);
            }
        }
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
            header = ZLinkEnvelopeCodec.DecodeHeader(
                joinRequest.Parts,
                runtime.Flow.CaptureEnabled);
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
        Exception? exception = null)
    {
        var errorReason = reason switch
        {
            "payload-decode-failed" => ZLinkDispatchErrorReason.PayloadDecodeFailed,
            "handler-exception" or "join-commit-failed" => ZLinkDispatchErrorReason.HandlerException,
            _ => ZLinkDispatchErrorReason.HandlerMissing
        };
        if (_dispatchErrors.Enabled)
            _dispatchErrors.Report(new ZLinkDispatchFailure(
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

    private void ReplyCanonicalAdmission(
        ZLinkBackendActorJoinRequest joinRequest,
        ZLinkRemoteActorAdmissionReply admission)
    {
        if (admission.Reply.Length == 0)
        {
            nativeSpot.ReplyActorJoin(
                joinRequest,
                admission.Accepted ? 0 : 1,
                Array.Empty<Message>());
            return;
        }

        // service-wire-v1 ActorJoin(28): spec 51 fixes the reply framing as
        // multipart wrap + sole raw part — the part preserves the handler's
        // packet name/content type only insofar as the transport carries no
        // per-part metadata channel; it must never be reinterpreted as a
        // nested application envelope (see spec 51's Framework multipart
        // application profile note on actorJoin(28)).
        using var reply = Message.From(admission.Reply);
        nativeSpot.ReplyActorJoin(
            joinRequest,
            admission.Accepted ? 0 : 1,
            reply);
    }

    private static void ReplyCanonicalTerminal(
        ZLinkBackendActorJoinRequest joinRequest,
        Exception exception)
    {
        if (joinRequest is not ZLinkMeshActorJoinRequest meshRequest)
            return;
        var terminal = exception is ZLinkFrameworkException framework
            ? framework.Kind switch
            {
                ZLinkFrameworkErrorKind.TypeMismatch => (
                    RequestResult.Conflict,
                    (uint)ServiceWireConstants.FrameworkErrorCode.ActorTypeMismatch),
                ZLinkFrameworkErrorKind.CapacityExceeded => (
                    RequestResult.Backpressured,
                    (uint)ServiceWireConstants.FrameworkErrorCode.None),
                ZLinkFrameworkErrorKind.ProtocolError => (
                    RequestResult.ProtocolError,
                    (uint)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError),
                ZLinkFrameworkErrorKind.InvalidOperation => (
                    RequestResult.Conflict,
                    (uint)ServiceWireConstants.FrameworkErrorCode.ActorLocationStale),
                ZLinkFrameworkErrorKind.NotFound => (
                    RequestResult.NotFound,
                    (uint)ServiceWireConstants.FrameworkErrorCode.ActorRouteNotFound),
                ZLinkFrameworkErrorKind.Unavailable => (
                    RequestResult.InternalError,
                    (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed),
                ZLinkFrameworkErrorKind.Rejected => (
                    RequestResult.Rejected,
                    (uint)ServiceWireConstants.FrameworkErrorCode.RequestRejected),
                _ => (
                    RequestResult.InternalError,
                    (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed)
            }
            : (
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed);
        meshRequest.ReplyTerminal(terminal.Item1, terminal.Item2);
    }

    private readonly record struct JoinPayload(
        bool UsesEnvelope,
        string MessageName,
        ZLinkMessage Request,
        Exception? Error,
        string? FlowId,
        ZLinkFlowOrigin? FlowOrigin);
}
