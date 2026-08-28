// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

/// <summary>
///     Submits one DEALER/ROUTER request to an exact Core target on the calling
///     thread and lets the Core reply callback complete the task.
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
            var target = SelectTarget(handle, routerRoutingId);
            SubmitParts(handle, socketType, ref target, parts, timeoutMs,
                userData);
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

    private static unsafe ZlinkRoutedSubmitTarget SelectTarget(IntPtr handle,
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
        ref ZlinkRoutedSubmitTarget target, IReadOnlyList<Message> parts,
        uint timeoutMs, IntPtr userData)
    {
        // DONTWAIT: the .NET terminal is `Async(...)`, so the submit must not
        // occupy the caller thread waiting for HWM credit. Core reports the
        // refusal immediately and the back-pressure policy belongs to the
        // application — the binding neither waits nor retries.
        const int blockingFlags = 1;
        var routedTarget = target;
        RequestReplySupport.SubmitOwnedParts(parts,
            (ref ZlinkMsg nativePart, NativeMethods.ZlinkPartFlag partFlag) =>
            {
                var final = partFlag == NativeMethods.ZlinkPartFlag.Final;
                var partTimeout = final ? timeoutMs : 0;
                var partHandler = final ? ReplyHandlerPointer : IntPtr.Zero;
                var partUserData = final ? userData : IntPtr.Zero;
                return socketType == SocketType.Dealer
                    ? NativeMethods.zlink_dealer_request_transport_pair_part(
                        handle, ref routedTarget, ref nativePart,
                        blockingFlags, partFlag, partTimeout, partHandler,
                        partUserData)
                    : NativeMethods.zlink_router_request_transport_pair_part(
                        handle, ref routedTarget.PeerRoutingId,
                        routedTarget.TransportPairId,
                        routedTarget.TransportPairGeneration, ref nativePart,
                        blockingFlags, partFlag, partTimeout, partHandler,
                        partUserData);
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
            if (state.Target is PendingRequest pending)
                pending.CompleteNativeReply(result, ref parts, ref partCount);
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

    private sealed class PendingRequest
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

        internal void CompleteNativeReply(int result, ref IntPtr parts,
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
