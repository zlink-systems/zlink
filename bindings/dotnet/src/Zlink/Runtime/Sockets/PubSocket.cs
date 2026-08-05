// SPDX-License-Identifier: MPL-2.0


namespace Systems.Zlink;

internal sealed class PubSocket : PublisherSocketBase, IPubSocket
{
    public PubSocket(Context context)
        : base(context, SocketType.Pub)
    {
        Options = new PubSocketOptions(this);
    }

    public new PubSocketOptions Options { get; }

    public void SetRoutingId(RoutingId routingId)
    {
        Kernel.SetOption(SocketOptions.RoutingId, routingId.ToBytes());
    }

    public RoutingId GetRoutingId()
    {
        return RoutingId.From(Kernel.GetOption(SocketOptions.RoutingId));
    }

}
