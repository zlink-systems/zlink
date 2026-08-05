using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Backend.Contracts;

// Framework-owned dispatch records (RouteMesh 10.0.0 Option B).
//
// In 9.x the framework drove the route/subscribe/reply plane with the bindings'
// Received/TopicMessage raw-socket receive types. 10.0.0 delivers pull dispatch
// via MeshReceiveRecord and does not expose a way to populate a Received from a
// record. Per the framework spec the framework itself drains claims to typed
// handlers and holds the reply token, route envelope and Core claim. These
// framework-owned records carry the retained message parts and a reply callback
// bound to the record's held reply token, so consumers reply without the binding
// Received type.

/// <summary>
///     A framework-owned inbound route/spot/channel message. Mirrors the subset
///     of the binding <c>Received</c> surface the route dispatch plane consumes
///     (parts, spot rid, request seq) and carries a reply delegate bound to the
///     originating pull-dispatch reply token.
/// </summary>
internal sealed class ZLinkBackendRouteReceived : IDisposable
{
    private readonly Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? _reply;
    private bool _disposed;

    public ZLinkBackendRouteReceived(
        IReadOnlyList<Message> parts,
        RoutingId? sourceNodeRid,
        string? spotId,
        ulong? requestSeq,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? reply,
        string? channelName = null,
        ZLinkMessageMetadata? metadata = null,
        MeshOperationId operationId = default,
        ulong targetNodeGeneration = 0,
        ulong authorityOwnerGeneration = 0,
        ulong ownerLeaseGeneration = 0,
        byte messageFollowHopCount = 0,
        ulong sourceNodeGeneration = 0,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource = null,
        ulong deadlineUnixMs = 0,
        ZLinkInboundDispatchLease? inboundDispatchLease = null)
    {
        Parts = parts;
        SourceNodeRid = sourceNodeRid;
        SpotId = spotId;
        RequestSeq = requestSeq;
        _reply = reply;
        ChannelName = channelName;
        Metadata = metadata ?? ZLinkMessageMetadata.Empty;
        OperationId = operationId;
        TargetNodeGeneration = targetNodeGeneration;
        AuthorityOwnerGeneration = authorityOwnerGeneration;
        OwnerLeaseGeneration = ownerLeaseGeneration;
        MessageFollowHopCount = messageFollowHopCount;
        SourceNodeGeneration = sourceNodeGeneration;
        RequestSource = requestSource;
        DeadlineUnixMs = deadlineUnixMs;
        InboundDispatchLease = inboundDispatchLease;
    }

    public IReadOnlyList<Message> Parts { get; }

    // The immutable application-metadata snapshot decoded from the record's
    // application-metadata frame (S8-06A). Empty when the sender attached none.
    public ZLinkMessageMetadata Metadata { get; }

    public RoutingId? SourceNodeRid { get; }

    public string? SpotId { get; }

    // The addressed channel name for node-level channel records
    // (ChannelSend/ChannelRequest). Null for RID-direct node route records
    // (NodeSend/NodeRequest) and for per-spot route records.
    public string? ChannelName { get; }

    public ulong? RequestSeq { get; }

    public MeshOperationId OperationId { get; }

    public ulong TargetNodeGeneration { get; }

    public ulong AuthorityOwnerGeneration { get; }

    public ulong OwnerLeaseGeneration { get; }

    public byte MessageFollowHopCount { get; }

    public ulong SourceNodeGeneration { get; }

    public ZLinkServiceWireCodec.RequestSourceFence? RequestSource { get; }

    public ulong DeadlineUnixMs { get; }

    internal ZLinkInboundDispatchLease? InboundDispatchLease { get; }

    internal void StartDispatch() => InboundDispatchLease?.StartDispatch();

    public bool CanReply => _reply is not null;

    public SubmitResult Reply(IReadOnlyList<Message> parts, SendFlags flags = SendFlags.None)
    {
        if (_reply is null)
            throw new InvalidOperationException(
                "This route record does not carry a reply token.");
        return _reply(parts, flags);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try
        {
            ZLinkMessageParts.DisposeAll(Parts);
        }
        finally
        {
            InboundDispatchLease?.Dispose();
        }
    }
}

/// <summary>
///     A framework-owned inbound subscription (logical multicast) message.
///     Mirrors the subset of the binding <c>TopicMessage</c> surface the
///     subscription drain consumes (topic, parts).
/// </summary>
internal sealed class ZLinkBackendSubscribeMessage : IDisposable
{
    private bool _disposed;

    public ZLinkBackendSubscribeMessage(
        string channelName,
        string topic,
        IReadOnlyList<Message> parts,
        ZLinkMessageMetadata? metadata = null,
        ZLinkInboundDispatchLease? inboundDispatchLease = null)
    {
        ChannelName = channelName;
        Topic = topic;
        Parts = parts;
        Metadata = metadata ?? ZLinkMessageMetadata.Empty;
        InboundDispatchLease = inboundDispatchLease;
    }

    public string ChannelName { get; }

    public string Topic { get; }

    public IReadOnlyList<Message> Parts { get; }

    // The immutable application-metadata snapshot decoded from the logical
    // multicast record's application-metadata frame (S8-06A). Empty when none.
    public ZLinkMessageMetadata Metadata { get; }

    internal ZLinkInboundDispatchLease? InboundDispatchLease { get; }

    internal void StartDispatch() => InboundDispatchLease?.StartDispatch();

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try
        {
            ZLinkMessageParts.DisposeAll(Parts);
        }
        finally
        {
            InboundDispatchLease?.Dispose();
        }
    }
}
