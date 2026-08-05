// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

internal static class SocketConnectionOperations
{
    internal static void Connect(SocketKernel kernel, string address)
    {
        kernel.Connect(address);
    }

    internal static void Disconnect(SocketKernel kernel, string address)
    {
        kernel.Disconnect(address);
    }

    internal static void DisconnectRid(SocketKernel kernel, RoutingId peerRid)
    {
        kernel.DisconnectRid(peerRid);
    }
}
