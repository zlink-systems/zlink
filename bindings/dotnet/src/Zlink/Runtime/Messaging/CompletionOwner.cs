// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

/// <summary>
///     Owns one socket's native completion drain and provisional operation
///     registry. Raw completion records never escape this type.
/// </summary>
internal sealed class CompletionOwner
{
    private const int DontWait = 1;
    private readonly IntPtr _handle;
    private readonly SocketType _socketType;
    private readonly object _sync = new();
    private readonly object _drainSync = new();
    private readonly object _submitSync = new();
    private readonly Dictionary<IntPtr, CompletionEntry> _entries = new();

    private object? _publicOwner;
    private IntPtr _runtimePoller;
    private Thread? _runtimeThread;
    private bool _runtimeStop;
    private bool _closing;

    internal CompletionOwner(IntPtr handle, SocketType socketType)
    {
        _handle = handle;
        _socketType = socketType;
    }

    internal Task SendAsync(RoutingId? target,
        IReadOnlyList<Message> parts, CancellationToken cancellationToken)
    {
        lock (_submitSync)
        {
            RequestReplySupport.EnsureParts(parts, nameof(parts));
            if (cancellationToken.IsCancellationRequested)
                return Task.FromCanceled(cancellationToken);
            EnsureOpenForSubmit();
            if (_socketType == SocketType.Stream && parts.Count != 1)
                throw new ArgumentException(
                    "STREAM sends contain exactly one message part.", nameof(parts));

            var entry = new SendCompletionEntry(cancellationToken);
            var userContext = Register(entry);
            try
            {
                var completionId = SubmitSend(target, parts, DontWait,
                    userContext);
                entry.Publish(completionId);
                if (completionId == 0)
                    entry.CaptureImmediateSend();
                return entry.Task;
            }
            catch (Exception exception)
            {
                entry.FailSubmit(exception);
                throw;
            }
        }
    }

    internal void Send(RoutingId? target, IReadOnlyList<Message> parts)
    {
        lock (_submitSync)
        {
            EnsureOpenForSubmit();
            RequestReplySupport.EnsureParts(parts, nameof(parts));
            if (_socketType == SocketType.Stream && parts.Count != 1)
                throw new ArgumentException(
                    "STREAM sends contain exactly one message part.", nameof(parts));
            _ = SubmitSend(target, parts, 0, IntPtr.Zero);
        }
    }

    internal Task<IReadOnlyList<Message>> RequestAsync(RoutingId? target,
        IReadOnlyList<Message> parts, uint timeoutMs,
        CancellationToken cancellationToken)
    {
        lock (_submitSync)
        {
            RequestReplySupport.EnsureParts(parts, nameof(parts));
            if (cancellationToken.IsCancellationRequested)
                return Task.FromCanceled<IReadOnlyList<Message>>(
                    cancellationToken);

            var entry = new RequestCompletionEntry(cancellationToken);
            var userContext = Register(entry);
            try
            {
                var completionId = SubmitRequest(target, parts, timeoutMs,
                    DontWait, userContext);
                entry.Publish(completionId);
                return entry.Task;
            }
            catch (Exception exception)
            {
                entry.FailSubmit(exception);
                throw;
            }
        }
    }

    internal IReadOnlyList<Message> Request(RoutingId? target,
        IReadOnlyList<Message> parts, uint timeoutMs)
    {
        RequestCompletionEntry entry;
        lock (_submitSync)
        {
            RequestReplySupport.EnsureParts(parts, nameof(parts));
            entry = new RequestCompletionEntry(CancellationToken.None);
            var userContext = Register(entry);
            try
            {
                var completionId = SubmitRequest(target, parts, timeoutMs, 0,
                    userContext);
                entry.Publish(completionId);
            }
            catch (Exception exception)
            {
                entry.FailSubmit(exception);
                throw;
            }
        }
        return entry.Task.GetAwaiter().GetResult();
    }

