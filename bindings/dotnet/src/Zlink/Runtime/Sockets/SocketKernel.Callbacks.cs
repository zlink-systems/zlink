// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    public void RecvHandler(SocketRecvHandler handler)
    {
        EnsureSupports(nameof(RecvHandler),
            SocketTypePolicy.SocketCapability.ReceiveHandler);
        if (handler == null)
            throw new ArgumentNullException(nameof(handler));

        var context = SynchronizationContext.Current;
        var socketNative = new NativeMethods.ZlinkSocketMsgHandlerDelegate(
            OnNativeReceive);
        _callbacks.RecvHandler = handler;
        _callbacks.RecvHandlerContext = context;
        _callbacks.RecvHandlerNative = socketNative;
        var rc = NativeMethods.zlink_recv_handler(Handle, socketNative, IntPtr.Zero);
        if (rc != 0)
        {
            _callbacks.ClearReceive();
            ZlinkException.ThrowHandlerIfError(rc);
        }
    }

    private unsafe void OnNativeReceive(IntPtr sourceRoutingId, IntPtr parts,
        nuint partCount, IntPtr userData)
    {
        var handler = _callbacks.RecvHandler;
        var context = _callbacks.RecvHandlerContext;
        if (handler == null)
        {
            if (parts != IntPtr.Zero)
                NativeMethods.zlink_multipart_close(parts, partCount);
            return;
        }

        Message[]? managedParts = null;
        var delivered = false;
        try
        {
            var routingId = string.Empty;
            if (sourceRoutingId != IntPtr.Zero)
            {
                var nativeRoutingId = (ZlinkRoutingId*)sourceRoutingId;
                routingId = RoutingIdCodec.ToPublicString(
                    NativeHelpers.ReadRoutingId(ref *nativeRoutingId));
            }

            managedParts = Message.FromNativeVector(parts, partCount);
            parts = IntPtr.Zero;
            partCount = 0;
            delivered = true;
            CallbackDelivery.Post(context,
                () => handler(routingId, managedParts));
        }
        catch (Exception ex)
        {
            CallbackExceptionHub.Report(ex);
            if (!delivered && managedParts != null)
                foreach (var part in managedParts)
                    part.Dispose();
        }
        finally
        {
            if (parts != IntPtr.Zero)
                NativeMethods.zlink_multipart_close(parts, partCount);
        }
    }

}
