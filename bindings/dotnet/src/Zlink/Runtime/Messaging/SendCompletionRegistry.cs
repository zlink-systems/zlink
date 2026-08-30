// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

/// <summary>
///     Binds one socket's asynchronous send terminal to the Core 0.13.2
///     send-completion contract (<c>zlink_send_async</c> /
///     <c>zlink_send_complete_handler</c> / <c>zlink_send_async_cancel</c>).
/// </summary>
/// <remarks>
///     <para>
///         This type owns no thread, no queue, no timer and no retry policy. It
///         hands one complete record to Core and lets the Core completion drive
///         the <see cref="TaskCompletionSource" />. The completion callback only
///         moves the outcome into managed state; it never submits.
///     </para>
///     <para>
///         Delegate lifetime: Core stores the raw function pointer produced for
///         <see cref="_nativeHandler" />. The managed delegate instance is
///         rooted both by this instance field and by an explicit
///         <see cref="GCHandle" /> that is released only after the native socket
///         has been closed. Letting it be collected while the socket is alive
///         would leave Core calling into a freed reverse-P/Invoke stub.
///     </para>
/// </remarks>
internal sealed class SendCompletionRegistry
{
    private const int ECanceled = 125;
    private const int EShutdown = 108;
    private const int ETerm = 156384765;

    private readonly IntPtr _handle;
    private readonly SocketType _socketType;
    private readonly object _handlerInstallSync = new();

    // Rooted for the socket lifetime; see the remarks above.
    private readonly NativeMethods.ZlinkSendCompleteHandlerDelegate
        _nativeHandler;

    private GCHandle _nativeHandlerRoot;
    private bool _handlerInstalled;
    private bool _closed;

    internal SendCompletionRegistry(IntPtr handle, SocketType socketType)
    {
        _handle = handle;
        _socketType = socketType;
        _nativeHandler = OnNativeSendComplete;
        _nativeHandlerRoot = GCHandle.Alloc(_nativeHandler,
            GCHandleType.Normal);
    }

    /// <summary>
    ///     Submits one complete record for asynchronous admission. Immediate
    ///     admission completes locally; only a Core-pending operation is
    ///     completed by the Core completion callback.
    /// </summary>
    internal unsafe Task SendAsync(RoutingId? routerRoutingId,
        IReadOnlyList<Message> parts, CancellationToken cancellationToken,
        ulong transportPairId = 0, ulong transportPairGeneration = 0)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        if (_socketType == SocketType.Stream && parts.Count != 1)
            throw new ArgumentException(
                "STREAM records carry exactly one FINAL message; submit frames separately.",
                nameof(parts));
        if (cancellationToken.IsCancellationRequested)
            return Task.FromCanceled(cancellationToken);

        EnsureHandlerInstalled();

        // ROUTER and STREAM address one exact peer, so the target is resolved
        // before submit. DEALER passes NULL and Core commits one selection.
        var hasTarget = _socketType is SocketType.Router or SocketType.Stream;
        var target = hasTarget
            ? SelectTarget(
                routerRoutingId,
                transportPairId,
                transportPairGeneration)
            : default;

