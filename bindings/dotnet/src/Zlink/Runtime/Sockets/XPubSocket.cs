// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal sealed class XPubSocket : PublisherSocketBase, IXPubSocket
{
    public XPubSocket(Context context)
        : base(context, SocketType.XPub)
    {
        Options = new PubSocketOptions(this);
    }

    public new PubSocketOptions Options { get; }

    public bool ReceiveSubscriptionEvent(SubscriptionEvent result,
        RecvFlags flags = RecvFlags.None)
    {
        return Kernel.ReceiveSubscriptionEvent(result, flags);
    }

}
