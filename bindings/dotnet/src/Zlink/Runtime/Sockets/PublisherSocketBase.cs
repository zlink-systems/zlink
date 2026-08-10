// SPDX-License-Identifier: MPL-2.0

using System.ComponentModel;
using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

[EditorBrowsable(EditorBrowsableState.Never)]
internal abstract class PublisherSocketBase : ConnectableSocketBase, IPublisherSocket
{
    internal PublisherSocketBase(Context context, SocketType type)
        : base(context, type)
    {
    }

    internal PublisherSocketBase(SocketKernel kernel)
        : base(kernel)
    {
    }

    /// <summary>
    ///     Start a topic publish operation (operation builder).
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public SendOperation Publish(string topic)
    {
        if (topic == null)
            throw new ArgumentNullException(nameof(topic));
        return new PublisherSendOperation(this, topic);
    }

    public void OnSendReady(Action handler)
    {
        Kernel.SendReadyHandler(handler);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool PublishCore(string topic, Message message,
        SendFlags flags = SendFlags.None)
    {
        if ((flags & SendFlags.DontWait) != 0)
            return SocketKernel.TrySendOrThrow(Kernel.PublishNoWaitResult(topic,
                message));

        Kernel.PublishMessageUnchecked(topic, message, flags);
        return true;
    }

    internal bool PublishCore(string topic, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        if (parts.Count == 1)
            return PublishCore(topic, parts[0], flags);
        if ((flags & SendFlags.DontWait) != 0)
            return SocketKernel.TrySendOrThrow(Kernel.PublishNoWaitResult(topic,
                parts));

        Kernel.Publish(topic, parts, flags);
        return true;
    }

    internal SendResult PublishNoWaitResult(string topic, Message message)
    {
        return Kernel.PublishNoWaitResult(topic, message);
    }

    internal SendResult PublishNoWaitResult(string topic,
        IReadOnlyList<Message> parts)
    {
        return Kernel.PublishNoWaitResult(topic, parts);
    }
}
