using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.Runtime.Actors;

using System.Buffers.Binary;

internal static class ZLinkActorBoundSessionRelay
{
    internal const uint ActorRecvInfoNoBind = 1u;

    public static bool IsSessionDisconnectedPacket(ZlinkStreamHeader header)
    {
        return string.Equals(
            header.Name,
            ZLinkRemoteActorJoinPackets.SessionDisconnectedPacketName,
            StringComparison.Ordinal
        );
    }

    private const int DisconnectedGenerationBytes = sizeof(ulong) * 2;

    internal static bool MatchesRelaySource(
        ZLinkActorBoundSession session,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid
    )
    {
        return session.SessionNodeRid is { } sessionNodeRid
            && sessionNodeRid == sourceNodeRid
            && session.SessionRid == sourceSessionRid;
    }

    public static byte[] EncodeSessionDisconnected(
        RoutingId sessionOwnerNodeRid,
        ulong sessionOwnerNodeGeneration,
        ulong bindingGeneration
    )
    {
        if (
            sessionOwnerNodeRid.IsEmpty
            || bindingGeneration == 0
            || sessionOwnerNodeGeneration == 0
        )
            throw new InvalidOperationException(
                "Actor session disconnect requires an exact binding identity."
            );
        var rid = sessionOwnerNodeRid.ToBytes();
        var payload = new byte[sizeof(byte) + rid.Length + DisconnectedGenerationBytes];
        payload[0] = checked((byte)rid.Length);
        rid.CopyTo(payload.AsSpan(sizeof(byte)));
        var generationOffset = sizeof(byte) + rid.Length;
        BinaryPrimitives.WriteUInt64BigEndian(
            payload.AsSpan(generationOffset),
            sessionOwnerNodeGeneration
        );
        BinaryPrimitives.WriteUInt64BigEndian(
            payload.AsSpan(generationOffset + sizeof(ulong)),
            bindingGeneration
        );
        return payload;
    }

    public static bool TryValidateDisconnectedBinding(
        ZLinkActorRuntimeState state,
        Message payload,
        out string bindingToken,
        out bool malformed
    )
    {
        bindingToken = string.Empty;
        malformed = false;
        var bytes = payload.AsReadOnlyMemory();
        if (
            !TryDecodeSessionDisconnected(
                bytes.Span,
                out var sessionOwnerNodeRid,
                out var sessionOwnerNodeGeneration,
                out var bindingGeneration
            )
        )
        {
            malformed = true;
            return false;
        }
        // A bindingAbsent participant has no target binding to compare. Only a
        // different current binding fences its in-flight disconnect (§8.3).
        if (!state.TryGetBoundSessionForInbound(out var current))
            return true;
        if (
            (current.SessionNodeRid ?? state.NativeActorRef?.NodeRid) != sessionOwnerNodeRid
            || current.BindingGeneration != bindingGeneration
            || current.SessionOwnerNodeGeneration != sessionOwnerNodeGeneration
        )
            return false;
        bindingToken = current.BindingToken;
        return true;
    }

    internal static bool TryDecodeSessionDisconnected(
        ReadOnlySpan<byte> payload,
        out RoutingId sessionOwnerNodeRid,
        out ulong sessionOwnerNodeGeneration,
        out ulong bindingGeneration
    )
    {
        sessionOwnerNodeRid = default;
        bindingGeneration = 0;
        sessionOwnerNodeGeneration = 0;
        if (payload.Length < sizeof(byte) + 1 + DisconnectedGenerationBytes)
            return false;
        var ridLength = payload[0];
        if (
            ridLength == 0
            || payload.Length != sizeof(byte) + ridLength + DisconnectedGenerationBytes
        )
            return false;
        sessionOwnerNodeRid = RoutingId.From(payload.Slice(sizeof(byte), ridLength));
        var generationOffset = sizeof(byte) + ridLength;
        sessionOwnerNodeGeneration = BinaryPrimitives.ReadUInt64BigEndian(
            payload.Slice(generationOffset, sizeof(ulong))
        );
        bindingGeneration = BinaryPrimitives.ReadUInt64BigEndian(
            payload.Slice(generationOffset + sizeof(ulong), sizeof(ulong))
        );
        return bindingGeneration != 0 && sessionOwnerNodeGeneration != 0;
    }

