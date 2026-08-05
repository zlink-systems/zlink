// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal sealed class XSubSocket : SubscriberSocketBase, IXSubSocket
{
    public XSubSocket(Context context)
        : base(context, SocketType.XSub)
    {
        Options = new SubSocketOptions(this);
    }

    public new SubSocketOptions Options { get; }
}