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
        lock (_streamRegistrationSync)
        {
            var context = SynchronizationContext.Current;
            var previousFramed = _callbacks.StreamFramedPacketHandler;
            var previousUInt32 = _callbacks.StreamUInt32FramedPacketHandler;
            var previousContext = _callbacks.StreamPacketContext;
            var previousNative = _callbacks.StreamPacketNative;
            _callbacks.StreamFramedPacketHandler = handler;
            _callbacks.StreamUInt32FramedPacketHandler = null;
            _callbacks.StreamPacketContext = context;
            _callbacks.StreamPacketNative = OnStreamPacket;
            var rc = NativeMethods.zlink_stream_packet_handler(Handle,
                _callbacks.StreamPacketNative, IntPtr.Zero);
            if (rc != 0)
            {
                _callbacks.StreamFramedPacketHandler = previousFramed;
                _callbacks.StreamUInt32FramedPacketHandler = previousUInt32;
                _callbacks.StreamPacketContext = previousContext;
                _callbacks.StreamPacketNative = previousNative;
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
        AttachStreamPacket((StreamFramedPacketHandler)((routingId, header, body) =>
            handler(routingId, header, body)));
    }

    public void AttachStreamPacket(StreamUInt32FramedPacketHandler handler)
    {
        EnsureSupports(nameof(AttachStreamPacket),
            SocketTypePolicy.SocketCapability.StreamAttach);
        if (handler == null)
            throw new ArgumentNullException(nameof(handler));
        lock (_streamRegistrationSync)
        {
            var context = SynchronizationContext.Current;
            var previousFramed = _callbacks.StreamFramedPacketHandler;
            var previousUInt32 = _callbacks.StreamUInt32FramedPacketHandler;
            var previousContext = _callbacks.StreamPacketContext;
            var previousNative = _callbacks.StreamPacketNative;
            _callbacks.StreamFramedPacketHandler = null;
            _callbacks.StreamUInt32FramedPacketHandler = handler;
            _callbacks.StreamPacketContext = context;
            _callbacks.StreamPacketNative = OnStreamPacketUInt32;
            var rc = NativeMethods.zlink_stream_packet_handler(Handle,
                _callbacks.StreamPacketNative, IntPtr.Zero);
            if (rc != 0)
            {
                _callbacks.StreamFramedPacketHandler = previousFramed;
                _callbacks.StreamUInt32FramedPacketHandler = previousUInt32;
                _callbacks.StreamPacketContext = previousContext;
                _callbacks.StreamPacketNative = previousNative;
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
        lock (_streamRegistrationSync)
        {
            if (_streamAttached)
                throw ZlinkException.CreateHandlerException(
                    HandlerResult.Busy);
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
            ref ZlinkRoutingId nativeRoutingId = ref *(ZlinkRoutingId*)routingId;
            var publicRoutingId = RoutingId.FromNative(ref nativeRoutingId);
            if (!publicRoutingId.HasValue)
            {
                CloseStreamPacket(header);
                CloseStreamPacket(body);
                return;
            }
            headerMsg = Message.MoveFromNativeSingle(header);
            bodyMsg = Message.MoveFromNativeSingle(body);
            if (context == null)
                packetHandler(publicRoutingId.Value, headerMsg, bodyMsg);
            else
                CallbackDelivery.Post(
                    context,
                    () => packetHandler(publicRoutingId.Value, headerMsg, bodyMsg));
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
