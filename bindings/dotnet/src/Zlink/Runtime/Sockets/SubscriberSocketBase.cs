// SPDX-License-Identifier: MPL-2.0

using System.ComponentModel;
using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

[EditorBrowsable(EditorBrowsableState.Never)]
internal abstract class SubscriberSocketBase : ConnectableSocketBase, ISubscriberSocket
{
    internal SubscriberSocketBase(Context context, SocketType type)
        : base(context, type)
    {
    }

    internal SubscriberSocketBase(SocketKernel kernel)
        : base(kernel)
    {
    }

    public void SetSubscription(string topicOrPattern)
    {
        Kernel.SetSubscription(topicOrPattern);
    }

    public void UnsetSubscription(string topicOrPattern)
    {
        Kernel.UnsetSubscription(topicOrPattern);
    }

    public SubscriptionEntry? SubscriptionAt(int index)
    {
        return SubscriptionIntrospection.At(Handle, index);
    }

    public bool Subscribe(TopicMessage result, RecvFlags flags = RecvFlags.None)
    {
        return Kernel.SubscribeIntoSubscriber(result, (int)flags);
    }

    public bool SubscribeRetained(TopicMessage result,
        RecvFlags flags = RecvFlags.None)
    {
        return Kernel.SubscribeRetainedIntoSubscriber(result, (int)flags);
    }

}