    public static ZLinkActorBoundSessionDispatch EnterDispatch(
        ZLinkFrameworkRuntime runtime,
        string actorId,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags
    )
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
        Func<IReadOnlyList<Message>, SubmitResult>? directReply = null,
        CancellationToken cancellationToken = default
    )
    {
        if (
            requestHeader.Kind != ZlinkStreamMessageKind.Request
            || requestHeader.RequestSeq is null
            || !IsNoBindRequest(requestId, flags)
        )
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
                ZLinkActorReply.FromError(
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorRef.ActorId}' is not available."
                    )
                ),
                directReply,
                cancellationToken
            )
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
        Func<IReadOnlyList<Message>, SubmitResult>? directReply = null
    )
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_reply_begin actor={actorId} request_id={requestId} flags={flags} no_bind={isNoBind} capability={!string.IsNullOrWhiteSpace(replyCapability)} source_node={sourceNodeRid} direct={directReply is not null}"
        );
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
                    cancellationToken
                )
                .ConfigureAwait(false);
            return;
        }

        var frame = reply.ToFrame(requestHeader);
        await SendFrameAsync(runtime, actorId, sourceSessionRid, frame, cancellationToken)
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
        Func<IReadOnlyList<Message>, SubmitResult>? directReply = null
    )
    {
        if (
            requestHeader.Kind != ZlinkStreamMessageKind.Request
            || requestHeader.RequestSeq is null
        )
            return;

        var dispatch = EnterDispatch(
            runtime,
            actorRef.ActorId,
            sourceNodeRid,
            sourceSessionRid,
            requestId,
            flags
        );
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
                    directReply
                )
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
        Func<IReadOnlyList<Message>, SubmitResult>? directReply,
        CancellationToken cancellationToken
    )
    {
        var frame = reply.ToFrame(requestHeader);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_reply_no_bind_begin actor={actorRef.ActorId} request_id={requestId} source_node={sourceNodeRid} source_session={sourceSessionRid} capability={!string.IsNullOrWhiteSpace(replyCapability)} direct={directReply is not null}"
        );
        if (directReply is not null)
        {
            cancellationToken.ThrowIfCancellationRequested();
            using var replyMessage = Message.From(frame);
            var result = directReply([replyMessage]);
            if (result is not (SubmitResult.Ok or SubmitResult.Backpressured))
                throw new ZlinkSubmitException((ZlinkSubmitException.ErrorCode)(int)result);
        }
        else
        {
            using var replyMessage = Message.From(frame);
            await runtime
                .ReplyActorNoBindAsync(
                    actorRef,
                    sourceNodeRid,
                    sourceSessionRid,
                    requestId,
                    flags,
                    replyCapability,
                    [replyMessage]
                )
                .ConfigureAwait(false);
        }
        runtime.LogActorHandoff(
            $"request_reply_direct actor={actorRef.ActorId} request_id={requestId} caller_node={sourceNodeRid}"
        );
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_reply_no_bind_done actor={actorRef.ActorId} request_id={requestId}"
        );
    }

    private static async ValueTask SendFrameAsync(
        ZLinkFrameworkRuntime runtime,
        string actorId,
        RoutingId sourceSessionRid,
        byte[] frame,
        CancellationToken cancellationToken
    )
    {
        var sourceBindingToken = sourceSessionRid.IsEmpty
            ? runtime.TryGetActorBoundSessionForOutbound(actorId, out var current)
                ? current.BindingToken
                : throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{actorId}' has no current bound session for its reply.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                )
            : ZLinkActorBoundSessionBindingToken.Native(sourceSessionRid);
        using var terminal = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            runtime.ShutdownToken
        );
        using var frameMessage = Message.From(frame);
        try
        {
            await runtime
                .SendActorBoundSessionIfCurrentAsync(
                    actorId,
                    sourceBindingToken,
                    [frameMessage],
                    terminal.Token
                )
                .EnsureAcceptedAsync("Actor request reply relay")
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (runtime.ShutdownToken.IsCancellationRequested)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "Actor request reply relay was interrupted by runtime shutdown."
            );
        }
    }

    internal static bool IsNoBindRequest(ulong requestId, uint flags)
    {
        return requestId != 0 && (flags & ActorRecvInfoNoBind) != 0;
    }
}

internal readonly struct ZLinkActorBoundSessionDispatch(
    bool isNoBind,
    ZLinkBoundSessionDispatchScope scope
) : IAsyncDisposable
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
