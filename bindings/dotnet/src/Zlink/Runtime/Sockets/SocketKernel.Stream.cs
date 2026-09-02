// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel
{
    internal bool ReceiveStreamPacket(StreamPacket result, RecvFlags flags)
    {
        ArgumentNullException.ThrowIfNull(result);
        result.BeginReceive();

        ZlinkMsg header = default;
        ZlinkMsg body = default;
        var headerInitialized = false;
        var bodyInitialized = false;
        try
        {
            var rc = NativeMethods.zlink_msg_init(ref header);
            if (rc != 0)
                throw ZlinkException.CreateRecvException(
                    NativeMethods.zlink_errno());
            headerInitialized = true;

            rc = NativeMethods.zlink_msg_init(ref body);
            if (rc != 0)
                throw ZlinkException.CreateRecvException(
                    NativeMethods.zlink_errno());
            bodyInitialized = true;

            rc = NativeMethods.zlink_stream_recv_packet(Handle,
                out var sourceRoutingId, ref header, ref body, (int)flags);
            if ((RecvResult)rc == RecvResult.NoData
                && (flags & RecvFlags.DontWait) != 0)
                return false;
            if ((RecvResult)rc != RecvResult.Ok)
                throw ZlinkException.CreateRecvException((RecvResult)rc);

            var routingId = RoutingIdSnapshot.FromPointer(sourceRoutingId)
                .ToRoutingId();
            if (!routingId.HasValue)
                throw new ZlinkRecvException(RecvResult.InternalError);

            var headerMessage = Message.MoveFromNative(ref header);
            Message? bodyMessage = null;
            try
            {
                bodyMessage = Message.MoveFromNative(ref body);
                result.CompleteReceive(routingId.Value, headerMessage,
                    bodyMessage);
            }
            catch
            {
                headerMessage.Dispose();
                bodyMessage?.Dispose();
                throw;
            }
            return true;
        }
        finally
        {
            if (headerInitialized)
                NativeMethods.zlink_msg_close(ref header);
            if (bodyInitialized)
                NativeMethods.zlink_msg_close(ref body);
            result.EndReceive();
        }
    }
}
