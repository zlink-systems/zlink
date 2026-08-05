using System.Text.Json;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkRemoteSessionBindingHandler
{
    public static async ValueTask<bool> TryHandleAsync(
        ZLinkFrameworkRuntime runtime,
        IZLinkActor actor,
        ZLinkActorRuntimeState state,
        ZLinkSpotActorFrame frame,
        ZLinkActorBoundSessionDispatch boundSession,
        Action? acknowledgeHandledFrame,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(
                frame.Header.Name,
                ZLinkRemoteSessionBindingProtocol.PacketName,
                StringComparison.Ordinal)) return false;

        if (frame.Header.Kind != ZlinkStreamMessageKind.Request
            || frame.Header.RequestSeq is null
            || state.NativeActorRef is not { } nativeActorRef)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Remote actor session binding requires a request and a concrete actor ref.");

        var request = JsonSerializer.Deserialize<ZLinkRemoteSessionBindRequest>(
                          frame.Body.AsReadOnlySpan(),
                          ZLinkJsonSerializerOptions.Default)
                      ?? throw new ZLinkFrameworkException(
                          ZLinkFrameworkErrorKind.ProtocolError,
                          "Remote actor session binding payload was empty.");
        var sessionNodeRid = RoutingId.From(request.SessionNodeRid);
        if (frame.SourceNodeRid != sessionNodeRid)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Remote actor session binding source did not match the declared session node.");

        if (!string.Equals(request.ActorId, actor.Context.ActorId, StringComparison.Ordinal)
            || RoutingId.From(request.TargetNodeRid) != nativeActorRef.NodeRid
            || request.ObjectGeneration != nativeActorRef.Generation)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Remote actor session binding request did not match the addressed Actor.");
        ZLinkRemoteSessionBindResponse response;
        response = await runtime.BindRemoteBoundSessionRouteAsync(
                request,
                frame.SourceNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
        acknowledgeHandledFrame?.Invoke();
        await ZLinkActorBoundSessionRelay.SendReplyAsync(
                runtime,
                actor.Context.ActorId,
                frame.Actor,
                frame.SourceNodeRid,
                frame.SourceSessionRid,
                frame.RequestId,
                frame.Flags,
                frame.RouteContext.ReplyCapability,
                boundSession.IsNoBind,
                frame.Header,
                AcknowledgedReply(response),
                cancellationToken,
                frame.DirectReply)
            .ConfigureAwait(false);
        await boundSession.DrainAsync(cancellationToken).ConfigureAwait(false);
        return true;
    }

    private static ZLinkActorReply AcknowledgedReply(
        ZLinkRemoteSessionBindResponse response) => new(
        ZlinkStreamMessageKind.Response,
        ZlinkStreamCodec.Json,
        ZLinkEnvelopeCodec.EncodeJsonBytes(response),
        ZlinkStreamHeaderFlags.None,
        ZlinkStreamMetadata.Empty);
}
