// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal delegate void StreamFramedPacketHandler(
    string routingId,
    Message header,
    Message body);

internal delegate void StreamUInt32FramedPacketHandler(uint routingId,
    Message header, Message body);

internal delegate void SocketRecvHandler(string routingId, Message[] parts);

internal static class SocketInterop
{
    internal static SocketBase RequireSocket(IZlinkSocket socket, string paramName)
    {
        if (socket == null)
            throw new ArgumentNullException(paramName);
        if (socket is not SocketBase concrete)
            throw new ArgumentException(
                "socket must be a concrete zlink socket instance",
                paramName);
        return concrete;
    }

    internal static IntPtr RequirePollableHandle(IZlinkSocket socket,
        string paramName)
    {
        if (socket == null)
            throw new ArgumentNullException(paramName);
        if (socket is not SocketBase concrete)
            throw new ArgumentException(
                "socket must be a concrete zlink socket instance", paramName);
        return concrete.Handle;
    }

    internal static DealerSocket RequireDealerSocket(IDealerSocket socket,
        string paramName)
    {
        if (socket == null)
            throw new ArgumentNullException(paramName);
        if (socket is not DealerSocket concrete)
            throw new ArgumentException(
                "socket must be a concrete zlink dealer socket instance",
                paramName);
        return concrete;
    }

    internal static RouterSocket RequireRouterSocket(IRouterSocket socket,
        string paramName)
    {
        if (socket == null)
            throw new ArgumentNullException(paramName);
        if (socket is not RouterSocket concrete)
            throw new ArgumentException(
                "socket must be a concrete zlink router socket instance",
                paramName);
        return concrete;
    }

    internal static PubSocket RequirePubSocket(IPubSocket socket,
        string paramName)
    {
        if (socket == null)
            throw new ArgumentNullException(paramName);
        if (socket is not PubSocket concrete)
            throw new ArgumentException(
                "socket must be a concrete zlink pub socket instance",
                paramName);
        return concrete;
    }

    internal static Timer RequireTimer(IZlinkTimer timer, string paramName)
    {
        if (timer == null)
            throw new ArgumentNullException(paramName);
        if (timer is not Timer concrete)
            throw new ArgumentException(
                "timer must be a concrete zlink timer instance",
                paramName);
        return concrete;
    }
}
