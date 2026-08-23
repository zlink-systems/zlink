// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed class SocketCallbackRegistry
{
    internal SocketRecvHandler? RecvHandler;
    internal SynchronizationContext? RecvHandlerContext;
    internal NativeMethods.ZlinkSocketMsgHandlerDelegate? RecvHandlerNative;
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

    internal void ClearAllNonStream()
    {
        ClearReceive();
    }
}