    internal void Reply(RoutingId target, ReplyToken replyToken,
        IReadOnlyList<Message> parts)
    {
        lock (_submitSync)
        {
            EnsureOpenForSubmit();
            var nativeTarget = target.ToNative();
            RequestReplySupport.SubmitPreservingOnFailure(parts,
                (ref ZlinkMsg nativePart,
                    NativeMethods.ZlinkPartFlag partFlag) =>
                    NativeMethods.zlink_reply_part(_handle,
                        ref nativeTarget, replyToken.Value, ref nativePart,
                        partFlag));
        }
    }

    internal void TransferToPublic(object pollerOwner)
    {
        Thread? thread;
        IntPtr poller;
        lock (_sync)
        {
            if (_closing)
                throw new ZlinkConfigException(ConfigResult.InvalidState);
            if (_publicOwner is not null
                && !ReferenceEquals(_publicOwner, pollerOwner))
                throw new ZlinkConfigException(ConfigResult.InvalidState,
                    (int)ErrorCode.EBusy);
            if (ReferenceEquals(_publicOwner, pollerOwner))
                return;

            _publicOwner = pollerOwner;
            _runtimeStop = true;
            thread = _runtimeThread;
            poller = _runtimePoller;
            _runtimeThread = null;
            _runtimePoller = IntPtr.Zero;
        }
        StopRuntime(thread, poller);
    }

    internal void TransferToRuntime(object pollerOwner)
    {
        lock (_sync)
        {
            if (!ReferenceEquals(_publicOwner, pollerOwner))
                return;
            _publicOwner = null;
            if (!_closing && _entries.Count != 0)
                StartRuntimeLocked();
        }
    }

    internal int Drain(bool waitForPublish)
    {
        lock (_drainSync)
        {
            var processed = 0;
            while (true)
            {
                ZlinkCompletion completion = default;
                completion.StructSize = checked((uint)Marshal.SizeOf<ZlinkCompletion>());
                var rc = NativeMethods.zlink_completion_recv(_handle,
                    ref completion, DontWait);
                if ((RecvResult)rc == RecvResult.NoData)
                    return processed;
                if ((RecvResult)rc != RecvResult.Ok)
                    throw ZlinkException.CreateRecvException((RecvResult)rc);

                CompletionEntry? entry;
                lock (_sync)
                    _entries.TryGetValue(completion.UserContext, out entry);
                try
                {
                    entry?.Capture(ref completion);
                }
                finally
                {
                    NativeMethods.zlink_completion_close(ref completion);
                }
                if (waitForPublish)
                    entry?.WaitJoined();
                processed++;
            }
        }
    }

    internal void PrepareClose()
    {
        Monitor.Enter(_submitSync);
        Thread? thread;
        IntPtr poller;
        lock (_sync)
        {
            _closing = true;
            _runtimeStop = true;
            thread = _runtimeThread;
            poller = _runtimePoller;
            _runtimeThread = null;
            _runtimePoller = IntPtr.Zero;
        }
        StopRuntime(thread, poller);
    }

    internal void CancelClose()
    {
        try
        {
            lock (_sync)
            {
                _closing = false;
                if (_publicOwner is null && _entries.Count != 0)
                    StartRuntimeLocked();
            }
        }
        finally
        {
            Monitor.Exit(_submitSync);
        }
    }

    internal void CompleteClose()
    {
        try
        {
            CompletionEntry[] entries;
            lock (_sync)
            {
                _closing = true;
                entries = _entries.Values.ToArray();
            }
            foreach (var entry in entries)
                entry.FailLifecycle();
        }
        finally
        {
            Monitor.Exit(_submitSync);
        }
    }

    private IntPtr Register(CompletionEntry entry)
    {
        lock (_sync)
        {
            if (_closing)
                throw new ZlinkSubmitException(SubmitResult.InvalidState,
                    (int)ErrorCode.EShutdown);
            var root = GCHandle.Alloc(entry, GCHandleType.Normal);
            var context = GCHandle.ToIntPtr(root);
            entry.Attach(this, context);
            _entries.Add(context, entry);
            if (_publicOwner is null)
            {
                try
                {
                    StartRuntimeLocked();
                }
                catch
                {
                    _entries.Remove(context);
                    root.Free();
                    entry.DetachAfterRegistrationFailure();
                    throw;
                }
            }
            entry.EnableCancellation();
            return context;
        }
    }

