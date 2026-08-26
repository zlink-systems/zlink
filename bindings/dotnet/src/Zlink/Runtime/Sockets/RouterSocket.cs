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

    public RoutedSendOperation Send(RoutingId routingId)
    {
        return new RoutedAsyncSendOperation(this, routingId);
    }

    internal Task SendSingleAsyncCore(RoutingId routingId, Message message,
        CancellationToken cancellationToken)
    {
        return Kernel.SendCompletion.SendSingleAsync(routingId, message,
            cancellationToken);
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
    public ReplyOperation Reply(RoutingId rid, ulong requestSeq)
    {
        return new RouterPeerReplyOperation(this, rid, requestSeq);
    }

    internal Task<IReadOnlyList<Message>> RequestCore(RoutingId peerRid,
        IReadOnlyList<Message> parts, TimeSpan timeout, CancellationToken ct)
    {
        var timeoutMs = RequestReplySupport.NormalizeRequestTimeout(
            timeout,
            DefaultRequestTimeout);
        return Kernel.RequestAsync(peerRid, parts, timeoutMs, ct);
    }

    internal void ReplyCore(RoutingId peerRid, ulong requestSeq,
        IReadOnlyList<Message> parts)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        var nativeRoutingId = peerRid.ToNative();
        var cloned = RequestReplySupport.CloneParts(parts);
        lock (SubmitGate)
        {
            try
            {
                RequestReplySupport.SubmitClonedParts(cloned,
                    (ref ZlinkMsg nativePart,
                            NativeMethods.ZlinkPartFlag partFlag) =>
                        NativeMethods.zlink_router_reply_part(Handle,
                            ref nativeRoutingId, requestSeq, ref nativePart,
                            partFlag));
            }
            finally
            {
                RequestReplySupport.DisposeParts(cloned);
            }
        }
    }

}
