// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

/// <summary>
///     Owns one socket's native completion drain and operation registry. Raw
///     completion records never escape this type.
/// </summary>
internal sealed class CompletionOwner
{
    private const int DontWait = 1;
    private readonly IntPtr _handle;
    private readonly SocketType _socketType;
    private readonly object _sync = new();
    private readonly object _drainSync = new();
    private readonly object _submitSync = new();
    private readonly object _runtimeSync = new();
    private readonly Dictionary<IntPtr, CompletionEntry> _entries = new();

    private object? _publicOwner;
    private IntPtr _runtimePoller;
    private long _runtimeEpoch;
    private bool _runtimePumpStarted;
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
            ValidateSend(parts);
            if (cancellationToken.IsCancellationRequested)
                return Task.FromCanceled(cancellationToken);
            EnsureOpenForSubmit();

            // Core never retains a back-pressured DONTWAIT payload. Keep an
            // operation-owned snapshot before asking Core to create a token.
            var retained = RequestReplySupport.CloneParts(parts);
            var entry = new SendCompletionEntry(target, retained,
                cancellationToken);
            try
            {
                Register(entry);
            }
            catch
            {
                RequestReplySupport.DisposeParts(retained);
                throw;
            }

            var attempt = SubmitSend(target, retained, DontWait,
                entry.Context);
            if (attempt.Failure is null)
            {
                try
                {
                    RequestReplySupport.ConsumeParts(parts);
                }
                catch (Exception exception)
                {
                    entry.CompleteInitialFailure(exception);
                    throw;
                }
                if (attempt.CompletionId != 0)
                {
                    var failure = CreateProtocolFailure();
                    entry.CompleteInitialFailure(failure);
                    return entry.Task;
                }

                entry.CompleteInitialSuccess();
                return entry.Task;
            }

            if (IsBackpressured(attempt.Failure)
                && attempt.CompletionId != 0)
            {
                try
                {
                    RequestReplySupport.ConsumeParts(parts);
                    entry.Arm(attempt.CompletionId);
                }
                catch (Exception exception)
                {
                    entry.ArmFailed(attempt.CompletionId, exception);
                    StartRuntimePump();
                    throw;
                }
                StartRuntimePump();
                return entry.Task;
            }

