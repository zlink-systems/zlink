// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal sealed class DealerSocket : ReceivingMessageSocketBase, IDealerSocket
{
    public DealerSocket(Context context)
        : base(context, SocketType.Dealer)
    {
        Options = new DealerSocketOptions(this);
    }

    public SendOperation Send()
    {
        return new SocketSendOperation(this);
    }

    public new DealerSocketOptions Options { get; }

    public void SetRoutingId(RoutingId routingId)
    {
        Kernel.SetOption(SocketOptions.RoutingId, routingId.ToBytes());
    }

    public RoutingId GetRoutingId()
    {
        return RoutingId.From(Kernel.GetOption(SocketOptions.RoutingId));
    }

    public RequestOperation Request()
    {
        return new DealerRequestOperation(this);
    }

    internal RequestSubmission RequestCore(
        IReadOnlyList<Message> parts, TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        var timeoutMs = RequestReplySupport.NormalizeTimeout(timeout);
        return Kernel.RequestAsync(null, parts, timeoutMs, cancellationToken);
    }

    internal IReadOnlyList<Message> RequestCore(IReadOnlyList<Message> parts,
        TimeSpan timeout)
    {
        var timeoutMs = RequestReplySupport.NormalizeTimeout(timeout);
        return Kernel.Request(null, parts, timeoutMs);
    }
}