    private void EnsureOpenForSubmit()
    {
        lock (_sync)
        {
            if (_closing)
                throw new ZlinkSubmitException(SubmitResult.Terminated,
                    (int)ErrorCode.EShutdown);
        }
    }

    private void Remove(CompletionEntry entry, IntPtr context)
    {
        lock (_sync)
        {
            if (!_entries.TryGetValue(context, out var current)
                || !ReferenceEquals(current, entry))
                return;
            _entries.Remove(context);
            GCHandle.FromIntPtr(context).Free();
        }
    }

    private unsafe ulong SubmitSend(RoutingId? target,
        IReadOnlyList<Message> parts, int flags, IntPtr userContext)
    {
        ZlinkRoutingId nativeTarget = default;
        var routed = target.HasValue;
        if (routed)
            nativeTarget = target!.Value.ToNative();
        ulong completionId = 0;
        var completionIdPointer = &completionId;
        RequestReplySupport.SubmitPreservingOnFailure(parts,
            (ref ZlinkMsg nativePart, NativeMethods.ZlinkPartFlag partFlag) =>
            {
                var final = partFlag == NativeMethods.ZlinkPartFlag.Final;
                var context = final ? userContext : IntPtr.Zero;
                var idOut = final && userContext != IntPtr.Zero
                    ? completionIdPointer : null;
                return routed
                    ? NativeMethods.zlink_send_part_rid(_handle,
                        ref nativeTarget, ref nativePart, flags, partFlag,
                        context, idOut)
                    : NativeMethods.zlink_send_part(_handle, ref nativePart,
                        flags, partFlag, context, idOut);
            });
        return completionId;
    }

    private unsafe ulong SubmitRequest(RoutingId? target,
        IReadOnlyList<Message> parts, uint timeoutMs, int flags,
        IntPtr userContext)
    {
        ZlinkRoutingId nativeTarget = default;
        ZlinkRoutingId* targetPointer = null;
        if (target.HasValue)
        {
            nativeTarget = target.Value.ToNative();
            targetPointer = &nativeTarget;
        }
        ulong completionId = 0;
        var completionIdPointer = &completionId;
        RequestReplySupport.SubmitPreservingOnFailure(parts,
            (ref ZlinkMsg nativePart, NativeMethods.ZlinkPartFlag partFlag) =>
            {
                var final = partFlag == NativeMethods.ZlinkPartFlag.Final;
                return NativeMethods.zlink_request_part(_handle,
                    targetPointer, ref nativePart, flags, partFlag,
                    final ? timeoutMs : 0,
                    final ? userContext : IntPtr.Zero,
                    final ? completionIdPointer : null);
            });
        if (completionId == 0)
            throw new ZlinkSubmitException(SubmitResult.InternalError,
                (int)ErrorCode.EProtoNoSupport);
        return completionId;
    }

    private void StartRuntimeLocked()
    {
        if (_runtimePoller != IntPtr.Zero || _runtimeThread is not null
            || _closing || _publicOwner is not null)
            return;
        var poller = NativeMethods.zlink_poller_new();
        if (poller == IntPtr.Zero)
            throw ZlinkException.CreateConfigException(
                NativeMethods.zlink_errno());
        var rc = NativeMethods.zlink_poller_add(poller, _handle,
            IntPtr.Zero, (short)PollEventFlags.PollCompletion);
        if (rc != 0)
        {
            _ = NativeMethods.zlink_poller_destroy(ref poller);
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
        }
        _runtimeStop = false;
        _runtimePoller = poller;
        _runtimeThread = new Thread(RuntimeLoop)
        {
            IsBackground = true,
            Name = "zlink-dotnet-completion"
        };
        _runtimeThread.Start();
    }