        Message[]? copied = null;
        var source = NativeMessageParts.AsSpan(parts, ref copied);
        ZlinkMsg[]? rentedNative = null;
        var native = source.Length <= NativeMessageParts.StackPartLimit
            ? stackalloc ZlinkMsg[source.Length]
            : rentedNative = ArrayPool<ZlinkMsg>.Shared.Rent(source.Length);
        native = native[..source.Length];
        var built = 0;
        try
        {
            try
            {
                NativeMessageParts.MoveToNative(source, native, nameof(parts),
                    ref built);
            }
            catch
            {
                NativeMessageParts.RestoreManaged(source, native, 0, built);
                throw;
            }

            int rc;
            ulong opId;
            PendingSend? pending = null;
            GCHandle operationRoot = default;
            try
            {
                pending = new PendingSend(cancellationToken);
                operationRoot = GCHandle.Alloc(pending, GCHandleType.Normal);
                fixed (ZlinkMsg* nativeParts = native)
                {
                    var options = new ZlinkSendAsyncOptions
                    {
                        StructSize = (uint)sizeof(ZlinkSendAsyncOptions),
                        TimeoutMs = 0,
                        Userdata = GCHandle.ToIntPtr(operationRoot),
                        Target = hasTarget ? &target : null
                    };
                    rc = NativeMethods.zlink_send_async(_handle, nativeParts,
                        (nuint)native.Length, &options, out opId);
                }
            }
            catch
            {
                if (operationRoot.IsAllocated)
                    operationRoot.Free();
                NativeMessageParts.RestoreManaged(source, native, 0, built);
                throw;
            }

            if (rc != (int)SubmitResult.Ok)
            {
                operationRoot.Free();
                NativeMessageParts.RestoreManaged(source, native, 0, built);
                throw ZlinkException.CreateSubmitException((SubmitResult)rc);
            }
            if (opId == 0)
            {
                operationRoot.Free();
                return Task.CompletedTask;
            }
            pending!.AttachCancellation(this, opId);
            return pending.Task;
        }
        finally
        {
            if (rentedNative != null)
                ArrayPool<ZlinkMsg>.Shared.Return(rentedNative);
        }
    }

    // Single-part hot path: use one stack native handle. The managed pending
    // state is passed to Core as userdata so an inline completion can resolve
    // it even before zlink_send_async returns.
    internal unsafe Task SendSingleAsync(RoutingId? routerRoutingId,
        Message part, CancellationToken cancellationToken,
        ulong transportPairId = 0, ulong transportPairGeneration = 0)
    {
        if (cancellationToken.IsCancellationRequested)
            return Task.FromCanceled(cancellationToken);
        EnsureHandlerInstalled();
        var hasTarget = _socketType is SocketType.Router or SocketType.Stream;
        var target = default(ZlinkRoutedSubmitTarget);
        if (hasTarget)
        {
            if (!routerRoutingId.HasValue)
                throw new ArgumentNullException(nameof(routerRoutingId));
            target.PeerRoutingId = routerRoutingId.Value.ToNative();
            if ((transportPairId == 0) != (transportPairGeneration == 0))
                throw new ArgumentException(
                    "A routed transport pair requires both id and generation.");
            target.TransportPairId = transportPairId;
            target.TransportPairGeneration = transportPairGeneration;
        }
        ZlinkMsg native = default;
        part.MoveTo(ref native);

        int rc;
        ulong opId;
        var pending = new PendingSend(cancellationToken);
        var operationRoot = GCHandle.Alloc(pending, GCHandleType.Normal);
        try
        {
            var options = new ZlinkSendAsyncOptions
            {
                StructSize = (uint)sizeof(ZlinkSendAsyncOptions),
                TimeoutMs = 0,
                Userdata = GCHandle.ToIntPtr(operationRoot),
                Target = hasTarget ? &target : null
            };
            rc = NativeMethods.zlink_send_async(_handle, &native, 1,
                &options, out opId);
        }
        catch
        {
            operationRoot.Free();
            part.RestoreFrom(ref native);
            throw;
        }
        if (rc != (int)SubmitResult.Ok)
        {
            operationRoot.Free();
            part.RestoreFrom(ref native);
            throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        }

        if (opId == 0)
        {
            operationRoot.Free();
            return Task.CompletedTask;
        }
        pending.AttachCancellation(this, opId);
        return pending.Task;
    }

    /// <summary>
    ///     Blocks further cancel entry points once the socket starts closing.
    ///     Core delivers a terminal completion for every operation still
    ///     pending, so no managed completion is lost here.
    /// </summary>
    internal void BeginClose()
    {
        Volatile.Write(ref _closed, true);
    }

    /// <summary>
    ///     Releases the completion delegate root. Only safe after the native
    ///     socket handle has been closed.
    /// </summary>
    internal void ReleaseAfterNativeClose()
    {
        Volatile.Write(ref _closed, true);
        if (_nativeHandlerRoot.IsAllocated)
            _nativeHandlerRoot.Free();
    }

    internal void EnsureHandlerInstalled()
    {
        if (Volatile.Read(ref _handlerInstalled))
            return;
        lock (_handlerInstallSync)
        {
            if (_handlerInstalled)
                return;
            if (Volatile.Read(ref _closed))
                throw new ZlinkSubmitException(
                    ZlinkSubmitException.ErrorCode.Terminated);

            var rc = NativeMethods.zlink_send_complete_handler(_handle,
                _nativeHandler, IntPtr.Zero);
            if (rc != 0)
                ZlinkException.ThrowHandlerIfError(rc);
            Volatile.Write(ref _handlerInstalled, true);
        }
    }

    private void RequestCancel(ulong opId)
    {
        if (opId == 0)
            return;
        if (Volatile.Read(ref _closed))
            return;
        // NOT_FOUND / INVALID_STATE are both benign: the operation either
        // already completed or is past the cancellable point, and either
        // way it still completes exactly once.
        _ = NativeMethods.zlink_send_async_cancel(_handle, opId);
    }

    private unsafe ZlinkRoutedSubmitTarget SelectTarget(
        RoutingId? routerRoutingId,
        ulong transportPairId = 0,
        ulong transportPairGeneration = 0)
    {
        if ((transportPairId == 0) != (transportPairGeneration == 0))
            throw new ArgumentException(
                "A routed transport pair requires both id and generation.");
        ZlinkRoutingId nativeRoutingId = default;
        var routingIdPointer = IntPtr.Zero;
        if (routerRoutingId.HasValue)
        {
            nativeRoutingId = routerRoutingId.Value.ToNative();
            routingIdPointer = (IntPtr)(&nativeRoutingId);
        }

        if (transportPairId == 0)
        {
            var rc = NativeMethods.zlink_select_routed_submit_target(_handle,
                routingIdPointer, out var selected);
            if (rc != (int)SubmitResult.Ok)
                throw ZlinkException.CreateSubmitException((SubmitResult)rc);
            return selected;
        }
        if (!routerRoutingId.HasValue)
            throw new ArgumentNullException(nameof(routerRoutingId));
        return new ZlinkRoutedSubmitTarget
        {
            PeerRoutingId = nativeRoutingId,
            TransportPairId = transportPairId,
            TransportPairGeneration = transportPairGeneration
        };
    }

    private unsafe void OnNativeSendComplete(IntPtr subject,
        IntPtr completeEvent, IntPtr userData)
    {
        if (completeEvent == IntPtr.Zero)
            return;

        var completion = (ZlinkSendCompleteEvent*)completeEvent;
        var opId = completion->OpId;
        var result = completion->Result;
        var terminalErrno = completion->TerminalErrno;
        if (opId == 0)
            return;

        var operationRoot = GCHandle.FromIntPtr(completion->Userdata);
        try
        {
            if (operationRoot.Target is PendingSend pending)
                pending.Complete(result, terminalErrno);
        }
        catch (Exception exception)
        {
            CallbackExceptionHub.Report(exception);
        }
        finally
        {
            operationRoot.Free();
        }
    }

    private static Exception MapTerminal(int terminalErrno)
    {
        var result = terminalErrno switch
        {
            ECanceled or EShutdown or ETerm or 10058 => SubmitResult.Terminated,
            2 or 3 or 113 or 10065 => SubmitResult.NotFound,
            _ => SubmitResult.NotConnected
        };
        return new ZlinkSubmitException(result, terminalErrno);
    }

    /// <summary>
    ///     One in-flight asynchronous send. Core returns this state unchanged in
    ///     the completion event's userdata field.
    /// </summary>
    private sealed class PendingSend
    {
        private readonly CancellationToken _cancellationToken;

        // Core reports immediate admission as opId == 0 and never calls the
        // completion handler for it. Keep the completion lazy so that common
        // path pays only for the userdata anchor, not for a Task that is never
        // observed. A pending completion can race SendAsync returning, so both
        // the callback and Task getter publish through the same atomic slot.
        private TaskCompletionSource? _completion;

        private CancellationTokenRegistration _registration;
        private int _completionStarted;

        internal PendingSend(CancellationToken cancellationToken)
        {
            _cancellationToken = cancellationToken;
        }

        internal Task Task => GetOrCreateCompletion().Task;
        internal bool IsCompleted => Volatile.Read(ref _completionStarted) != 0;

        internal void AttachCancellation(SendCompletionRegistry owner, ulong opId)
        {
            if (!_cancellationToken.CanBeCanceled || IsCompleted)
                return;
            _registration = _cancellationToken.Register(static state =>
            {
                var pair = ((SendCompletionRegistry Owner, ulong OpId))state!;
                pair.Owner.RequestCancel(pair.OpId);
            }, (owner, opId));

            // The completion may have raced the registration in.
            if (IsCompleted)
                _registration.Unregister();
        }

        internal void Complete(ZlinkSendCompleteResult result,
            int terminalErrno)
        {
            // Core invokes exactly one callback for every nonzero operation id.
            // This marker only coordinates callback completion with cancellation
            // registration that may still be returning from SendAsync.
            Volatile.Write(ref _completionStarted, 1);

            // Unregister, never Dispose: Core can dispatch this completion
            // inline on the thread running the cancellation callback, and a
            // disposing wait would deadlock against itself there.
            _registration.Unregister();

            var completion = GetOrCreateCompletion();

            switch (result)
            {
                case ZlinkSendCompleteResult.Admitted:
                    completion.TrySetResult();
                    return;
                case ZlinkSendCompleteResult.TimedOut:
                    completion.TrySetException(new ZlinkSubmitException(
                        SubmitResult.Backpressured,
                        (int)ErrorCode.ETimedOut));
                    return;
                default:
                    if (terminalErrno == ECanceled
                        && _cancellationToken.IsCancellationRequested)
                        completion.TrySetCanceled(_cancellationToken);
                    else
                        completion.TrySetException(
                            MapTerminal(terminalErrno));
                    return;
            }
        }

        private TaskCompletionSource GetOrCreateCompletion()
        {
            var completion = Volatile.Read(ref _completion);
            if (completion != null)
                return completion;

            var created = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            return Interlocked.CompareExchange(ref _completion, created, null)
                   ?? created;
        }
    }
}
