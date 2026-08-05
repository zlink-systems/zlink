using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.Runtime.Actors;

using System.Text;
using System.Buffers.Binary;

internal static class ZLinkActorBoundSessionRelay
{
    internal const uint ActorRecvInfoNoBind = 1u;

    public static bool IsSessionDisconnectedPacket(ZlinkStreamHeader header)
    {
        return string.Equals(
            header.Name,
            ZLinkRemoteActorJoinPackets.SessionDisconnectedPacketName,
            StringComparison.Ordinal);
    }

    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static bool MatchesRelaySource(
        ZLinkActorBoundSession session,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid)
    {
        return session.SessionNodeRid is { } sessionNodeRid
               && sessionNodeRid == sourceNodeRid
               && session.SessionRid == sourceSessionRid;
    }

    public static byte[] EncodeSessionDisconnected(
        string bindingToken,
        ulong bindingGeneration,
        ulong sessionOwnerNodeGeneration)
    {
        if (string.IsNullOrWhiteSpace(bindingToken)
            || bindingGeneration == 0
            || sessionOwnerNodeGeneration == 0)
            throw new InvalidOperationException(
                "Actor session disconnect requires an exact binding identity.");
        var token = StrictUtf8.GetBytes(bindingToken);
        var payload = new byte[sizeof(uint) + token.Length + (sizeof(ulong) * 2)];
        BinaryPrimitives.WriteUInt32BigEndian(payload, checked((uint)token.Length));
        token.CopyTo(payload.AsSpan(sizeof(uint)));
        var generationOffset = sizeof(uint) + token.Length;
        BinaryPrimitives.WriteUInt64BigEndian(
            payload.AsSpan(generationOffset),
            bindingGeneration);
        BinaryPrimitives.WriteUInt64BigEndian(
            payload.AsSpan(generationOffset + sizeof(ulong)),
            sessionOwnerNodeGeneration);
        return payload;
    }

    public static bool TryValidateDisconnectedBinding(
        ZLinkActorRuntimeState state,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Message payload,
        out string bindingToken)
    {
        bindingToken = string.Empty;
        var bytes = payload.AsReadOnlyMemory();
        if (!TryDecodeSessionDisconnected(
                bytes.Span,
                out var decodedBindingToken,
                out var bindingGeneration,
                out var sessionOwnerNodeGeneration))
            return false;
        // Final relocation replay runs before the session-owner route commit.
        // The target therefore exposes the exact committed target projection
        // through the inbound view even though _boundSession is not promoted
        // until the route update is acknowledged.
        if (!state.TryGetBoundSessionForInbound(out var current)
            || !string.Equals(
                current.BindingToken,
                decodedBindingToken,
                StringComparison.Ordinal)
            || current.SessionNodeRid is not { } currentNodeRid
            || currentNodeRid != sourceNodeRid
            || current.SessionRid != sourceSessionRid
            || current.BindingGeneration != bindingGeneration
            || current.SessionOwnerNodeGeneration != sessionOwnerNodeGeneration)
        {
            //  Dropping here leaves the Actor holding a binding whose session
            //  is gone; the caller traces it (spec 26 §2.1).
            return false;
        }

        bindingToken = decodedBindingToken;
        return true;
    }

    private static bool TryDecodeSessionDisconnected(
        ReadOnlySpan<byte> payload,
        out string bindingToken,
        out ulong bindingGeneration,
        out ulong sessionOwnerNodeGeneration)
    {
        bindingToken = string.Empty;
        bindingGeneration = 0;
        sessionOwnerNodeGeneration = 0;
        if (payload.Length < sizeof(uint) + (sizeof(ulong) * 2)) return false;
        var tokenLength = BinaryPrimitives.ReadUInt32BigEndian(payload);
        if (tokenLength == 0
            || tokenLength > int.MaxValue
            || payload.Length != sizeof(uint) + (int)tokenLength + (sizeof(ulong) * 2))
            return false;
        try
        {
            bindingToken = StrictUtf8.GetString(
                payload.Slice(sizeof(uint), (int)tokenLength));
        }
        catch (DecoderFallbackException)
        {
            return false;
        }
        var generationOffset = sizeof(uint) + (int)tokenLength;
        bindingGeneration = BinaryPrimitives.ReadUInt64BigEndian(
            payload.Slice(generationOffset, sizeof(ulong)));
        sessionOwnerNodeGeneration = BinaryPrimitives.ReadUInt64BigEndian(
            payload.Slice(generationOffset + sizeof(ulong), sizeof(ulong)));
        return bindingGeneration != 0 && sessionOwnerNodeGeneration != 0;
    }

    public static ZLinkActorBoundSessionDispatch EnterDispatch(
        ZLinkFrameworkRuntime runtime,
        string actorId,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags)
    {
        var isNoBind = IsNoBindRequest(requestId, flags);
        var scope = ZLinkBoundSessionDispatchScope.Enter(actorId);
        // The bind command owns the exact Mesh, NodeGeneration and owner-lease
        // fences. A data frame may use that route but cannot recreate it from
        // transport coordinates, because those coordinates do not carry the
        // authority identity needed for a safe replacement.

        return new ZLinkActorBoundSessionDispatch(isNoBind, scope);
    }

