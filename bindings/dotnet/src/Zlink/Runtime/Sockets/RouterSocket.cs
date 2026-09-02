// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class RouterSocket : RoutedReceivingSocketBase, IRouterSocket
{
    private static readonly TimeSpan DefaultRequestTimeout = TimeSpan.FromSeconds(5);

    public RouterSocket(Context context)
        : base(context, SocketType.Router)
    {
        Options = new RouterSocketOptions(this);
    }

    public new RouterSocketOptions Options { get; }

    public SendOperation Send(RoutingId routingId)
    {
        return new SocketSendOperation(this, routingId);
    }

    public void SetRoutingId(RoutingId routingId)
    {
        Kernel.SetOption(SocketOptions.RoutingId, routingId.ToBytes());
    }

    public RoutingId GetRoutingId()
    {
        return RoutingId.From(Kernel.GetOption(SocketOptions.RoutingId));
    }

    public void Connect(string address)
    {
        SocketConnectionOperations.Connect(Kernel, address);
    }

    public void Disconnect(string address)
    {
        SocketConnectionOperations.Disconnect(Kernel, address);
    }

    public void DisconnectRid(RoutingId peerRid)
    {
        SocketConnectionOperations.DisconnectRid(Kernel, peerRid);
    }

    /// <summary>
    ///     Start a request to a specific peer (operation builder).
    /// </summary>
    public RequestOperation Request(RoutingId peerRid)
    {
        return new RouterPeerRequestOperation(this, peerRid);
    }

    /// <summary>
    ///     Start a reply (operation builder).
    /// </summary>
    public ReplyOperation Reply(RoutingId rid, ReplyToken replyToken)
    {
        ArgumentNullException.ThrowIfNull(replyToken);
        if (!replyToken.IsOwnedBy(Kernel.ReplyTokenOwner))
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        return new RouterPeerReplyOperation(this, rid, replyToken);
    }

    internal Task<IReadOnlyList<Message>> RequestCore(RoutingId peerRid,
        IReadOnlyList<Message> parts, TimeSpan timeout, CancellationToken ct)
    {
        var timeoutMs = RequestReplySupport.NormalizeRequestTimeout(
            timeout,
            DefaultRequestTimeout);
        return Kernel.RequestAsync(peerRid, parts, timeoutMs, ct);
    }

    internal IReadOnlyList<Message> RequestCore(RoutingId peerRid,
        IReadOnlyList<Message> parts, TimeSpan timeout)
    {
        var timeoutMs = RequestReplySupport.NormalizeRequestTimeout(timeout,
            DefaultRequestTimeout);
        return Kernel.Request(peerRid, parts, timeoutMs);
    }

    internal void ReplyCore(RoutingId peerRid, ReplyToken replyToken,
        IReadOnlyList<Message> parts)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        Kernel.SendReplyCore(peerRid, replyToken, parts);
    }

}
