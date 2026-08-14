// SPDX-License-Identifier: MPL-2.0

using System.ComponentModel;
using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

[EditorBrowsable(EditorBrowsableState.Never)]
internal abstract class ReceivingMessageSocketBase : ConnectableSocketBase,
    IReceivingMessageSocket
{
    internal ReceivingMessageSocketBase(Context context, SocketType type)
        : base(context, type)
    {
    }

    internal ReceivingMessageSocketBase(SocketKernel kernel)
        : base(kernel)
    {
    }

    /// <summary>
    ///     Receive a message into <paramref name="result" />. Caller-provided
    ///     storage lets the binding refill the same Received instance across
    ///     calls and avoid per-receive allocation.
    /// </summary>
    /// <param name="result">
    ///     Long-lived Received storage. Starting a receive releases the
    ///     preceding envelope held by the same storage before waiting for data.
    /// </param>
    /// <param name="flags">
    ///     Recv flags. With <see cref="RecvFlags.DontWait" />,
    ///     returns <c>false</c> when no data is available; otherwise blocks
    ///     until data arrives or the socket reports a hard error.
    /// </param>
    /// <returns>
    ///     true on success, false when DontWait is set and no data is
    ///     available.
    /// </returns>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public virtual bool Recv(Received result, RecvFlags flags = RecvFlags.None)
    {
        return Kernel.ReceiveInto(result, (int)flags);
    }

    /// <summary>
    ///     Receive a message and retain its Core HWM credit with the supplied
    ///     envelope until deterministic cleanup.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public virtual bool RecvRetained(Received result,
        RecvFlags flags = RecvFlags.None)
    {
        return Kernel.ReceiveRetainedInto(result, (int)flags);
    }


    public void OnSendReady(Action handler)
    {
        Kernel.SendReadyHandler(handler);
    }

    internal void OnReceive(SocketRecvHandler handler)
    {
        Kernel.RecvHandler(handler);
    }

    /// <summary>
    ///     Receive one wire part into <paramref name="result" />. This exposes the
    ///     single-part receive primitive directly; when <paramref name="hasMore" />
    ///     is true, call again to read the remaining parts of the same message.
    /// </summary>
    /// <param name="result">
    ///     Reusable message storage that is overwritten on
    ///     success. With <see cref="RecvFlags.DontWait" />, it is left unchanged
    ///     when no part is available.
    /// </param>
    /// <param name="hasMore">
    ///     True when more parts remain for the current
    ///     message.
    /// </param>
    /// <param name="flags">Receive flags.</param>
    /// <returns>
    ///     true on success, false when DontWait is set and no data is
    ///     available.
    /// </returns>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool RecvPart(Message result, out bool hasMore,
        RecvFlags flags = RecvFlags.None)
    {
        return Kernel.ReceivePartInto(result, out hasMore, (int)flags);
    }
}

[EditorBrowsable(EditorBrowsableState.Never)]
internal abstract class MessageSocketBase : ReceivingMessageSocketBase,
    IMessageSocket
{
    internal MessageSocketBase(Context context, SocketType type)
        : base(context, type)
    {
    }

    internal MessageSocketBase(SocketKernel kernel)
        : base(kernel)
    {
    }

    /// <summary>
    ///     Start a send operation (operation builder).
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public SendOperation Send()
    {
        return new MessageSocketSendOperation(this);
    }

    /// <summary>
    ///     Send a single message part directly.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool Send(Message message, SendFlags flags = SendFlags.None)
    {
        return SendCore(message, flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool SendCore(Message message, SendFlags flags = SendFlags.None)
    {
        if ((flags & SendFlags.DontWait) != 0)
            return SocketKernel.TrySendOrThrow(
                Kernel.SendMessageResultUnchecked(message, (int)flags));

        Kernel.SendMessageUnchecked(message, flags);
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool SendCore(IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        if (parts.Count == 1)
            return SendCore(parts[0], flags);
        if ((flags & SendFlags.DontWait) != 0)
            return SocketKernel.TrySendOrThrow(Kernel.SendNoWaitResult(parts));

        Kernel.Send(parts, flags);
        return true;
    }

    internal SendResult SendNoWaitResult(Message message)
    {
        return Kernel.SendNoWaitResult(message);
    }

    internal SendResult SendNoWaitResult(IReadOnlyList<Message> parts)
    {
        return Kernel.SendNoWaitResult(parts);
    }

}
