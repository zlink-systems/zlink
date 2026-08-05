// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    public void AttachStreamPacket(StreamFramedPacketHandler handler)
    {
        EnsureSupports(nameof(AttachStreamPacket),
            SocketTypePolicy.SocketCapability.StreamAttach);
        if (handler == null)
            throw new ArgumentNullException(nameof(handler));
        lock (_streamModeGate)
        {
            if (_streamAttached || Volatile.Read(ref _streamReceiveMode) != 0)
                throw new ZlinkHandlerException(HandlerResult.Busy,
                    (int)ErrorCode.EBusy);

            var context = SynchronizationContext.Current;
            _callbacks.StreamFramedPacketHandler = handler;
            _callbacks.StreamPacketContext = context;
            _callbacks.StreamPacketNative = OnStreamPacket;
            var rc = NativeMethods.zlink_stream_packet_handler(Handle,
                _callbacks.StreamPacketNative, IntPtr.Zero);
            if (rc != 0)
            {
                _callbacks.StreamFramedPacketHandler = null;
                _callbacks.StreamPacketContext = null;
                _callbacks.StreamPacketNative = null;
                throw ZlinkException.CreateHandlerException(
                    NativeMethods.zlink_errno());
            }

            _streamAttached = true;
        }
    }

    public void AttachStreamPacket(StreamPacketHandler handler)
    {
        if (handler == null)
            throw new ArgumentNullException(nameof(handler));
        AttachStreamPacket((routingId, header, body) =>
            handler(ParsePublicRoutingId(routingId), header, body));
    }

    public void AttachStreamPacket(StreamUInt32FramedPacketHandler handler)
    {
        EnsureSupports(nameof(AttachStreamPacket),
            SocketTypePolicy.SocketCapability.StreamAttach);
        if (handler == null)
            throw new ArgumentNullException(nameof(handler));
        lock (_streamModeGate)
        {
            if (_streamAttached || Volatile.Read(ref _streamReceiveMode) != 0)
                throw new ZlinkHandlerException(HandlerResult.Busy,
                    (int)ErrorCode.EBusy);

            var context = SynchronizationContext.Current;
            _callbacks.StreamUInt32FramedPacketHandler = handler;
            _callbacks.StreamPacketContext = context;
            _callbacks.StreamPacketNative = OnStreamPacketUInt32;
            var rc = NativeMethods.zlink_stream_packet_handler(Handle,
                _callbacks.StreamPacketNative, IntPtr.Zero);
            if (rc != 0)
            {
                _callbacks.StreamUInt32FramedPacketHandler = null;
                _callbacks.StreamPacketContext = null;
                _callbacks.StreamPacketNative = null;
                throw ZlinkException.CreateHandlerException(
                    NativeMethods.zlink_errno());
            }

            _streamAttached = true;
        }
    }

    public bool ReceiveStreamPart(
        out RoutingId? sourceRoutingId,
        out Message? part,
        out bool hasMore,
        RecvFlags flags)
    {
        EnsureSupports(nameof(ReceiveStreamPart),
            SocketTypePolicy.SocketCapability.RoutedReceive);
        if (Volatile.Read(ref _streamReceiveMode) == 0)
        {
            lock (_streamModeGate)
            {
                if (_streamAttached)
                    throw new ZlinkHandlerException(HandlerResult.Busy,
                        (int)ErrorCode.EBusy);
                Volatile.Write(ref _streamReceiveMode, 1);
            }
        }
        var received = new Message();
        try
        {
            if (ReceiveRoutedPartInto(
                    received,
                    out sourceRoutingId,
                    out hasMore,
                    (int)flags))
            {
                part = received;
                return true;
            }

            received.Dispose();
            part = null;
            return false;
        }
        catch
        {
            received.Dispose();
            sourceRoutingId = null;
            hasMore = false;
            part = null;
            throw;
        }
    }

    private static void CloseStreamPacket(IntPtr msg)
    {
        if (msg == IntPtr.Zero)
            return;
        try
        {
            NativeMethods.zlink_msg_close(msg);
        }
        catch
        {
        }
    }

    private unsafe void OnStreamPacket(IntPtr stream, IntPtr routingId,
        IntPtr header, IntPtr body, IntPtr userdata)
    {
        var packetHandler = _callbacks.StreamFramedPacketHandler;
        var context = _callbacks.StreamPacketContext;
        if (packetHandler == null || routingId == IntPtr.Zero)
        {
            CloseStreamPacket(header);
            CloseStreamPacket(body);
            return;
        }

        Message? headerMsg = null;
        Message? bodyMsg = null;
        var delivered = false;
        try
        {
            var routingIdText = RoutingIdCodec.ToPublicString(
                NativeHelpers.ReadRoutingId(ref *(ZlinkRoutingId*)routingId));
            headerMsg = Message.MoveFromNativeSingle(header);
            bodyMsg = Message.MoveFromNativeSingle(body);
            if (context == null)
                packetHandler(routingIdText, headerMsg, bodyMsg);
            else
                CallbackDelivery.Post(
                    context,
                    () => packetHandler(routingIdText, headerMsg, bodyMsg));
            delivered = true;
        }
        catch (Exception ex)
        {
            CallbackExceptionHub.Report(ex);
            if (!delivered)
            {
                try
                {
                    headerMsg?.Dispose();
                }
                catch
                {
                }

                try
                {
                    bodyMsg?.Dispose();
                }
                catch
                {
                }
            }
        }
        finally
        {
            CloseStreamPacket(header);
            CloseStreamPacket(body);
        }
    }

    private unsafe void OnStreamPacketUInt32(IntPtr stream, IntPtr routingId,
        IntPtr header, IntPtr body, IntPtr userdata)
    {
        var packetHandler =
            _callbacks.StreamUInt32FramedPacketHandler;
        var context = _callbacks.StreamPacketContext;
        if (packetHandler == null || routingId == IntPtr.Zero)
        {
            CloseStreamPacket(header);
            CloseStreamPacket(body);
            return;
        }

        var ridBytes = NativeHelpers.ReadRoutingId(
            ref *(ZlinkRoutingId*)routingId);
        if (!RoutingIdCodec.TryToUInt32(ridBytes, out var routingIdValue))
        {
            CloseStreamPacket(header);
            CloseStreamPacket(body);
            return;
        }

        Message? headerMsg = null;
        Message? bodyMsg = null;
        var delivered = false;
        try
        {
            headerMsg = Message.MoveFromNativeSingle(header);
            bodyMsg = Message.MoveFromNativeSingle(body);
            if (context == null)
                packetHandler(routingIdValue, headerMsg, bodyMsg);
            else
                CallbackDelivery.Post(
                    context,
                    () => packetHandler(routingIdValue, headerMsg, bodyMsg));
            delivered = true;
        }
        catch (Exception ex)
        {
            CallbackExceptionHub.Report(ex);
            if (!delivered)
            {
                try
                {
                    headerMsg?.Dispose();
                }
                catch
                {
                }

                try
                {
                    bodyMsg?.Dispose();
                }
                catch
                {
                }
            }
        }
        finally
        {
            CloseStreamPacket(header);
            CloseStreamPacket(body);
        }
    }

}
