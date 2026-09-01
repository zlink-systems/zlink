// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

/// <summary>
///     Submits one DEALER/ROUTER request on the calling thread and lets the
///     Core reply callback complete the task. DEALER target selection remains
///     Core-owned; ROUTER requests keep their explicit exact target.
/// </summary>
/// <remarks>
///     There is no binding-owned pending queue, retry loop, deadline timer or
///     dispatcher thread here. Core owns the send-side HWM wait
///     (<c>ZLINK_OPT_SNDTIMEO</c>) and the reply deadline
///     (<c>ZLINK_REQUEST_TIMED_OUT</c>); the binding only bridges the reply
///     callback into a <see cref="TaskCompletionSource{TResult}" />.
/// </remarks>
internal static class RoutedRequestSubmitter
{
    private static readonly NativeMethods.ZlinkReplyHandlerDelegate
        ReplyHandler = OnNativeReply;

    private static readonly IntPtr ReplyHandlerPointer =
        Marshal.GetFunctionPointerForDelegate(ReplyHandler);

    internal static Task<IReadOnlyList<Message>> RequestAsync(IntPtr handle,
        SocketType socketType, RoutingId? routerRoutingId,
        IReadOnlyList<Message> parts, uint timeoutMs,
        CancellationToken cancellationToken)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        if (cancellationToken.IsCancellationRequested)
            return Task.FromCanceled<IReadOnlyList<Message>>(
                cancellationToken);

        var pending = new PendingRequest(cancellationToken);
        var self = GCHandle.Alloc(pending, GCHandleType.Normal);
        var userData = GCHandle.ToIntPtr(self);
        try
        {
            SubmitParts(handle, socketType, routerRoutingId, parts, timeoutMs,
                (int)SendFlags.DontWait, userData);
        }
        catch
        {
            // No reply callback runs when the submit failed, so the operation
            // state is still owned here.
            self.Free();
            throw;
        }

