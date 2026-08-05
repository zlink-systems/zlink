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

    public void SendReadyHandler(Action handler)
    {
        if (handler == null)
            throw new ArgumentNullException(nameof(handler));

        var context = SynchronizationContext.Current;
        var native = new NativeMethods.ZlinkSendReadyHandlerDelegate(
            OnNativeSendReady);
        _callbacks.SendReadyHandler = handler;
        _callbacks.SendReadyHandlerContext = context;
        _callbacks.SendReadyHandlerNative = native;
        var rc = NativeMethods.zlink_send_ready_handler(Handle, native,
            IntPtr.Zero);
        if (rc != 0)
        {
            _callbacks.ClearSendReady();
            ZlinkException.ThrowHandlerIfError(rc);
        }
    }

    public void CompletionControlHandler(CompletionControlHandler handler)
    {
        if (handler == null)
            throw new ArgumentNullException(nameof(handler));

        var context = SynchronizationContext.Current;
        lock (_completionControlRegistrationSync)
        {
            // Core always calls the same rooted trampoline. Replacing the
            // managed target therefore cannot expose a collected native
            // delegate or require a second native registration.
            if (_callbacks.CompletionControlNative == null)
            {
                var native = new NativeMethods.ZlinkSocketMsgHandlerDelegate(
                    OnNativeCompletionControl);
                _callbacks.PublishCompletionControl(handler, context);
                _callbacks.CompletionControlNative = native;
                var rc = NativeMethods.zlink_router_completion_control_handler(
                    Handle, native, IntPtr.Zero);
                if (rc != 0)
                {
                    _callbacks.ClearCompletionControl();
                    ZlinkException.ThrowHandlerIfError(rc);
                }
                return;
            }

            _callbacks.PublishCompletionControl(handler, context);
        }
    }

    private void OnNativeSendReady(IntPtr subject, IntPtr userData)
    {
        var handler = _callbacks.SendReadyHandler;
        var context = _callbacks.SendReadyHandlerContext;
        if (handler == null)
            return;

        try
        {
            CallbackDelivery.Post(context, handler);
        }
        catch (Exception ex)
        {
            CallbackExceptionHub.Report(ex);
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

    private unsafe void OnNativeCompletionControl(
        IntPtr sourceRoutingId,
        IntPtr parts,
        nuint partCount,
        IntPtr userData)
    {
        var callback = _callbacks.ReadCompletionControl();
        if (callback == null || sourceRoutingId == IntPtr.Zero)
        {
            if (parts != IntPtr.Zero)
                NativeMethods.zlink_multipart_close(parts, partCount);
            return;
        }

        Message[]? managedParts = null;
        var delivered = false;
        try
        {
            var source = RoutingIdSnapshot
                .FromPointer(sourceRoutingId)
                .ToRoutingId()
                ?? throw new InvalidOperationException(
                    "Completion control source routing id was empty.");
            managedParts = Message.FromNativeVector(parts, partCount);
            parts = IntPtr.Zero;
            partCount = 0;
            delivered = true;
            CallbackDelivery.Post(callback.Context,
                () => callback.Handler(source, managedParts));
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