            var submitFailure = IsBackpressured(attempt.Failure)
                ? CreateProtocolFailure()
                : attempt.Failure;
            entry.AbortBeforeNativeWait(submitFailure);
            throw submitFailure;
        }
    }

    internal void Send(RoutingId? target, IReadOnlyList<Message> parts)
    {
        lock (_submitSync)
        {
            EnsureOpenForSubmit();
            ValidateSend(parts);
            var attempt = SubmitSend(target, parts, 0, IntPtr.Zero);
            if (attempt.Failure is not null)
                throw attempt.Failure;
            if (attempt.CompletionId != 0)
                throw CreateProtocolFailure();
        }
    }

    internal bool TrySend(RoutingId? target, IReadOnlyList<Message> parts)
    {
        lock (_submitSync)
        {
            EnsureOpenForSubmit();
            ValidateSend(parts);

            // Even a caller-visible false result has a native WRITABLE waiter.
            // Keep a payload-free sink alive until that exact token is pulled.
            var entry = new SendCompletionEntry(target);
            Register(entry);
            var attempt = SubmitSend(target, parts, DontWait, entry.Context);
            if (attempt.Failure is null)
            {
                if (attempt.CompletionId != 0)
                {
                    var failure = CreateProtocolFailure();
                    entry.AbortBeforeNativeWait(failure);
                    throw failure;
                }

                entry.CompleteInitialSuccess();
                return true;
            }

            if (IsBackpressured(attempt.Failure)
                && attempt.CompletionId != 0)
            {
                entry.Arm(attempt.CompletionId);
                StartRuntimePump();
                return false;
            }

            var submitFailure = IsBackpressured(attempt.Failure)
                ? CreateProtocolFailure()
                : attempt.Failure;
            entry.AbortBeforeNativeWait(submitFailure);
            throw submitFailure;
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
            Register(entry);
            try
            {
                var completionId = SubmitRequest(target, parts, timeoutMs,
                    DontWait, entry.Context);
                entry.Publish(completionId);
                StartRuntimePump();
                return entry.Task;
            }
            catch (Exception exception)
            {
                entry.AbortBeforeNativeWait(exception);
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
            Register(entry);
            try
            {
                var completionId = SubmitRequest(target, parts, timeoutMs, 0,
                    entry.Context);
                entry.Publish(completionId);
                StartRuntimePump();
            }
            catch (Exception exception)
            {
                entry.AbortBeforeNativeWait(exception);
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
        }
        StopRuntimePump();
    }

    internal void TransferToRuntime(object pollerOwner)
    {
        lock (_sync)
        {
            if (!ReferenceEquals(_publicOwner, pollerOwner))
                return;
            _publicOwner = null;
        }
        PrepareRuntimeDrain();
        StartRuntimePump();
    }

    internal CompletionDrainResult Drain()
    {
        // Submission and token publication are one critical section. A public
        // poller can therefore never observe WRITABLE before its entry is armed.
        lock (_submitSync)
        lock (_drainSync)
            return DrainCore();
    }

    private CompletionDrainResult DrainCore()
    {
        var processed = 0;
        var requests = 0;
        while (true)
        {
            ZlinkCompletion completion = default;
            completion.StructSize = checked(
                (uint)Marshal.SizeOf<ZlinkCompletion>());
            var rc = NativeMethods.zlink_completion_recv(_handle,
                ref completion, DontWait);
            if ((RecvResult)rc == RecvResult.NoData)
                return new CompletionDrainResult(processed, requests);
            if ((RecvResult)rc != RecvResult.Ok)
                throw ZlinkException.CreateRecvException((RecvResult)rc);

            CompletionEntry? entry;
            lock (_sync)
                _entries.TryGetValue(completion.UserContext, out entry);
            var isRequest = completion.Kind == CompletionKind.Request;
            try
            {
                entry?.Capture(ref completion);
            }
            finally
            {
                NativeMethods.zlink_completion_close(ref completion);
            }
            if (isRequest)
                requests++;
            processed++;
        }
    }

    internal void PrepareClose()
    {
        // Publish closing before waiting for an in-flight retry. The retry never
        // waits on close while holding an entry lock.
        lock (_sync)
            _closing = true;
        StopRuntimePump();
        Monitor.Enter(_submitSync);
    }

    internal void CancelClose()
    {
        try
        {
            lock (_sync)
                _closing = false;
            PrepareRuntimeDrain();
            StartRuntimePump();
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

    private void ValidateSend(IReadOnlyList<Message> parts)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        if (_socketType == SocketType.Stream && parts.Count != 1)
            throw new ArgumentException(
                "STREAM sends contain exactly one message part.", nameof(parts));
    }

    private void Register(CompletionEntry entry)
    {
        lock (_sync)
        {
            if (_closing)
                throw new ZlinkSubmitException(SubmitResult.Terminated,
                    (int)ErrorCode.EShutdown);
            var root = GCHandle.Alloc(entry, GCHandleType.Normal);
            var context = GCHandle.ToIntPtr(root);
            entry.Attach(this, context);
            _entries.Add(context, entry);
        }

        try
        {
            PrepareRuntimeDrain();
            entry.EnableCancellation();
        }
        catch
        {
            Remove(entry, entry.Context);
            throw;
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

    private SendAttempt SubmitSend(RoutingId? target,
        IReadOnlyList<Message> parts, int flags, IntPtr userContext)
    {
        unsafe
        {
            ZlinkRoutingId nativeTarget = default;
            var routed = target.HasValue;
            if (routed)
                nativeTarget = target!.Value.ToNative();
            ulong completionId = 0;
            var completionIdPointer = &completionId;
            try
            {
                RequestReplySupport.SubmitPreservingOnFailure(parts,
                    (ref ZlinkMsg nativePart,
                        NativeMethods.ZlinkPartFlag partFlag) =>
                    {
                        var final = partFlag ==
                            NativeMethods.ZlinkPartFlag.Final;
                        var context = final ? userContext : IntPtr.Zero;
                        var idOut = final && userContext != IntPtr.Zero
                            ? completionIdPointer : null;
                        return routed
                            ? NativeMethods.zlink_send_part_rid(_handle,
                                ref nativeTarget, ref nativePart, flags,
                                partFlag, context, idOut)
                            : NativeMethods.zlink_send_part(_handle,
                                ref nativePart, flags, partFlag, context,
                                idOut);
                    });
                return new SendAttempt(completionId, null);
            }
            catch (Exception exception)
            {
                return new SendAttempt(completionId, exception);
            }
        }
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
            throw CreateProtocolFailure();
        return completionId;
    }

    private static bool IsBackpressured(Exception exception)
    {
        return exception is ZlinkSubmitException submit
               && submit.Result ==
               ZlinkSubmitException.ErrorCode.Backpressured
               && ZlinkException.MapErrorCode(submit.NativeErrno)
               == ErrorCode.EAgain;
    }

    private static ZlinkSubmitException CreateProtocolFailure() =>
        new(SubmitResult.InternalError, (int)ErrorCode.EProtoNoSupport);

    private bool TargetMatches(RoutingId? target,
        ref ZlinkRoutingId completionTarget)
    {
        if (!target.HasValue)
            return completionTarget.Size == 0;
        var actual = RoutingIdCodec.ToRoutingId(ref completionTarget);
        return actual.HasValue && actual.Value == target.Value;
    }

    private void PrepareRuntimeDrain()
    {
        lock (_runtimeSync)
        {
            lock (_sync)
            {
                if (_closing || _publicOwner is not null
                    || _entries.Count == 0 || _runtimePoller != IntPtr.Zero)
                    return;
            }

            var poller = NativeMethods.zlink_poller_new();
            if (poller == IntPtr.Zero)
                throw ZlinkException.CreateConfigException(
                    NativeMethods.zlink_errno());
            var events = PollEventFlags.PollOut
                         | PollEventFlags.PollCompletion;
            var rc = NativeMethods.zlink_poller_add(poller, _handle,
                IntPtr.Zero, (short)events);
            if (rc != 0)
            {
                _ = NativeMethods.zlink_poller_destroy(ref poller);
                throw ZlinkException.CreateConfigException((ConfigResult)rc);
            }
            _runtimePoller = poller;
            _runtimePumpStarted = false;
            _runtimeEpoch++;
        }
    }

    private void StartRuntimePump()
    {
        PrepareRuntimeDrain();
        long epoch;
        lock (_runtimeSync)
        {
            lock (_sync)
            {
                if (_closing || _publicOwner is not null
                    || _entries.Count == 0 || _runtimePoller == IntPtr.Zero
                    || _runtimePumpStarted)
                    return;
            }
            _runtimePumpStarted = true;
            epoch = _runtimeEpoch;
        }

        // Start on the shared managed scheduler so a caller's single-threaded
        // SynchronizationContext cannot own or block completion progress.
        _ = Task.Run(() => RuntimePumpAsync(epoch));
    }

    private async Task RuntimePumpAsync(long epoch)
    {
        var events = new ZlinkPollerEvent[1];
        while (true)
        {
            // One non-blocking readiness turn at a time keeps progress on the
            // managed event loop without a dedicated OS thread or timer.
            await Task.Yield();
            var ready = false;
            var waitFailed = false;
            lock (_runtimeSync)
            {
                lock (_sync)
                {
                    if (epoch != _runtimeEpoch || _closing
                        || _publicOwner is not null || _entries.Count == 0
                        || _runtimePoller == IntPtr.Zero)
                    {
                        if (epoch == _runtimeEpoch)
                            _runtimePumpStarted = false;
                        return;
                    }
                }

                var rc = NativeMethods.zlink_poller_wait(_runtimePoller,
                    events, 1, 0, out var error);
                if (rc > 0)
                    ready = true;
                else if (rc < 0 && (ConfigResult)error != ConfigResult.Ok)
                {
                    waitFailed = true;
                }
            }

            if (waitFailed)
            {
                FailRuntimeWaits(epoch);
                return;
            }

            if (ready)
            {
                try
                {
                    DrainRuntime(epoch);
                }
                catch
                {
                    FailRuntimeWaits(epoch);
                    return;
                }
            }
        }
    }

    private void FailRuntimeWaits(long epoch)
    {
        // A poll/drain failure must not leave an awaiter unresolved. Native may
        // still own each published token, so entries become payload-free
        // tombstones and retain their GCHandle until that token is pulled by a
        // later pump/public poller or the socket closes.
        lock (_submitSync)
        {
            CompletionEntry[] entries;
            lock (_runtimeSync)
            lock (_sync)
            {
                if (epoch != _runtimeEpoch || _closing
                    || _publicOwner is not null)
                    return;
                _runtimePumpStarted = false;
                entries = _entries.Values.ToArray();
            }

            foreach (var entry in entries)
                entry.FailRuntimeWait();
        }
    }

    private void DrainRuntime(long epoch)
    {
        lock (_submitSync)
        lock (_drainSync)
        {
            lock (_runtimeSync)
            lock (_sync)
            {
                if (epoch != _runtimeEpoch || _closing
                    || _publicOwner is not null || _runtimePoller == IntPtr.Zero)
                    return;
            }
            DrainCore();
        }
    }

    private void StopRuntimePump()
    {
        lock (_runtimeSync)
        {
            _runtimeEpoch++;
            _runtimePumpStarted = false;
            if (_runtimePoller != IntPtr.Zero)
                _ = NativeMethods.zlink_poller_destroy(ref _runtimePoller);
        }
        // Wait for an already-claimed drain turn to leave the queue before a
        // public owner starts pulling from it.
        lock (_drainSync)
        {
        }
    }

    private readonly record struct SendAttempt(
        ulong CompletionId, Exception? Failure);

    private abstract class CompletionEntry
    {
        private int _removed;

        protected CompletionOwner Owner { get; private set; } = null!;
        internal IntPtr Context { get; private set; }

        internal void Attach(CompletionOwner owner, IntPtr context)
        {
            Owner = owner;
            Context = context;
        }

        internal virtual void EnableCancellation()
        {
        }

        internal abstract void Capture(ref ZlinkCompletion completion);
        internal abstract void AbortBeforeNativeWait(Exception exception);
        internal abstract void FailRuntimeWait();
        internal abstract void FailLifecycle();

        protected void Remove()
        {
            if (Interlocked.Exchange(ref _removed, 1) == 0)
                Owner.Remove(this, Context);
        }
    }

    private sealed class SendCompletionEntry : CompletionEntry
    {
        private readonly object _sync = new();
        private readonly RoutingId? _target;
        private readonly Message[]? _retained;
        private readonly CancellationToken _cancellationToken;
        private readonly TaskCompletionSource? _completion;
        private CancellationTokenRegistration _cancellationRegistration;
        private SendEntryState _state;
        private ulong _token;
        private bool _cancelClaimed;
        private bool _taskSettled;
        private bool _payloadReleased;

        internal SendCompletionEntry(RoutingId? target,
            Message[] retained, CancellationToken cancellationToken)
        {
            _target = target;
            _retained = retained;
            _cancellationToken = cancellationToken;
            _completion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
        }

        // Payload-free sink used by TrySubmit(false) until WRITABLE is pulled.
        internal SendCompletionEntry(RoutingId? target)
        {
            _target = target;
            _state = SendEntryState.Registered;
        }

        internal Task Task => _completion!.Task;

        internal override void EnableCancellation()
        {
            if (!_cancellationToken.CanBeCanceled)
                return;
            _cancellationRegistration = _cancellationToken.Register(
                static state => ((SendCompletionEntry)state!).Cancel(), this);
        }

        internal void CompleteInitialSuccess()
        {
            lock (_sync)
            {
                if (_state == SendEntryState.Terminal)
                    return;
                _state = SendEntryState.Terminal;
                ReleasePayloadLocked();
                SetResultLocked();
            }
            _cancellationRegistration.Unregister();
            Remove();
        }

        internal void CompleteInitialFailure(Exception exception)
        {
            lock (_sync)
            {
                if (_state == SendEntryState.Terminal)
                    return;
                _state = SendEntryState.Terminal;
                ReleasePayloadLocked();
                SetExceptionLocked(exception);
            }
            _cancellationRegistration.Unregister();
            Remove();
        }

        internal void Arm(ulong token)
        {
            lock (_sync)
            {
                if (_state != SendEntryState.Registered || token == 0)
                    throw new InvalidOperationException(
                        "A writable wait must be armed once with a nonzero token.");
                _token = token;
                _state = SendEntryState.Waiting;
                if (_cancelClaimed)
                {
                    ReleasePayloadLocked();
                    SetCanceledLocked();
                }
            }
        }

        internal void ArmFailed(ulong token, Exception exception)
        {
            lock (_sync)
            {
                if (_state != SendEntryState.Registered || token == 0)
                    return;
                _token = token;
                _state = SendEntryState.FailedWaiting;
                ReleasePayloadLocked();
                SetExceptionLocked(exception);
            }
            _cancellationRegistration.Unregister();
        }

        internal override void Capture(ref ZlinkCompletion completion)
        {
            Exception? terminalFailure = null;
            var retry = false;
            var keepExpectedWaiter = false;
            lock (_sync)
            {
                if (_state == SendEntryState.FailedWaiting)
                {
                    if (completion.CompletionId != _token)
                        return;
                    _state = SendEntryState.Terminal;
                }
                if (_state != SendEntryState.Waiting)
                {
                    if (_state != SendEntryState.Terminal)
                        return;
                }
                else if (completion.CompletionId != _token)
                {
                    terminalFailure = CreateProtocolFailure();
                    _state = SendEntryState.FailedWaiting;
                    ReleasePayloadLocked();
                    SetExceptionLocked(terminalFailure);
                    keepExpectedWaiter = true;
                }
                else if (completion.Kind != CompletionKind.Writable
                    || completion.UserContext != Context
                    || !Owner.TargetMatches(_target,
                        ref completion.PeerRoutingId))
                {
                    terminalFailure = CreateProtocolFailure();
                    _state = SendEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetExceptionLocked(terminalFailure);
                }
                else if (completion.SendResult ==
                         ZlinkSendCompleteResult.Terminal)
                {
                    terminalFailure = completion.SendTerminalErrno != 0
                        ? ZlinkException.CreateSubmitException(
                            completion.SendTerminalErrno)
                        : new ZlinkSubmitException(SubmitResult.NotAdmitted);
                    _state = SendEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetExceptionLocked(terminalFailure);
                }
                else if (completion.SendResult !=
                         ZlinkSendCompleteResult.Admitted
                         || completion.SendTerminalErrno != 0)
                {
                    terminalFailure = CreateProtocolFailure();
                    _state = SendEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetExceptionLocked(terminalFailure);
                }
                else if (_cancelClaimed || _retained is null)
                {
                    _state = SendEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetCanceledLocked();
                }
                else
                {
                    _state = SendEntryState.Retrying;
                    retry = true;
                }
            }

            if (keepExpectedWaiter)
                return;
            if (!retry)
            {
                _cancellationRegistration.Unregister();
                Remove();
                return;
            }

            var attempt = Owner.SubmitSend(_target, _retained!, DontWait,
                Context);
            var terminal = false;
            lock (_sync)
            {
                if (attempt.Failure is null && attempt.CompletionId == 0)
                {
                    _state = SendEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetResultLocked();
                    terminal = true;
                }
                else if (attempt.Failure is not null
                         && IsBackpressured(attempt.Failure)
                         && attempt.CompletionId != 0)
                {
                    _token = attempt.CompletionId;
                    _state = SendEntryState.Waiting;
                    if (_cancelClaimed)
                    {
                        ReleasePayloadLocked();
                        SetCanceledLocked();
                    }
                }
                else
                {
                    terminalFailure = attempt.Failure is null
                                      || IsBackpressured(attempt.Failure)
                        ? CreateProtocolFailure()
                        : attempt.Failure;
                    _state = SendEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetExceptionLocked(terminalFailure);
                    terminal = true;
                }
            }

            if (terminal)
            {
                _cancellationRegistration.Unregister();
                Remove();
            }
        }

        internal override void AbortBeforeNativeWait(Exception exception)
        {
            lock (_sync)
            {
                if (_state == SendEntryState.Terminal)
                    return;
                _state = SendEntryState.Terminal;
                ReleasePayloadLocked();
                SetExceptionLocked(exception);
            }
            _cancellationRegistration.Unregister();
            Remove();
        }

        internal override void FailLifecycle()
        {
            AbortBeforeNativeWait(new ZlinkSubmitException(
                SubmitResult.Terminated, (int)ErrorCode.EShutdown));
        }

        internal override void FailRuntimeWait()
        {
            lock (_sync)
            {
                if (_state != SendEntryState.Waiting)
                    return;
                _state = SendEntryState.FailedWaiting;
                ReleasePayloadLocked();
                SetExceptionLocked(new ZlinkSubmitException(
                    SubmitResult.InternalError));
            }
        }

        private void Cancel()
        {
            lock (_sync)
            {
                if (_state == SendEntryState.Terminal || _taskSettled)
                    return;
                _cancelClaimed = true;
                SetCanceledLocked();
                if (_state == SendEntryState.Waiting)
                    ReleasePayloadLocked();
            }
        }

        private void SetResultLocked()
        {
            if (_taskSettled || _completion is null)
                return;
            _taskSettled = true;
            if (_cancelClaimed)
                _completion.TrySetCanceled(_cancellationToken);
            else
                _completion.TrySetResult();
        }

        private void SetExceptionLocked(Exception exception)
        {
            if (_taskSettled || _completion is null)
                return;
            _taskSettled = true;
            if (_cancelClaimed)
                _completion.TrySetCanceled(_cancellationToken);
            else
                _completion.TrySetException(exception);
        }

        private void SetCanceledLocked()
        {
            if (_taskSettled || _completion is null)
                return;
            _taskSettled = true;
            _completion.TrySetCanceled(_cancellationToken);
        }

        private void ReleasePayloadLocked()
        {
            if (_payloadReleased || _retained is null)
                return;
            _payloadReleased = true;
            RequestReplySupport.DisposeParts(_retained);
        }
    }

    private sealed class RequestCompletionEntry : CompletionEntry
    {
        private readonly object _sync = new();
        private readonly CancellationToken _cancellationToken;
        private readonly TaskCompletionSource<IReadOnlyList<Message>>
            _completion = new(
                TaskCreationOptions.RunContinuationsAsynchronously);
        private CancellationTokenRegistration _cancellationRegistration;
        private IReadOnlyList<Message> _reply = Array.Empty<Message>();
        private ulong _completionId;
        private bool _published;
        private bool _terminal;
        private bool _failedWaiting;
        private bool _cancelClaimed;
        private bool _taskSettled;

        internal RequestCompletionEntry(CancellationToken cancellationToken)
        {
            _cancellationToken = cancellationToken;
        }

        internal Task<IReadOnlyList<Message>> Task => _completion.Task;

        internal override void EnableCancellation()
        {
            if (!_cancellationToken.CanBeCanceled)
                return;
            _cancellationRegistration = _cancellationToken.Register(
                static state => ((RequestCompletionEntry)state!).Cancel(), this);
        }

        internal void Publish(ulong completionId)
        {
            if (completionId == 0)
                throw CreateProtocolFailure();
            lock (_sync)
            {
                _completionId = completionId;
                _published = true;
                if (_cancelClaimed)
                    SetCanceledLocked();
            }
        }

        internal override unsafe void Capture(ref ZlinkCompletion completion)
        {
            var keepExpectedWaiter = false;
            lock (_sync)
            {
                if (_terminal)
                    return;
                Exception? failure = null;
                if (_failedWaiting)
                {
                    if (completion.CompletionId != _completionId)
                        return;
                    _terminal = true;
                }
                else if (_published
                         && completion.CompletionId != _completionId)
                {
                    failure = new ZlinkRequestException(
                        RequestResult.InternalError);
                    _failedWaiting = true;
                    keepExpectedWaiter = true;
                }
                else
                {
                    _terminal = true;
                    if (!_published
                        || completion.Kind != CompletionKind.Request
                        || completion.UserContext != Context)
                        failure = new ZlinkRequestException(
                            RequestResult.InternalError);
                    else if (!_cancelClaimed)
                    {
                        if (completion.RequestResult != RequestResult.Ok)
                            failure = new ZlinkRequestException(
                                completion.RequestResult);
                        else
                        {
                            try
                            {
                                _reply = MoveReply(ref completion);
                            }
                            catch (Exception exception)
                            {
                                failure = exception;
                            }
                        }
                    }
                }

                if (!_taskSettled)
                {
                    if (_cancelClaimed)
                        SetCanceledLocked();
                    else if (failure is not null)
                        SetExceptionLocked(failure);
                    else
                        SetResultLocked();
                }
            }
            if (keepExpectedWaiter)
                return;
            _cancellationRegistration.Unregister();
            Remove();
        }

        internal override void AbortBeforeNativeWait(Exception exception)
        {
            lock (_sync)
            {
                if (_terminal)
                    return;
                _terminal = true;
                if (!_taskSettled)
                {
                    if (_cancelClaimed)
                        SetCanceledLocked();
                    else
                        SetExceptionLocked(exception);
                }
            }
            _cancellationRegistration.Unregister();
            Remove();
        }

        internal override void FailLifecycle()
        {
            AbortBeforeNativeWait(new ZlinkRequestException(
                RequestResult.Terminated));
        }

        internal override void FailRuntimeWait()
        {
            lock (_sync)
            {
                if (_terminal || !_published || _failedWaiting)
                    return;
                _failedWaiting = true;
                if (!_taskSettled)
                    SetExceptionLocked(new ZlinkRequestException(
                        RequestResult.InternalError));
            }
        }

        private void Cancel()
        {
            lock (_sync)
            {
                if (_terminal || _taskSettled)
                    return;
                _cancelClaimed = true;
                SetCanceledLocked();
            }
        }

        private static unsafe IReadOnlyList<Message> MoveReply(
            ref ZlinkCompletion completion)
        {
            var count = checked((int)completion.ReplyPartCount);
            if (count == 0)
                return Array.Empty<Message>();
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
                return result;
            }
            catch
            {
                for (var i = 0; i < built; i++)
                    result[i].Dispose();
                throw;
            }
        }

        private void SetResultLocked()
        {
            _taskSettled = true;
            _completion.TrySetResult(_reply);
        }

        private void SetExceptionLocked(Exception exception)
        {
            _taskSettled = true;
            _completion.TrySetException(exception);
        }

        private void SetCanceledLocked()
        {
            if (_taskSettled)
                return;
            _taskSettled = true;
            _completion.TrySetCanceled(_cancellationToken);
        }
    }

    private enum SendEntryState
    {
        Registered,
        Waiting,
        FailedWaiting,
        Retrying,
        Terminal
    }
}

internal readonly record struct CompletionDrainResult(
    int TotalCount, int RequestCount);