    private void RuntimeLoop()
    {
        var events = new ZlinkPollerEvent[1];
        while (true)
        {
            IntPtr poller;
            lock (_sync)
            {
                if (_runtimeStop || _closing)
                    return;
                poller = _runtimePoller;
            }
            var rc = NativeMethods.zlink_poller_wait(poller, events, 1, 25,
                out var error);
            if (rc > 0)
            {
                try
                {
                    _ = Drain(false);
                }
                catch
                {
                    return;
                }
            }
            else if (rc < 0 && (ConfigResult)error != ConfigResult.Ok)
            {
                return;
            }
        }
    }

    private static void StopRuntime(Thread? thread, IntPtr poller)
    {
        if (thread is not null && thread != Thread.CurrentThread)
            thread.Join();
        if (poller != IntPtr.Zero)
            _ = NativeMethods.zlink_poller_destroy(ref poller);
    }

    private abstract class CompletionEntry
    {
        private readonly object _sync = new();
        private readonly CancellationToken _cancellationToken;
        private readonly ManualResetEventSlim _joinedEvent = new(false);
        private CancellationTokenRegistration _cancellationRegistration;
        private CompletionOwner? _owner;
        private IntPtr _context;
        private bool _published;
        private bool _captured;
        private bool _joined;
        private bool _cancelClaimed;
        private bool _taskSettled;
        private Exception? _failure;

        protected CompletionEntry(CancellationToken cancellationToken)
        {
            _cancellationToken = cancellationToken;
        }

        internal void Attach(CompletionOwner owner, IntPtr context)
        {
            _owner = owner;
            _context = context;
        }

        internal void EnableCancellation()
        {
            if (!_cancellationToken.CanBeCanceled)
                return;
            _cancellationRegistration = _cancellationToken.Register(
                static state => ((CompletionEntry)state!).CancelWait(), this);
        }

        internal void DetachAfterRegistrationFailure()
        {
            _cancellationRegistration.Unregister();
            _owner = null;
            _context = IntPtr.Zero;
        }

        internal void Publish(ulong completionId)
        {
            _ = completionId;
            lock (_sync)
            {
                _published = true;
                if (_cancelClaimed && !_captured)
                    SetCanceledLocked();
                else if (_captured)
                    SetCapturedResultLocked();
                JoinIfReadyLocked();
            }
        }

        internal void CaptureImmediateSend()
        {
            lock (_sync)
            {
                if (_captured)
                    return;
                if (!_cancelClaimed)
                    CaptureImmediateLocked();
                _captured = true;
                if (_published && !_cancelClaimed)
                    SetCapturedResultLocked();
                JoinIfReadyLocked();
            }
        }

        internal void Capture(ref ZlinkCompletion completion)
        {
            lock (_sync)
            {
                if (_captured)
                    return;
                if (!_cancelClaimed)
                {
                    try
                    {
                        CaptureNativeLocked(ref completion);
                    }
                    catch (Exception exception)
                    {
                        _failure = exception;
                    }
                }
                _captured = true;
                if (_published && !_cancelClaimed)
                    SetCapturedResultLocked();
                JoinIfReadyLocked();
            }
        }

        internal void FailSubmit(Exception exception)
        {
            lock (_sync)
            {
                _failure = exception;
                _published = true;
                _captured = true;
                SetCapturedResultLocked();
                JoinIfReadyLocked();
            }
        }

        internal void FailLifecycle()
        {
            FailSubmit(CreateLifecycleFailure());
        }

        internal void WaitJoined()
        {
            _joinedEvent.Wait();
        }

        protected Exception? Failure
        {
            get => _failure;
            set => _failure = value;
        }

        protected abstract void CaptureImmediateLocked();
        protected abstract void CaptureNativeLocked(ref ZlinkCompletion completion);
        protected abstract void SetResultLocked();
        protected abstract void SetExceptionLocked(Exception exception);
        protected abstract void SetCanceledLocked(CancellationToken token);
        protected abstract Exception CreateLifecycleFailure();

        private void CancelWait()
        {
            lock (_sync)
            {
                if (_captured || _taskSettled)
                    return;
                _cancelClaimed = true;
                if (_published)
                    SetCanceledLocked();
            }
        }

