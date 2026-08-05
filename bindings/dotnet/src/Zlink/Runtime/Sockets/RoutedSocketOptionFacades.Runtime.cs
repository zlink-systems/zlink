// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

public sealed partial class DealerSocketOptions
{
    internal DealerSocketOptions(ISocketOptionEndpoint socket)
        : base(socket)
    {
    }

    internal RoutingId RoutingId
    {
        get => RoutingId.From(Socket.GetOption(SocketOptions.RoutingId));
        set => Socket.SetOption(SocketOptions.RoutingId, value.ToBytes());
    }
}

public sealed partial class RouterSocketOptions
{
    internal RouterSocketOptions(ISocketOptionEndpoint socket)
        : base(socket)
    {
    }

    internal RoutingId RoutingId
    {
        get => RoutingId.From(Socket.GetOption(SocketOptions.RoutingId));
        set => Socket.SetOption(SocketOptions.RoutingId, value.ToBytes());
    }
}