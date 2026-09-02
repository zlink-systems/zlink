// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class StreamSocket : RoutedMessageSocketBase, IStreamSocket
{
    public StreamSocket(Context context)
        : base(context, SocketType.Stream)
    {
        Options = new StreamSocketOptions(this);
    }

    public new StreamSocketOptions Options { get; }

    public SendOperation Send(RoutingId routingId)
    {
        return new SocketSendOperation(this, routingId);
    }

    public bool RecvPacket(StreamPacket result,
        RecvFlags flags = RecvFlags.None)
    {
        return Kernel.ReceiveStreamPacket(result, flags);
    }

    public void DisconnectRid(RoutingId peerRid)
    {
        Kernel.DisconnectRid(peerRid);
    }
}