        private void SetCanceledLocked()
        {
            if (_taskSettled)
                return;
            _taskSettled = true;
            _cancellationRegistration.Unregister();
            SetCanceledLocked(_cancellationToken);
        }

        private void SetCapturedResultLocked()
        {
            if (_taskSettled)
                return;
            _taskSettled = true;
            _cancellationRegistration.Unregister();
            if (_failure is not null)
                SetExceptionLocked(_failure);
            else
                SetResultLocked();
        }

        private void JoinIfReadyLocked()
        {
            if (_joined || !_published || !_captured)
                return;
            _joined = true;
            _joinedEvent.Set();
            _owner?.Remove(this, _context);
        }
    }

    private sealed class SendCompletionEntry : CompletionEntry
    {
        private readonly TaskCompletionSource _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        internal SendCompletionEntry(CancellationToken cancellationToken)
            : base(cancellationToken)
        {
        }

        internal Task Task => _completion.Task;

        protected override void CaptureImmediateLocked()
        {
        }

        protected override void CaptureNativeLocked(
            ref ZlinkCompletion completion)
        {
            if (completion.Kind != ZlinkCompletionKind.Send)
            {
                Failure = new ZlinkSubmitException(
                    SubmitResult.InternalError);
                return;
            }
            if (completion.SendResult != ZlinkSendCompleteResult.Admitted)
                Failure = completion.SendTerminalErrno != 0
                    ? ZlinkException.CreateSubmitException(
                        completion.SendTerminalErrno)
                    : new ZlinkSubmitException(SubmitResult.NotAdmitted);
        }

        protected override void SetResultLocked()
        {
            _completion.TrySetResult();
        }

        protected override void SetExceptionLocked(Exception exception)
        {
            _completion.TrySetException(exception);
        }

        protected override void SetCanceledLocked(CancellationToken token)
        {
            _completion.TrySetCanceled(token);
        }

        protected override Exception CreateLifecycleFailure() =>
            new ZlinkSubmitException(SubmitResult.Terminated,
                (int)ErrorCode.EShutdown);
    }

    private sealed class RequestCompletionEntry : CompletionEntry
    {
        private readonly TaskCompletionSource<IReadOnlyList<Message>>
            _completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private IReadOnlyList<Message> _reply = Array.Empty<Message>();

        internal RequestCompletionEntry(CancellationToken cancellationToken)
            : base(cancellationToken)
        {
        }

        internal Task<IReadOnlyList<Message>> Task => _completion.Task;

        protected override void CaptureImmediateLocked()
        {
            Failure = new ZlinkRequestException(RequestResult.InternalError);
        }

        protected override unsafe void CaptureNativeLocked(
            ref ZlinkCompletion completion)
        {
            if (completion.Kind != ZlinkCompletionKind.Request)
            {
                Failure = new ZlinkRequestException(
                    RequestResult.InternalError);
                return;
            }
            if (completion.RequestResult != RequestResult.Ok)
            {
                Failure = new ZlinkRequestException(
                    completion.RequestResult);
                return;
            }

            var count = checked((int)completion.ReplyPartCount);
            if (count == 0)
            {
                _reply = Array.Empty<Message>();
                return;
            }
            var result = new Message[count];
            var built = 0;
            try
            {
                for (var i = 0; i < count; i++)
                {
                    result[i] = Message.MoveFromNative(
                        ref completion.ReplyParts[i]);
                    built++;
                }
                _reply = result;
            }
            catch
            {
                for (var i = 0; i < built; i++)
                    result[i].Dispose();
                throw;
            }
        }

        protected override void SetResultLocked()
        {
            _completion.TrySetResult(_reply);
        }

        protected override void SetExceptionLocked(Exception exception)
        {
            _completion.TrySetException(exception);
        }

        protected override void SetCanceledLocked(CancellationToken token)
        {
            _completion.TrySetCanceled(token);
        }

        protected override Exception CreateLifecycleFailure() =>
            new ZlinkRequestException(RequestResult.Terminated);
    }
}
