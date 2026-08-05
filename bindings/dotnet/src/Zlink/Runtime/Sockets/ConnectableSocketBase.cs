// SPDX-License-Identifier: MPL-2.0

using System.ComponentModel;
using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

[EditorBrowsable(EditorBrowsableState.Never)]
internal abstract class ConnectableSocketBase : SocketBase, IConnectableSocket
{
    internal ConnectableSocketBase(Context context, SocketType type)
        : base(context, type)
    {
    }

    internal ConnectableSocketBase(SocketKernel kernel)
        : base(kernel)
    {
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
}
