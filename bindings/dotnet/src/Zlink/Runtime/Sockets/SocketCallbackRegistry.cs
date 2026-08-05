// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed class SocketCallbackRegistry
{
    internal sealed class CompletionControlCallbackState
    {
        internal CompletionControlCallbackState(
            CompletionControlHandler handler,
            SynchronizationContext? context)
        {
            Handler = handler;
            Context = context;
        }

        internal CompletionControlHandler Handler { get; }
        internal SynchronizationContext? Context { get; }
    }

    internal SocketRecvHandler? RecvHandler;
    internal SynchronizationContext? RecvHandlerContext;
    internal NativeMethods.ZlinkSocketMsgHandlerDelegate? RecvHandlerNative;
    internal Action? SendReadyHandler;
    internal SynchronizationContext? SendReadyHandlerContext;
    internal NativeMethods.ZlinkSendReadyHandlerDelegate? SendReadyHandlerNative;
    private CompletionControlCallbackState? _completionControl;
    internal NativeMethods.ZlinkSocketMsgHandlerDelegate? CompletionControlNative;
    internal StreamFramedPacketHandler? StreamFramedPacketHandler;
    internal SynchronizationContext? StreamPacketContext;
    internal NativeMethods.ZlinkStreamOnPacketDelegate? StreamPacketNative;
    internal StreamUInt32FramedPacketHandler? StreamUInt32FramedPacketHandler;

    internal void ClearStream()
    {
        StreamFramedPacketHandler = null;
        StreamUInt32FramedPacketHandler = null;
        StreamPacketNative = null;
        StreamPacketContext = null;
    }

    internal void ClearReceive()
    {
        RecvHandler = null;
        RecvHandlerContext = null;
        RecvHandlerNative = null;
    }

    internal void ClearSendReady()
    {
        SendReadyHandler = null;
        SendReadyHandlerContext = null;
        SendReadyHandlerNative = null;
    }

    internal void ClearCompletionControl()
    {
        Volatile.Write(ref _completionControl, null);
        CompletionControlNative = null;
    }

    internal CompletionControlCallbackState? ReadCompletionControl()
    {
        return Volatile.Read(ref _completionControl);
    }

    internal void PublishCompletionControl(
        CompletionControlHandler handler,
        SynchronizationContext? context)
    {
        Volatile.Write(ref _completionControl,
            new CompletionControlCallbackState(handler, context));
    }

    internal void ClearAllNonStream()
    {
        ClearReceive();
        ClearSendReady();
        ClearCompletionControl();
    }
}
