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

    public void OnPacket(StreamPacketHandler handler)
    {
        Kernel.AttachStreamPacket(handler);
    }

    public bool RecvPart(
        out RoutingId? sourceRoutingId,
        out Message? part,
        out bool hasMore,
        RecvFlags flags = RecvFlags.None)
    {
        return Kernel.ReceiveStreamPart(
            out sourceRoutingId,
            out part,
            out hasMore,
            flags);
    }

    public void DisconnectRid(RoutingId peerRid)
    {
        Kernel.DisconnectRid(peerRid);
    }
}