        pending.AttachCancellation();
        return pending.Task;
    }

    internal static IReadOnlyList<Message> Request(IntPtr handle,
        SocketType socketType, RoutingId? routerRoutingId,
        IReadOnlyList<Message> parts, uint timeoutMs, SendFlags flags)
    {
        var pending = new BlockingRequest();
        Submit(handle, socketType, routerRoutingId, parts, timeoutMs, flags,
            pending);
        return pending.Wait();
    }

    internal static void Request(IntPtr handle, SocketType socketType,
        RoutingId? routerRoutingId, IReadOnlyList<Message> parts,
        uint timeoutMs, SendFlags flags, RequestCallback callback)
    {
        if (callback == null)
            throw new ArgumentNullException(nameof(callback));
        Submit(handle, socketType, routerRoutingId, parts, timeoutMs, flags,
            new CallbackRequest(callback));
    }

    private static void Submit(IntPtr handle, SocketType socketType,
        RoutingId? routerRoutingId, IReadOnlyList<Message> parts,
        uint timeoutMs, SendFlags flags, IRequestCompletion completion)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        var self = GCHandle.Alloc(completion, GCHandleType.Normal);
        try
        {
            SubmitParts(handle, socketType, routerRoutingId, parts, timeoutMs,
                (int)flags, GCHandle.ToIntPtr(self));
        }
        catch
        {
            self.Free();
            throw;
        }
    }

    private static unsafe ZlinkRoutedSubmitTarget SelectRouterTarget(IntPtr handle,
        RoutingId? routerRoutingId)
    {
        ZlinkRoutingId nativeRoutingId = default;
        var routingIdPointer = IntPtr.Zero;
        if (routerRoutingId.HasValue)
        {
            nativeRoutingId = routerRoutingId.Value.ToNative();
            routingIdPointer = (IntPtr)(&nativeRoutingId);
        }

        var rc = NativeMethods.zlink_select_routed_submit_target(handle,
            routingIdPointer, out var target);
        if (rc != (int)SubmitResult.Ok)
            throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        return target;
    }

    private static void SubmitParts(IntPtr handle, SocketType socketType,
        RoutingId? routerRoutingId, IReadOnlyList<Message> parts,
        uint timeoutMs, int flags, IntPtr userData)
    {
        if (socketType == SocketType.Dealer)
        {
            SubmitDealerParts(handle, parts, timeoutMs, flags, userData);
            return;
        }

        var routedTarget = SelectRouterTarget(handle, routerRoutingId);
        RequestReplySupport.SubmitOwnedParts(parts,
            (ref ZlinkMsg nativePart, NativeMethods.ZlinkPartFlag partFlag) =>
            {
                var final = partFlag == NativeMethods.ZlinkPartFlag.Final;
                var partTimeout = final ? timeoutMs : 0;
                var partHandler = final ? ReplyHandlerPointer : IntPtr.Zero;
                var partUserData = final ? userData : IntPtr.Zero;
                return NativeMethods.zlink_router_request_transport_pair_part(
                    handle, ref routedTarget.PeerRoutingId,
                    routedTarget.TransportPairId,
                    routedTarget.TransportPairGeneration, ref nativePart,
                    flags, partFlag, partTimeout, partHandler,
                    partUserData);
            });
    }

    private static void SubmitDealerParts(IntPtr handle,
        IReadOnlyList<Message> parts, uint timeoutMs, int flags,
        IntPtr userData)
    {
        RequestReplySupport.SubmitOwnedParts(parts,
            (ref ZlinkMsg nativePart, NativeMethods.ZlinkPartFlag partFlag) =>
            {
                var final = partFlag == NativeMethods.ZlinkPartFlag.Final;
                return NativeMethods.zlink_dealer_request_part(handle,
                    ref nativePart, flags, partFlag,
                    final ? timeoutMs : 0,
                    final ? ReplyHandlerPointer : IntPtr.Zero,
                    final ? userData : IntPtr.Zero);
            });
    }

    private static void OnNativeReply(int result, IntPtr parts,
        nuint partCount, IntPtr userData)
    {
        if (userData == IntPtr.Zero)
        {
            if (parts != IntPtr.Zero)
                NativeMethods.zlink_multipart_close(parts, partCount);
            return;
        }

        var state = GCHandle.FromIntPtr(userData);
        try
        {
            if (state.Target is IRequestCompletion pending)
                pending.Complete(result, ref parts, ref partCount);
        }
        catch (Exception exception)
        {
            CallbackExceptionHub.Report(exception);
        }
        finally
        {
            if (parts != IntPtr.Zero)
                NativeMethods.zlink_multipart_close(parts, partCount);
            state.Free();
        }
    }

    private interface IRequestCompletion
    {
        void Complete(int result, ref IntPtr parts, ref nuint partCount);
    }

    private sealed class BlockingRequest : IRequestCompletion
    {
        private readonly TaskCompletionSource<IReadOnlyList<Message>>
            _completed = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Complete(int result, ref IntPtr parts, ref nuint partCount)
        {
            if (result != 0)
            {
                _completed.TrySetException(new ZlinkRequestException(
                    (RequestResult)result));
                return;
            }
            var reply = Message.FromNativeVector(parts, partCount);
            parts = IntPtr.Zero;
            partCount = 0;
            _completed.TrySetResult(reply);
        }

        internal IReadOnlyList<Message> Wait()
        {
            return _completed.Task.GetAwaiter().GetResult();
        }
    }

    private sealed class CallbackRequest : IRequestCompletion
    {
        private readonly RequestCallback _callback;

        internal CallbackRequest(RequestCallback callback)
        {
            _callback = callback;
        }

        public void Complete(int result, ref IntPtr parts, ref nuint partCount)
        {
            IReadOnlyList<Message> reply = Array.Empty<Message>();
            if (result == 0)
            {
                reply = Message.FromNativeVector(parts, partCount);
                parts = IntPtr.Zero;
                partCount = 0;
            }
            _callback((RequestResult)result, reply);
        }
    }

    private sealed class PendingRequest : IRequestCompletion
    {
        private readonly CancellationToken _cancellationToken;

        private readonly TaskCompletionSource<IReadOnlyList<Message>>
            _completion = new(
                TaskCreationOptions.RunContinuationsAsynchronously);

        private CancellationTokenRegistration _registration;
        private int _completed;

        internal PendingRequest(CancellationToken cancellationToken)
        {
            _cancellationToken = cancellationToken;
        }

        internal Task<IReadOnlyList<Message>> Task => _completion.Task;

        internal void AttachCancellation()
        {
            if (!_cancellationToken.CanBeCanceled
                || Volatile.Read(ref _completed) != 0)
                return;

            _registration = _cancellationToken.Register(static state =>
            {
                var pending = (PendingRequest)state!;
                if (Interlocked.CompareExchange(ref pending._completed, 1, 0)
                    == 0)
                    pending._completion.TrySetCanceled(
                        pending._cancellationToken);
            }, this);
        }

        public void Complete(int result, ref IntPtr parts,
            ref nuint partCount)
        {
            if (result != 0)
            {
                if (Interlocked.CompareExchange(ref _completed, 1, 0) != 0)
                    return;
                _registration.Unregister();
                _completion.TrySetException(new ZlinkRequestException(
                    (RequestResult)result));
                return;
            }

            var messages = Message.FromNativeVector(parts, partCount);
            parts = IntPtr.Zero;
            partCount = 0;
            if (Interlocked.CompareExchange(ref _completed, 1, 0) != 0)
            {
                RequestReplySupport.DisposeParts(messages);
                return;
            }

            _registration.Unregister();
            _completion.TrySetResult(messages);
        }
    }
}
