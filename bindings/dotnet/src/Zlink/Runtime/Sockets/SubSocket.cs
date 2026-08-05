// SPDX-License-Identifier: MPL-2.0


namespace Systems.Zlink;

internal sealed class SubSocket : SubscriberSocketBase, ISubSocket
{
    public SubSocket(Context context)
        : base(context, SocketType.Sub)
    {
        Options = new SubSocketOptions(this);
    }

    public new SubSocketOptions Options { get; }

    public void SetRoutingId(RoutingId routingId)
    {
        Kernel.SetOption(SocketOptions.RoutingId, routingId.ToBytes());
    }

    public RoutingId GetRoutingId()
    {
        return RoutingId.From(Kernel.GetOption(SocketOptions.RoutingId));
    }

}
