// SPDX-License-Identifier: MPL-2.0

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
    private readonly object _sync = new();
    private readonly Dictionary<ulong, PendingSend> _pending = new();
    private readonly Dictionary<ulong, EarlyCompletion> _early = new();

    // Core refuses `zlink_send_async` with EINVAL while a multipart part
    // sequence is active on the same native handle, so the record submit
    // shares the socket's short submit gate with the request and reply part
    // loops. The gate is never held across a wait: `zlink_send_async` hands
    // the record over and returns.
    private readonly object _submitGate;

    // Rooted for the socket lifetime; see the remarks above.
    private readonly NativeMethods.ZlinkSendCompleteHandlerDelegate
        _nativeHandler;

    private GCHandle _nativeHandlerRoot;
    private bool _handlerInstalled;
    private bool _closed;

    internal SendCompletionRegistry(IntPtr handle, SocketType socketType,
        object submitGate)
    {
        _handle = handle;
        _socketType = socketType;
        _submitGate = submitGate
            ?? throw new ArgumentNullException(nameof(submitGate));
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
        IReadOnlyList<Message> parts, CancellationToken cancellationToken)
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
            ? SelectTarget(routerRoutingId)
            : default;

        Message[]? copied = null;
        var source = NativeMessageParts.AsSpan(parts, ref copied);
        var native = new ZlinkMsg[source.Length];
        var built = 0;
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
        try
        {
            fixed (ZlinkMsg* nativeParts = native)
            {
                var options = new ZlinkSendAsyncOptions
                {
                    StructSize = (uint)sizeof(ZlinkSendAsyncOptions),
                    // The per-operation deadline is a Core-side option. The
                    // binding owns no deadline timer of its own.
                    TimeoutMs = 0,
                    Userdata = IntPtr.Zero,
                    Target = hasTarget ? &target : null
                };
                lock (_submitGate)
                {
                    rc = NativeMethods.zlink_send_async(_handle, nativeParts,
                        (nuint)native.Length, &options, out opId);
                }
            }
        }
        catch
        {
            NativeMessageParts.RestoreManaged(source, native, 0, built);
            throw;
        }

        if (rc != (int)SubmitResult.Ok)
        {
            // No completion runs unless the submit returned OK, so the record
            // and its GC root are still owned here.
            NativeMessageParts.RestoreManaged(source, native, 0, built);
            throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        }

        // Ownership of every part has moved to Core. opId == 0 is the
        // immediate-admission disposition and deliberately has no callback.
        if (opId == 0)
            return Task.CompletedTask;

        return RegisterPending(opId, cancellationToken);
    }

    // Single-part hot path: use one stack native handle and create managed
    // pending state only when Core returns a non-zero operation id.
    internal unsafe Task SendSingleAsync(RoutingId? routerRoutingId,
        Message part, CancellationToken cancellationToken)
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
        }
        ZlinkMsg native = default;
        part.MoveTo(ref native);

        int rc;
        ulong opId;
        try
        {
            var options = new ZlinkSendAsyncOptions
            {
                StructSize = (uint)sizeof(ZlinkSendAsyncOptions),
                TimeoutMs = 0,
                Userdata = IntPtr.Zero,
                Target = hasTarget ? &target : null
            };
            lock (_submitGate)
            {
                rc = NativeMethods.zlink_send_async(_handle, &native, 1,
                    &options, out opId);
            }
        }
        catch
        {
            part.RestoreFrom(ref native);
            throw;
        }
        if (rc != (int)SubmitResult.Ok)
        {
            part.RestoreFrom(ref native);
            throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        }

        if (opId == 0)
            return Task.CompletedTask;
        return RegisterPending(opId, cancellationToken);
    }

    /// <summary>
    ///     Blocks further cancel entry points once the socket starts closing.
    ///     Core delivers a terminal completion for every operation still
    ///     pending, so no managed completion is lost here.
    /// </summary>
    internal void BeginClose()
    {
        lock (_sync)
            _closed = true;
    }

    /// <summary>
    ///     Releases the completion delegate root. Only safe after the native
    ///     socket handle has been closed.
    /// </summary>
    internal void ReleaseAfterNativeClose()
    {
        lock (_sync)
        {
            _closed = true;
            if (_nativeHandlerRoot.IsAllocated)
                _nativeHandlerRoot.Free();
        }
    }

    internal void EnsureHandlerInstalled()
    {
        if (Volatile.Read(ref _handlerInstalled))
            return;
        lock (_sync)
        {
            if (_handlerInstalled)
                return;
            if (_closed)
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
        lock (_sync)
        {
            if (_closed)
                return;
            // NOT_FOUND / INVALID_STATE are both benign: the operation either
            // already completed or is past the cancellable point, and either
            // way it still completes exactly once.
            _ = NativeMethods.zlink_send_async_cancel(_handle, opId);
        }
    }

    private unsafe ZlinkRoutedSubmitTarget SelectTarget(
        RoutingId? routerRoutingId)
    {
        ZlinkRoutingId nativeRoutingId = default;
        var routingIdPointer = IntPtr.Zero;
        if (routerRoutingId.HasValue)
        {
            nativeRoutingId = routerRoutingId.Value.ToNative();
            routingIdPointer = (IntPtr)(&nativeRoutingId);
        }

        var rc = NativeMethods.zlink_select_routed_submit_target(_handle,
            routingIdPointer, out var target);
        if (rc != (int)SubmitResult.Ok)
            throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        return target;
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

        PendingSend? pending = null;
        try
        {
            lock (_sync)
            {
                if (_pending.Remove(opId, out pending))
                {
                    // Complete outside the registry lock.
                }
                else
                {
                    _early[opId] = new EarlyCompletion(result,
                        terminalErrno);
                }
            }
            pending?.Complete(result, terminalErrno);
        }
        catch (Exception exception)
        {
            CallbackExceptionHub.Report(exception);
        }
    }

    private Task RegisterPending(ulong opId,
        CancellationToken cancellationToken)
    {
        var pending = new PendingSend(cancellationToken) { OpId = opId };
        EarlyCompletion early = default;
        bool completedEarly;
        lock (_sync)
        {
            completedEarly = _early.Remove(opId, out early);
            if (!completedEarly)
                _pending.Add(opId, pending);
        }

        if (completedEarly)
            pending.Complete(early.Result, early.TerminalErrno);
        else if (cancellationToken.CanBeCanceled && !pending.IsCompleted)
            pending.AttachCancellation(this);

        var task = pending.Task;
        return task.IsCompletedSuccessfully ? Task.CompletedTask : task;
    }

    private readonly record struct EarlyCompletion(
        ZlinkSendCompleteResult Result, int TerminalErrno);

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
    ///     One in-flight asynchronous send. The socket registry owns the state
    ///     by operation id until its exactly-once completion arrives.
    /// </summary>
    private sealed class PendingSend
    {
        private readonly CancellationToken _cancellationToken;

        private readonly TaskCompletionSource _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        private CancellationTokenRegistration _registration;
        private int _completed;

        internal PendingSend(CancellationToken cancellationToken)
        {
            _cancellationToken = cancellationToken;
        }

        internal Task Task => _completion.Task;
        internal ulong OpId;
        internal bool IsCompleted => Volatile.Read(ref _completed) != 0;

        internal void AttachCancellation(SendCompletionRegistry owner)
        {
            var opId = OpId;
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
            if (Interlocked.CompareExchange(ref _completed, 1, 0) != 0)
                return;

            // Unregister, never Dispose: Core can dispatch this completion
            // inline on the thread running the cancellation callback, and a
            // disposing wait would deadlock against itself there.
            _registration.Unregister();

            switch (result)
            {
                case ZlinkSendCompleteResult.Admitted:
                    _completion.TrySetResult();
                    return;
                case ZlinkSendCompleteResult.TimedOut:
                    _completion.TrySetException(new ZlinkSubmitException(
                        SubmitResult.Backpressured,
                        (int)ErrorCode.ETimedOut));
                    return;
                default:
                    if (terminalErrno == ECanceled
                        && _cancellationToken.IsCancellationRequested)
                        _completion.TrySetCanceled(_cancellationToken);
                    else
                        _completion.TrySetException(
                            MapTerminal(terminalErrno));
                    return;
            }
        }
    }
}