    public static async ValueTask<bool> TryReplyMissingNoBindActorAsync(
        ZLinkFrameworkRuntime runtime,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        string? replyCapability,
        ZlinkStreamHeader requestHeader,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? directReply = null,
        CancellationToken cancellationToken = default)
    {
        if (requestHeader.Kind != ZlinkStreamMessageKind.Request
            || requestHeader.RequestSeq is null
            || !IsNoBindRequest(requestId, flags))
            return false;

        await ReplyNoBindAsync(
                runtime,
                actorRef,
                sourceNodeRid,
                sourceSessionRid,
                requestId,
                flags,
                replyCapability,
                requestHeader,
                ZLinkActorReply.FromError(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    $"Actor '{actorRef.ActorId}' is not available.")),
                directReply,
                cancellationToken)
            .ConfigureAwait(false);
        return true;
    }

    public static async ValueTask SendReplyAsync(
        ZLinkFrameworkRuntime runtime,
        string actorId,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        string? replyCapability,
        bool isNoBind,
        ZlinkStreamHeader requestHeader,
        ZLinkActorReply reply,
        CancellationToken cancellationToken,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? directReply = null)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_reply_begin actor={actorId} request_id={requestId} "
            + $"flags={flags} no_bind={isNoBind} capability={(!string.IsNullOrWhiteSpace(replyCapability))} "
            + $"source_node={sourceNodeRid} direct={directReply is not null}");
        if (isNoBind)
        {
            await ReplyNoBindAsync(
                    runtime,
                    actorRef,
                    sourceNodeRid,
                    sourceSessionRid,
                    requestId,
                    flags,
                    replyCapability,
                    requestHeader,
                    reply,
                    directReply,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        var frame = reply.ToFrame(requestHeader);
        await SendFrameWithRetryAsync(runtime, actorId, sourceSessionRid, frame, cancellationToken)
            .ConfigureAwait(false);
    }

    public static async ValueTask ReplyStaleActorAsync(
        ZLinkFrameworkRuntime runtime,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        string? replyCapability,
        ZlinkStreamHeader requestHeader,
        ZLinkFrameworkException exception,
        CancellationToken cancellationToken,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? directReply = null)
    {
        if (requestHeader.Kind != ZlinkStreamMessageKind.Request
            || requestHeader.RequestSeq is null)
            return;

        var dispatch = EnterDispatch(
            runtime,
            actorRef.ActorId,
            sourceNodeRid,
            sourceSessionRid,
            requestId,
            flags);
        try
        {
            await SendReplyAsync(
                    runtime,
                    actorRef.ActorId,
                    actorRef,
                    sourceNodeRid,
                    sourceSessionRid,
                    requestId,
                    flags,
                    replyCapability,
                    dispatch.IsNoBind,
                    requestHeader,
                    ZLinkActorReply.FromError(exception),
                    cancellationToken,
                    directReply)
                .ConfigureAwait(false);
            await dispatch.DrainAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            await dispatch.DisposeAsync().ConfigureAwait(false);
        }
    }

    private static async ValueTask ReplyNoBindAsync(
        ZLinkFrameworkRuntime runtime,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        string? replyCapability,
        ZlinkStreamHeader requestHeader,
        ZLinkActorReply reply,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? directReply,
        CancellationToken cancellationToken)
    {
        var frame = reply.ToFrame(requestHeader);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_reply_no_bind_begin actor={actorRef.ActorId} request_id={requestId} "
            + $"source_node={sourceNodeRid} source_session={sourceSessionRid} "
            + $"capability={(!string.IsNullOrWhiteSpace(replyCapability))} "
            + $"direct={directReply is not null}");
        if (directReply is not null)
            await ZLinkRetryingSubmitter.Async(
                    () =>
                    {
                        using var replyMessage = Message.From(frame);
                        return directReply([replyMessage], SendFlags.DontWait);
                    },
                    runtime.Registration.DefaultRequestTimeout,
                    "Direct actor request terminal reply failed.",
                    cancellationToken)
                .ConfigureAwait(false);
        else
        {
            using var replyMessage = Message.From(frame);
            await runtime.ReplyActorNoBindAsync(
                    actorRef,
                    sourceNodeRid,
                    sourceSessionRid,
                    requestId,
                    flags,
                    replyCapability,
                    [replyMessage])
                .ConfigureAwait(false);
        }
        runtime.LogActorHandoff(
            $"request_reply_direct actor={actorRef.ActorId} request_id={requestId} caller_node={sourceNodeRid}");
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_reply_no_bind_done actor={actorRef.ActorId} request_id={requestId}");
    }

    private static async ValueTask SendFrameWithRetryAsync(
        ZLinkFrameworkRuntime runtime,
        string actorId,
        RoutingId sourceSessionRid,
        byte[] frame,
        CancellationToken cancellationToken)
    {
        // An identity-less frame (no session rid) replies on the actor's
        // current binding: it cannot name a binding token, and gating it on
        // the empty native token would never match a session bound through
        // the coordinator's own registry.
        var sourceBindingToken = ZLinkActorBoundSessionBindingToken.Native(sourceSessionRid);
        await ZLinkRetryingSubmitter.Async(
                () =>
                {
                    using var frameMessage = Message.From(frame);
                    return sourceSessionRid.IsEmpty
                        ? runtime.SendActorBoundSession(
                            actorId,
                            new[] { frameMessage },
                            SendFlags.None)
                        : runtime.SendActorBoundSessionIfCurrent(
                            actorId,
                            sourceBindingToken,
                            new[] { frameMessage },
                            SendFlags.None);
                },
                runtime.Registration.DefaultRequestTimeout,
                "Actor request reply relay failed.",
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal static bool IsNoBindRequest(ulong requestId, uint flags)
    {
        return requestId != 0 && (flags & ActorRecvInfoNoBind) != 0;
    }

}

internal readonly struct ZLinkActorBoundSessionDispatch(
    bool isNoBind,
    ZLinkBoundSessionDispatchScope scope) : IAsyncDisposable
{
    public bool IsNoBind => isNoBind;

    public ValueTask DrainAsync(CancellationToken cancellationToken)
    {
        return scope.DrainAsync(cancellationToken);
    }

    public ValueTask DisposeAsync()
    {
        return scope.DisposeAsync();
    }
}
