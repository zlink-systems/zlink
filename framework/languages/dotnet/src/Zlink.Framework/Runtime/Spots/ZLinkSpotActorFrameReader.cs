using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorFrame(
    ZLinkBackendActorRef actor,
    ZLinkBackendActorRef replyActor,
    RoutingId sourceNodeRid,
    RoutingId sourceSessionRid,
    ulong requestId,
    uint flags,
    ZLinkBackendActorRouteContext routeContext,
    ZlinkStreamHeader header,
    Message body,
    ulong sourceNodeGeneration = 0,
    ZLinkServiceWireCodec.RequestSourceFence? requestSource = null,
    Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? directReply = null,
    ReadOnlyMemory<byte> applicationMetadata = default,
    long? handoffArrivalIndex = null) : IDisposable
{
    private Message? _body = body;

    public ZLinkBackendActorRef Actor { get; } = actor;

    public ZLinkBackendActorRef ReplyActor { get; } = replyActor;

    public RoutingId SourceNodeRid { get; } = sourceNodeRid;

    public RoutingId SourceSessionRid { get; } = sourceSessionRid;

    public ulong RequestId { get; } = requestId;

    public uint Flags { get; } = flags;

    public ZLinkBackendActorRouteContext RouteContext { get; private set; } = routeContext;

    public ulong SourceNodeGeneration { get; } = sourceNodeGeneration;

    public ZLinkServiceWireCodec.RequestSourceFence? RequestSource { get; } =
        requestSource;

    public Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? DirectReply { get; private set; } =
        directReply;

    public ReadOnlyMemory<byte> ApplicationMetadata { get; } =
        applicationMetadata;

    // A restored handoff frame must acknowledge the exact journal entry that
    // produced it. Live ingress frames do not have this identity.
    internal long? HandoffArrivalIndex { get; } = handoffArrivalIndex;

    public ulong RelocationReplyRouteId { get; private set; }

    internal void BindRelocationReplyRoute(ulong replyRouteId)
    {
        if (replyRouteId == 0 || RelocationReplyRouteId != 0)
            throw new InvalidOperationException(
                "The Actor frame relocation reply route is invalid.");
        RelocationReplyRouteId = replyRouteId;
        RouteContext = RouteContext with { ReplyRequestId = replyRouteId };
    }

    internal void BindRelocationReplyCapability(
        string replyCapability,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult> directReply)
    {
        if (string.IsNullOrWhiteSpace(replyCapability)
            || RouteContext.ReplyCapability is not null)
            throw new InvalidOperationException(
                "The Actor frame relocation reply capability is invalid.");
        RouteContext = RouteContext with { ReplyCapability = replyCapability };
        DirectReply = directReply;
    }

    public ZlinkStreamHeader Header { get; } = header;

    public Message Body => _body
                           ?? throw new ObjectDisposedException(nameof(ZLinkSpotActorFrame));

    public void Dispose()
    {
        Interlocked.Exchange(ref _body, null)?.Dispose();
    }
}

internal sealed class ZLinkSpotActorFrameBatch(
    IReadOnlyList<ZLinkSpotActorFrame> frames,
    Action? completion = null,
    ZLinkInboundDispatchLease? inboundDispatchLease = null) : IDisposable
{
    private int _disposed;

    public int Count => frames.Count;

    public ZLinkSpotActorFrame this[int index] => frames[index];

    public ZLinkSpotActorFrameBatch WithCompletion(Action onCompleted) =>
        new(frames, onCompleted, inboundDispatchLease);

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;

        try
        {
            foreach (var frame in frames) frame.Dispose();
        }
        finally
        {
            try
            {
                completion?.Invoke();
            }
            finally
            {
                inboundDispatchLease?.Dispose();
            }
        }
    }
}

internal static class ZLinkSpotActorFrameReader
{
    public static bool TryRead(
        IReadOnlyList<ZLinkBackendActorPart> parts,
        ref int index,
        ZLinkBackendActorPart headerPart,
        out ZLinkSpotActorFrame frame)
    {
        ZlinkStreamHeader header;
        try
        {
            header = ZLinkStreamProtocolDefaults.DecodeHeader(headerPart.Message.AsReadOnlyMemory());
        }
        catch
        {
            DisposeContinuationParts(parts, ref index, headerPart.More);
            frame = null!;
            return false;
        }
        finally
        {
            headerPart.Message.Dispose();
        }

        var body = TakeBodyPart(parts, ref index, headerPart.More);
        if (body is null)
        {
            frame = null!;
            return false;
        }

        frame = new ZLinkSpotActorFrame(
            headerPart.Actor,
            headerPart.ReplyActor ?? headerPart.Actor,
            headerPart.SourceNodeRid,
            headerPart.SourceSessionRid,
            headerPart.RequestId,
            headerPart.Flags,
            headerPart.RouteContext,
            header,
            body,
            headerPart.SourceNodeGeneration,
            headerPart.RequestSource,
            headerPart.DirectReply,
            headerPart.ApplicationMetadata);
        return true;
    }

    private static Message? TakeBodyPart(
        IReadOnlyList<ZLinkBackendActorPart> parts,
        ref int index,
        bool hasBody)
    {
        if (!hasBody) return Message.From(ReadOnlySpan<byte>.Empty);

        if (index >= parts.Count) return null;

        return parts[index++].Message;
    }

    private static void DisposeContinuationParts(
        IReadOnlyList<ZLinkBackendActorPart> parts,
        ref int index,
        bool hasMore)
    {
        while (hasMore && index < parts.Count)
        {
            var part = parts[index++];
            hasMore = part.More;
            part.Message.Dispose();
        }
    }
}
