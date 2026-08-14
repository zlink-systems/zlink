// SPDX-License-Identifier: MPL-2.0

using System.ComponentModel;
using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

[EditorBrowsable(EditorBrowsableState.Never)]
internal abstract class RoutedReceivingSocketBase : SocketBase,
    IReceivingMessageSocket
{
    internal RoutedReceivingSocketBase(Context context, SocketType type)
        : base(context, type)
    {
    }

    internal RoutedReceivingSocketBase(SocketKernel kernel)
        : base(kernel)
    {
    }

    /// <summary>
    ///     Receive a routed message into <paramref name="result" />.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool Recv(Received result, RecvFlags flags = RecvFlags.None)
    {
        return Kernel.ReceiveRoutedInto(result, (int)flags);
    }

    /// <summary>
    ///     Receive a routed message and retain its Core HWM credit with the
    ///     supplied envelope until deterministic cleanup.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool RecvRetained(Received result,
        RecvFlags flags = RecvFlags.None)
    {
        return Kernel.ReceiveRoutedRetainedInto(result, (int)flags);
    }


    public void OnSendReady(Action handler)
    {
        Kernel.SendReadyHandler(handler);
    }
}

[EditorBrowsable(EditorBrowsableState.Never)]
internal abstract class RoutedMessageSocketBase : RoutedReceivingSocketBase
{
    internal RoutedMessageSocketBase(Context context, SocketType type)
        : base(context, type)
    {
    }

    internal RoutedMessageSocketBase(SocketKernel kernel)
        : base(kernel)
    {
    }

    /// <summary>
    ///     Start a synchronous STREAM send operation.
    /// </summary>
    public SendOperation TrySend(RoutingId routingId)
    {
        return new StreamSendOperation(this, routingId);
    }

    /// <summary>
    ///     Send a single routed message part directly.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool Send(RoutingId routingId, Message message,
        SendFlags flags = SendFlags.None)
    {
        return SendRoutedCore(routingId, message, flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool SendStringCore(string routingId, Message message,
        SendFlags flags = SendFlags.None)
    {
        if ((flags & SendFlags.DontWait) != 0)
            return SocketKernel.TrySendOrThrow(Kernel.SendNoWaitResult(routingId,
                message));

        Kernel.Send(routingId, message, flags);
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool SendRoutedCore(RoutingId routingId, Message message,
        SendFlags flags = SendFlags.None)
    {
        if ((flags & SendFlags.DontWait) != 0)
            return SocketKernel.TrySendOrThrow(
                Kernel.SendRoutedMessageResultUnchecked(routingId, message,
                    (int)flags));

        Kernel.SendRoutedMessageUnchecked(routingId, message, flags);
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool SendStringCore(string routingId, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        if ((flags & SendFlags.DontWait) != 0)
            return SocketKernel.TrySendOrThrow(Kernel.SendNoWaitResult(routingId,
                parts));

        Kernel.Send(routingId, parts, flags);
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool SendRoutedCore(RoutingId routingId,
        IReadOnlyList<Message> parts, SendFlags flags = SendFlags.None)
    {
        if (parts.Count == 1)
            return SendRoutedCore(routingId, parts[0], flags);
        if ((flags & SendFlags.DontWait) != 0)
            return SocketKernel.TrySendOrThrow(Kernel.SendNoWaitResult(routingId,
                parts));

        Kernel.Send(routingId, parts, flags);
        return true;
    }

    internal SendResult SendNoWaitResult(string routingId, Message message)
    {
        return Kernel.SendNoWaitResult(routingId, message);
    }

    internal SendResult SendNoWaitResult(RoutingId routingId, Message message)
    {
        return Kernel.SendNoWaitResult(routingId, message);
    }

    internal SendResult SendNoWaitResult(string routingId,
        IReadOnlyList<Message> parts)
    {
        return Kernel.SendNoWaitResult(routingId, parts);
    }

    internal SendResult SendNoWaitResult(RoutingId routingId,
        IReadOnlyList<Message> parts)
    {
        return Kernel.SendNoWaitResult(routingId, parts);
    }

    internal void OnReceive(SocketRecvHandler handler)
    {
        Kernel.RecvHandler(handler);
    }

    /// <summary>
    ///     Receive one routed wire part into <paramref name="result" />.
    /// </summary>
    /// <param name="result">
    ///     Reusable message storage that is overwritten on
    ///     success. With <see cref="RecvFlags.DontWait" />, it is left unchanged
    ///     when no part is available.
    /// </param>
    /// <param name="routingId">
    ///     Source routing id for the first received
    ///     part.
    /// </param>
    /// <param name="hasMore">
    ///     True when more parts remain for the current
    ///     routed message.
    /// </param>
    /// <param name="flags">Receive flags.</param>
    /// <returns>
    ///     true on success, false when DontWait is set and no data is
    ///     available.
    /// </returns>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool RecvPart(Message result, out RoutingId? routingId,
        out bool hasMore, RecvFlags flags = RecvFlags.None)
    {
        return Kernel.ReceiveRoutedPartInto(result, out routingId, out hasMore,
            (int)flags);
    }
}
