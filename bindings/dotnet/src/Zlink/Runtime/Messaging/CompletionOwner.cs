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
    private readonly object _submitSync = new();
    private readonly object _runtimeSync = new();
    private readonly Dictionary<IntPtr, CompletionEntry> _entries = new();
    private List<CompletionEntry>? _retries;

    private object? _publicOwner;
    private IntPtr _runtimePoller;
    private long _runtimeEpoch;
    private bool _runtimePumpStarted;
    private bool _closing;
    private long _nextContext;

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

            // Keep cancellation registration before admission for cancelable
            // calls. The common noncancelable path needs no entry or Task until
            // Core actually returns a writable token. Drain shares _submitSync,
            // so that token cannot be pulled before registration and Arm.
            SendCompletionEntry? entry = null;
            var context = NextContext();
            if (cancellationToken.CanBeCanceled)
            {
                entry = new SendCompletionEntry(this, target, cancellationToken);
                Register(entry, context);
            }
            var attempt = SubmitSend(target, parts, DontWait, context);
            if (attempt.Failure is null)
            {
                if (attempt.CompletionId != 0)
                {
                    var failure = CreateProtocolFailure();
                    if (entry is null)
                        return Task.FromException(failure);
                    entry.CompleteInitialFailure(failure);
                    return entry.Task;
                }

                if (entry is null)
                    return Task.CompletedTask;
                entry.CompleteInitialSuccess();
                return entry.Task;
            }

            if (IsBackpressured(attempt.Failure)
                && attempt.CompletionId != 0)
            {
                if (entry is null)
                {
                    entry = new SendCompletionEntry(this, target, cancellationToken);
                    Register(entry, context, admittedSubmit: true);
                }
                Message[]? retained = null;
                try
                {
                    // Core retains only the token. Take a shared zlink_msg
                    // snapshot only after rejection, then consume the caller's
                    // messages once ownership has moved to this operation.
                    retained = RequestReplySupport.CloneParts(parts);
                    RequestReplySupport.ConsumeParts(parts);
                    entry.Arm(attempt.CompletionId, retained);
                }
                catch (Exception exception)
                {
                    entry.ArmFailed(attempt.CompletionId, retained, exception);
                    StartRuntimePump();
                    throw;
                }
                StartRuntimePump();
                return entry.Task;
            }

            entry?.AbortBeforeNativeWait(attempt.Failure);
            throw attempt.Failure;
        }
    }

    internal void Send(RoutingId? target, IReadOnlyList<Message> parts)
    {
        EnsureOpenForSubmit();
        ValidateSend(parts);
        var attempt = SubmitSend(target, parts, 0, IntPtr.Zero);
        if (attempt.Failure is not null)
            throw attempt.Failure;
        if (attempt.CompletionId != 0)
            throw CreateProtocolFailure();
    }

    internal bool TrySend(RoutingId? target, IReadOnlyList<Message> parts)
    {
        lock (_submitSync)
        {
            EnsureOpenForSubmit();
            ValidateSend(parts);

            // Even a caller-visible false result has a native WRITABLE waiter.
            // Keep a payload-free sink alive until that exact token is pulled.
            var context = NextContext();
            var attempt = SubmitSend(target, parts, DontWait, context);
            if (attempt.Failure is null)
            {
                if (attempt.CompletionId != 0)
                {
                    var failure = CreateProtocolFailure();
                    throw failure;
                }

                return true;
            }

            if (IsBackpressured(attempt.Failure)
                && attempt.CompletionId != 0)
            {
                var entry = new SendCompletionEntry(this, target);
                Register(entry, context, admittedSubmit: true);
                entry.Arm(attempt.CompletionId);
                StartRuntimePump();
                return false;
            }

            throw attempt.Failure;
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

            var entry = new RequestCompletionEntry(this, target, timeoutMs,
                cancellationToken);
            Register(entry);
            var attempt = SubmitRequest(target, parts, timeoutMs, DontWait,
                entry.Context);
            if (attempt.Failure is null)
            {
                if (attempt.CompletionId == 0)
                {
                    var failure = CreateProtocolFailure();
                    entry.AbortBeforeNativeWait(failure);
                    throw failure;
                }

                entry.PublishRequest(attempt.CompletionId);
                StartRuntimePump();
                return entry.Task;
            }

            if (IsBackpressured(attempt.Failure)
                && attempt.CompletionId != 0)
            {
                Message[]? retained = null;
                try
                {
                    // Core retains only the token. Snapshot the request only
                    // after refusal so the admitted path remains unchanged.
                    retained = RequestReplySupport.CloneParts(parts);
                    RequestReplySupport.ConsumeParts(parts);
                    entry.ArmWritable(attempt.CompletionId, retained);
                }
                catch (Exception exception)
                {
                    entry.ArmFailed(attempt.CompletionId, retained, exception);
                    StartRuntimePump();
                    throw;
                }
                StartRuntimePump();
                return entry.Task;
            }

            entry.AbortBeforeNativeWait(attempt.Failure);
            throw attempt.Failure;
        }
    }

    internal IReadOnlyList<Message> Request(RoutingId? target,
        IReadOnlyList<Message> parts, uint timeoutMs)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        var entry = new RequestCompletionEntry(this, target, timeoutMs,
            CancellationToken.None);
        Register(entry);
        var attempt = SubmitRequest(target, parts, timeoutMs, 0,
            entry.Context);
        if (attempt.Failure is not null)
        {
            entry.AbortBeforeNativeWait(attempt.Failure);
            throw attempt.Failure;
        }
        if (attempt.CompletionId == 0)
        {
            var failure = CreateProtocolFailure();
            entry.AbortBeforeNativeWait(failure);
            throw failure;
        }
        entry.PublishRequest(attempt.CompletionId);
        StartRuntimePump();
        return entry.Task.GetAwaiter().GetResult();
    }

    internal void Reply(RoutingId target, ReplyToken replyToken,
        IReadOnlyList<Message> parts)
    {
        EnsureOpenForSubmit();
        var submitter = new ReplyPartSubmitter
        {
            Handle = _handle,
            Target = target.ToNative(),
            ReplyToken = replyToken.Value
        };
        RequestReplySupport.SubmitPreservingOnFailure(parts, ref submitter);
    }

    // Like send and request, synchronous reply keeps its native arguments on
    // the stack. The submitter never escapes SubmitPreservingOnFailure.
    private struct ReplyPartSubmitter : INativePartSubmitter<ReplyPartSubmitter>
    {
        internal IntPtr Handle;
        internal ZlinkRoutingId Target;
        internal ulong ReplyToken;

        public static int Submit(ref ReplyPartSubmitter self,
            ref ZlinkMsg part, NativeMethods.ZlinkPartFlag flag) =>
            NativeMethods.zlink_reply_part(self.Handle, ref self.Target,
                self.ReplyToken, ref part, flag);
    }

    internal bool TransferToPublic(object pollerOwner)
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
                return false;
            _publicOwner = pollerOwner;
        }
        StopRuntimePump();
        return true;
    }

    internal void TransferToRuntime(object pollerOwner)
    {
        lock (_sync)
        {
            if (!ReferenceEquals(_publicOwner, pollerOwner))
                return;
            _publicOwner = null;
        }
        try
        {
            PrepareRuntimeDrain();
            StartRuntimePump();
        }
        catch
        {
            // Native removal has already committed, so ownership cannot be
            // rolled back. Settle managed waiters instead of leaking a wait
            // with no completion drain owner.
            StopRuntimePump();
            FailUnownedWaits();
        }
    }

    internal CompletionDrainResult Drain()
    {
        // DONTWAIT submission and token publication share this section, so
        // WRITABLE cannot be pulled before its entry is armed. Blocking request
        // publication joins a pre-return completion inside that request entry.
        lock (_submitSync)
        {
            lock (_sync)
                if (_closing)
                    return default;
            return DrainCore();
        }
    }

    internal void FailPublicWaitTerminated()
    {
        lock (_submitSync)
        {
            CompletionEntry[] entries;
            lock (_sync)
                entries = _entries.Values.ToArray();
            foreach (var entry in entries)
                entry.FailLifecycle();
        }
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
            {
                // Core's part contract requires draining to NO_DATA before
                // retrying. A retry may publish another WRITABLE immediately;
                // it belongs to a subsequent drain, never this one.
                if (_retries is { Count: > 0 })
                {
                    try
                    {
                        foreach (var retry in _retries)
                            retry.Retry();
                    }
                    finally
                    {
                        _retries.Clear();
                    }
                }
                return new CompletionDrainResult(processed, requests);
            }
            if ((RecvResult)rc != RecvResult.Ok)
            {
                FailDrainWaits((RecvResult)rc);
                _retries?.Clear();
                throw ZlinkException.CreateRecvException((RecvResult)rc);
            }

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

    // Core treats user_context as an opaque value. The registry roots entries;
    // monotonically increasing socket-local identities never reuse a GCHandle
    // address and require no managed/native handle-table round trip.
    private IntPtr NextContext() =>
        checked((IntPtr)Interlocked.Increment(ref _nextContext));

    private void Register(CompletionEntry entry, IntPtr context = default,
        bool admittedSubmit = false)
    {
        lock (_sync)
        {
            if (_closing && !admittedSubmit)
                throw new ZlinkSubmitException(SubmitResult.Terminated,
                    (int)ErrorCode.EShutdown);
            if (context == IntPtr.Zero)
                context = NextContext();
            entry.Context = context;
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
        if (Volatile.Read(ref _closing))
            throw new ZlinkSubmitException(SubmitResult.Terminated,
                (int)ErrorCode.EShutdown);
    }

    private void Remove(CompletionEntry entry, IntPtr context)
    {
        lock (_sync)
        {
            if (!_entries.TryGetValue(context, out var current)
                || !ReferenceEquals(current, entry))
                return;
            _entries.Remove(context);
        }
    }

    private SendAttempt SubmitSend(RoutingId? target,
        IReadOnlyList<Message> parts, int flags, IntPtr userContext)
    {
        var submitter = new SendPartSubmitter
        {
            Handle = _handle,
            Target = target.HasValue ? target.Value.ToNative() : default,
            Routed = target.HasValue,
            Flags = flags,
            Context = userContext
        };
        try
        {
            RequestReplySupport.SubmitPreservingOnFailure(parts, ref submitter);
            return new SendAttempt(submitter.CompletionId, null);
        }
        catch (Exception exception)
        {
            return new SendAttempt(submitter.CompletionId, exception);
        }
    }

    private unsafe struct SendPartSubmitter : INativePartSubmitter<SendPartSubmitter>
    {
        internal IntPtr Handle, Context;
        internal ZlinkRoutingId Target;
        internal bool Routed;
        internal int Flags;
        internal ulong CompletionId;

        public static int Submit(ref SendPartSubmitter self,
            ref ZlinkMsg part, NativeMethods.ZlinkPartFlag flag)
        {
            var final = flag == NativeMethods.ZlinkPartFlag.Final;
            var context = final ? self.Context : IntPtr.Zero;
            fixed (ulong* id = &self.CompletionId)
            {
                var idOut = final && context != IntPtr.Zero ? id : null;
                return self.Routed
                    ? NativeMethods.zlink_send_part_rid(self.Handle,
                        ref self.Target, ref part, self.Flags, flag, context, idOut)
                    : NativeMethods.zlink_send_part(self.Handle,
                        ref part, self.Flags, flag, context, idOut);
            }
        }
    }

    private SendAttempt SubmitRequest(RoutingId? target,
        IReadOnlyList<Message> parts, uint timeoutMs, int flags,
        IntPtr userContext)
    {
        var submitter = new RequestPartSubmitter
        {
            Handle = _handle,
            Target = target.HasValue ? target.Value.ToNative() : default,
            Routed = target.HasValue,
            Flags = flags,
            TimeoutMs = timeoutMs,
            Context = userContext
        };
        try
        {
            RequestReplySupport.SubmitPreservingOnFailure(parts, ref submitter);
            return new SendAttempt(submitter.CompletionId, null);
        }
        catch (Exception exception)
        {
            return new SendAttempt(submitter.CompletionId, exception);
        }
    }

    private unsafe struct RequestPartSubmitter : INativePartSubmitter<RequestPartSubmitter>
    {
        internal IntPtr Handle, Context;
        internal ZlinkRoutingId Target;
        internal bool Routed;
        internal int Flags;
        internal uint TimeoutMs;
        internal ulong CompletionId;

        public static int Submit(ref RequestPartSubmitter self,
            ref ZlinkMsg part, NativeMethods.ZlinkPartFlag flag)
        {
            var final = flag == NativeMethods.ZlinkPartFlag.Final;
            fixed (ZlinkRoutingId* target = &self.Target)
            fixed (ulong* id = &self.CompletionId)
                return NativeMethods.zlink_request_part(self.Handle,
                    self.Routed ? target : null, ref part, self.Flags, flag,
                    final ? self.TimeoutMs : 0,
                    final ? self.Context : IntPtr.Zero, final ? id : null);
        }
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
            var events = PollEventFlags.PollCompletion;
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
        // Scope both captures after the early returns: an existing pump or a
        // public drain owner needs no scheduled callback or closure allocation.
        {
            var owner = this;
            var pumpEpoch = epoch;
            _ = Task.Run(() => owner.RuntimePump(pumpEpoch));
        }
    }

    private void RuntimePump(long epoch)
    {
        var events = new ZlinkPollerEvent[1];
        while (true)
        {
            var ready = false;
            var waitFailed = false;
            var waitTerminated = false;
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
                    events, 1, 25, out var error);
                if (rc > 0)
                    ready = true;
                else if (rc < 0 && (ConfigResult)error != ConfigResult.Ok)
                {
                    // A signal-interrupted wait (EINTR) leaves the queue and
                    // every token intact; only a real failure ends the pump.
                    var errno = NativeMethods.zlink_errno();
                    if (ZlinkException.MapErrorCode(errno) != ErrorCode.EIntr)
                    {
                        waitFailed = true;
                        waitTerminated =
                            ZlinkException.IsTerminationError(errno);
                    }
                }
            }

            if (waitFailed)
            {
                FailRuntimeWaits(epoch, waitTerminated);
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

    private void FailDrainWaits(RecvResult result)
    {
        CompletionEntry[] entries;
        lock (_sync)
        {
            if (result == RecvResult.Terminated)
                _closing = true;
            entries = _entries.Values.ToArray();
        }

        foreach (var entry in entries)
        {
            if (result == RecvResult.Terminated)
                entry.FailLifecycle();
            else
                entry.FailRuntimeWait();
        }
    }

    private void FailUnownedWaits()
    {
        lock (_submitSync)
        {
            CompletionEntry[] entries;
            lock (_sync)
            {
                if (_closing || _publicOwner is not null)
                    return;
                entries = _entries.Values.ToArray();
            }
            foreach (var entry in entries)
                entry.FailRuntimeWait();
        }
    }

    private void FailRuntimeWaits(long epoch, bool terminated = false)
    {
        // A poll/drain failure must not leave an awaiter unresolved. Native may
        // still own each published token, so entries become payload-free
        // tombstones and retain their registry identity until that token is pulled by a
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
            {
                if (terminated)
                    entry.FailLifecycle();
                else
                    entry.FailRuntimeWait();
            }
        }
    }

    private void DrainRuntime(long epoch)
    {
        lock (_submitSync)
        {
            lock (_runtimeSync)
            {
                lock (_sync)
                {
                    if (epoch != _runtimeEpoch || _closing
                        || _publicOwner is not null
                        || _runtimePoller == IntPtr.Zero)
                        return;
                }
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
        lock (_submitSync)
        {
        }
    }

    private readonly record struct SendAttempt(
        ulong CompletionId, Exception? Failure);

    private interface CompletionEntry
    {
        IntPtr Context { get; set; }
        void EnableCancellation();
        void Capture(ref ZlinkCompletion completion);
        void Retry();
        void AbortBeforeNativeWait(Exception exception);
        void FailRuntimeWait();
        void FailLifecycle();
    }

    private sealed class SendCompletionEntry : CompletionEntry
    {
        private readonly CompletionOwner Owner;
        public IntPtr Context { get; set; }
        private readonly RoutingId? _target;
        private Message[]? _retained;
        private readonly CancellationToken _cancellationToken;
        private readonly TaskCompletionSource? _completion;
        private CancellationTokenRegistration _cancellationRegistration;
        private SendEntryState _state;
        private ulong _token;
        private bool _cancelClaimed;
        private bool _taskSettled;
        private bool _payloadReleased;

        internal SendCompletionEntry(CompletionOwner owner, RoutingId? target,
            CancellationToken cancellationToken)
        {
            Owner = owner;
            _target = target;
            _cancellationToken = cancellationToken;
            _completion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
        }

        // Payload-free sink used by TrySubmit(false) until WRITABLE is pulled.
        internal SendCompletionEntry(CompletionOwner owner, RoutingId? target)
        {
            Owner = owner;
            _target = target;
            _state = SendEntryState.Registered;
        }

        internal Task Task => _completion!.Task;

        public void EnableCancellation()
        {
            if (!_cancellationToken.CanBeCanceled)
                return;
            _cancellationRegistration = _cancellationToken.Register(
                static state => ((SendCompletionEntry)state!).Cancel(), this);
        }

        internal void CompleteInitialSuccess()
        {
            lock (this)
            {
                if (_state == SendEntryState.Terminal)
                    return;
                _state = SendEntryState.Terminal;
                ReleasePayloadLocked();
                SetResultLocked();
            }
            _cancellationRegistration.Unregister();
            Owner.Remove(this, Context);
        }

        internal void CompleteInitialFailure(Exception exception)
        {
            lock (this)
            {
                if (_state == SendEntryState.Terminal)
                    return;
                _state = SendEntryState.Terminal;
                ReleasePayloadLocked();
                SetExceptionLocked(exception);
            }
            _cancellationRegistration.Unregister();
            Owner.Remove(this, Context);
        }

        internal void Arm(ulong token, Message[]? retained = null)
        {
            lock (this)
            {
                if (_state != SendEntryState.Registered || token == 0)
                    throw new InvalidOperationException(
                        "A writable wait must be armed once with a nonzero token.");
                _token = token;
                _retained = retained;
                _state = SendEntryState.Waiting;
                if (_cancelClaimed)
                {
                    ReleasePayloadLocked();
                    SetCanceledLocked();
                }
            }
        }

        internal void ArmFailed(ulong token, Message[]? retained,
            Exception exception)
        {
            lock (this)
            {
                if (_state != SendEntryState.Registered || token == 0)
                    return;
                _token = token;
                _retained = retained;
                _state = SendEntryState.FailedWaiting;
                ReleasePayloadLocked();
                SetExceptionLocked(exception);
            }
            _cancellationRegistration.Unregister();
        }

        public void Capture(ref ZlinkCompletion completion)
        {
            Exception? terminalFailure = null;
            var retry = false;
            var keepExpectedWaiter = false;
            lock (this)
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
                    || completion.UserContext != Context)
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
                else if (_completion is null)
                {
                    // TrySubmit(false) owns no payload and only drains the
                    // token that Core necessarily published.
                    _state = SendEntryState.Terminal;
                }
                else if (_cancelClaimed)
                {
                    _state = SendEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetCanceledLocked();
                }
                else if (_retained is null)
                {
                    terminalFailure = CreateProtocolFailure();
                    _state = SendEntryState.Terminal;
                    SetExceptionLocked(terminalFailure);
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
                Owner.Remove(this, Context);
                return;
            }

            (Owner._retries ??= new List<CompletionEntry>()).Add(this);
        }

        public void Retry()
        {
            var attempt = Owner.SubmitSend(_target, _retained!, DontWait,
                Context);
            var terminal = false;
            lock (this)
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
                    var terminalFailure = attempt.Failure ?? CreateProtocolFailure();
                    _state = SendEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetExceptionLocked(terminalFailure);
                    terminal = true;
                }
            }

            if (terminal)
            {
                _cancellationRegistration.Unregister();
                Owner.Remove(this, Context);
            }
        }

        public void AbortBeforeNativeWait(Exception exception)
        {
            lock (this)
            {
                if (_state == SendEntryState.Terminal)
                    return;
                _state = SendEntryState.Terminal;
                ReleasePayloadLocked();
                SetExceptionLocked(exception);
            }
            _cancellationRegistration.Unregister();
            Owner.Remove(this, Context);
        }

        public void FailLifecycle()
        {
            AbortBeforeNativeWait(new ZlinkSubmitException(
                SubmitResult.Terminated, (int)ErrorCode.EShutdown));
        }

        public void FailRuntimeWait()
        {
            if (_state == SendEntryState.Retrying)
            {
                AbortBeforeNativeWait(CreateProtocolFailure());
                return;
            }
            lock (this)
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
            lock (this)
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

    private sealed class RequestCompletionEntry : TaskCompletionSource<IReadOnlyList<Message>>, CompletionEntry
    {
        private readonly CompletionOwner Owner;
        public IntPtr Context { get; set; }
        private readonly RoutingId? _target;
        private readonly uint _timeoutMs;
        private readonly CancellationToken _cancellationToken;
        private CancellationTokenRegistration _cancellationRegistration;
        private IReadOnlyList<Message> _reply = Array.Empty<Message>();
        private Message[]? _retained;
        private ulong _completionId;
        private RequestEntryState _state;
        private bool _cancelClaimed;
        private bool _payloadReleased;

        internal RequestCompletionEntry(CompletionOwner owner, RoutingId? target, uint timeoutMs,
            CancellationToken cancellationToken)
            : base(TaskCreationOptions.RunContinuationsAsynchronously)
        {
            Owner = owner;
            _target = target;
            _timeoutMs = timeoutMs;
            _cancellationToken = cancellationToken;
        }

        public void EnableCancellation()
        {
            if (!_cancellationToken.CanBeCanceled)
                return;
            _cancellationRegistration = _cancellationToken.Register(
                static state => ((RequestCompletionEntry)state!).Cancel(), this);
        }

        internal void PublishRequest(ulong completionId)
        {
            if (completionId == 0)
                throw CreateProtocolFailure();
            lock (this)
            {
                if (_state == RequestEntryState.Terminal)
                    return;
                if (_state != RequestEntryState.Registered)
                    throw new InvalidOperationException(
                        "A request completion must be published once.");
                _completionId = completionId;
                _state = RequestEntryState.WaitingRequest;
                Monitor.PulseAll(this);
                if (_cancelClaimed)
                    SetCanceledLocked();
            }
        }

        internal void ArmWritable(ulong token, Message[] retained)
        {
            lock (this)
            {
                if (_state != RequestEntryState.Registered || token == 0)
                    throw new InvalidOperationException(
                        "A writable wait must be armed once with a nonzero token.");
                _completionId = token;
                _retained = retained;
                _state = RequestEntryState.WaitingWritable;
                if (_cancelClaimed)
                {
                    ReleasePayloadLocked();
                    SetCanceledLocked();
                }
            }
        }

        internal void ArmFailed(ulong token, Message[]? retained,
            Exception exception)
        {
            lock (this)
            {
                if (_state != RequestEntryState.Registered || token == 0)
                    return;
                _completionId = token;
                _retained = retained;
                _state = RequestEntryState.FailedWaiting;
                ReleasePayloadLocked();
                SetExceptionLocked(exception);
            }
            _cancellationRegistration.Unregister();
        }

        public unsafe void Capture(ref ZlinkCompletion completion)
        {
            Exception? failure = null;
            var retry = false;
            var keepExpectedWaiter = false;
            lock (this)
            {
                // The drain owns the native record until submit publication
                // joins it. Waiting releases only this entry's monitor; other
                // blocking submissions use their own entries and Core admission.
                while (_state == RequestEntryState.Registered)
                    Monitor.Wait(this);
                if (_state == RequestEntryState.Terminal)
                    return;
                if (_state == RequestEntryState.FailedWaiting)
                {
                    if (completion.CompletionId != _completionId)
                        return;
                    _state = RequestEntryState.Terminal;
                }
                else if (completion.CompletionId != _completionId)
                {
                    failure = CreateStateFailure();
                    _state = RequestEntryState.FailedWaiting;
                    ReleasePayloadLocked();
                    SetExceptionLocked(failure);
                    keepExpectedWaiter = true;
                }
                else if (_state == RequestEntryState.WaitingWritable)
                {
                    if (completion.Kind != CompletionKind.Writable
                        || completion.UserContext != Context)
                    {
                        failure = CreateProtocolFailure();
                        _state = RequestEntryState.Terminal;
                        ReleasePayloadLocked();
                        SetExceptionLocked(failure);
                    }
                    else if (completion.SendResult ==
                             ZlinkSendCompleteResult.Terminal)
                    {
                        failure = completion.SendTerminalErrno != 0
                            ? ZlinkException.CreateSubmitException(
                                completion.SendTerminalErrno)
                            : new ZlinkSubmitException(
                                SubmitResult.NotAdmitted);
                        _state = RequestEntryState.Terminal;
                        ReleasePayloadLocked();
                        SetExceptionLocked(failure);
                    }
                    else if (completion.SendResult !=
                             ZlinkSendCompleteResult.Admitted
                             || completion.SendTerminalErrno != 0)
                    {
                        failure = CreateProtocolFailure();
                        _state = RequestEntryState.Terminal;
                        ReleasePayloadLocked();
                        SetExceptionLocked(failure);
                    }
                    else if (_cancelClaimed)
                    {
                        _state = RequestEntryState.Terminal;
                        ReleasePayloadLocked();
                        SetCanceledLocked();
                    }
                    else if (_retained is null)
                    {
                        failure = CreateProtocolFailure();
                        _state = RequestEntryState.Terminal;
                        SetExceptionLocked(failure);
                    }
                    else
                    {
                        _state = RequestEntryState.Retrying;
                        retry = true;
                    }
                }
                else if (_state == RequestEntryState.WaitingRequest)
                {
                    _state = RequestEntryState.Terminal;
                    if (completion.Kind != CompletionKind.Request
                        || completion.UserContext != Context)
                    {
                        failure = new ZlinkRequestException(
                            RequestResult.InternalError);
                        SetExceptionLocked(failure);
                    }
                    else if (!_cancelClaimed)
                    {
                        if (completion.RequestResult != RequestResult.Ok)
                        {
                            failure = new ZlinkRequestException(
                                completion.RequestResult);
                            SetExceptionLocked(failure);
                        }
                        else
                        {
                            try
                            {
                                _reply = MoveReply(ref completion);
                                SetResultLocked();
                            }
                            catch (Exception exception)
                            {
                                failure = exception;
                                SetExceptionLocked(failure);
                            }
                        }
                    }
                    else
                    {
                        SetCanceledLocked();
                    }
                }
                else
                {
                    failure = CreateStateFailure();
                    _state = RequestEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetExceptionLocked(failure);
                }
            }
            if (keepExpectedWaiter)
                return;
            if (retry)
            {
                (Owner._retries ??= new List<CompletionEntry>()).Add(this);
                return;
            }
            _cancellationRegistration.Unregister();
            Owner.Remove(this, Context);
        }

        void CompletionEntry.Retry() => RetryRequest();

        private void RetryRequest()
        {
            var attempt = Owner.SubmitRequest(_target, _retained!, _timeoutMs,
                DontWait, Context);
            var terminal = false;
            lock (this)
            {
                if (attempt.Failure is null && attempt.CompletionId != 0)
                {
                    _completionId = attempt.CompletionId;
                    _state = RequestEntryState.WaitingRequest;
                    ReleasePayloadLocked();
                    if (_cancelClaimed)
                        SetCanceledLocked();
                }
                else if (attempt.Failure is not null
                         && IsBackpressured(attempt.Failure)
                         && attempt.CompletionId != 0)
                {
                    _completionId = attempt.CompletionId;
                    _state = RequestEntryState.WaitingWritable;
                    if (_cancelClaimed)
                    {
                        ReleasePayloadLocked();
                        SetCanceledLocked();
                    }
                }
                else
                {
                    var failure = attempt.Failure ?? CreateProtocolFailure();
                    _state = RequestEntryState.Terminal;
                    ReleasePayloadLocked();
                    SetExceptionLocked(failure);
                    terminal = true;
                }
            }

            if (terminal)
            {
                _cancellationRegistration.Unregister();
                Owner.Remove(this, Context);
            }
        }

        public void AbortBeforeNativeWait(Exception exception)
        {
            lock (this)
            {
                if (_state == RequestEntryState.Terminal)
                    return;
                _state = RequestEntryState.Terminal;
                Monitor.PulseAll(this);
                ReleasePayloadLocked();
                if (!Task.IsCompleted)
                {
                    if (_cancelClaimed)
                        SetCanceledLocked();
                    else
                        SetExceptionLocked(exception);
                }
            }
            _cancellationRegistration.Unregister();
            Owner.Remove(this, Context);
        }

        public void FailLifecycle()
        {
            Exception failure;
            lock (this)
                failure = _state is RequestEntryState.WaitingWritable or RequestEntryState.Retrying
                    ? new ZlinkSubmitException(SubmitResult.Terminated,
                        (int)ErrorCode.EShutdown)
                    : new ZlinkRequestException(RequestResult.Terminated);
            AbortBeforeNativeWait(failure);
        }

        public void FailRuntimeWait()
        {
            if (_state == RequestEntryState.Retrying)
            {
                AbortBeforeNativeWait(CreateStateFailure());
                return;
            }
            lock (this)
            {
                if (_state is RequestEntryState.Registered
                    or RequestEntryState.Terminal
                    or RequestEntryState.FailedWaiting)
                    return;
                var failure = CreateStateFailure();
                _state = RequestEntryState.FailedWaiting;
                ReleasePayloadLocked();
                if (!Task.IsCompleted)
                    SetExceptionLocked(failure);
            }
        }

        private void Cancel()
        {
            lock (this)
            {
                if (_state == RequestEntryState.Terminal || Task.IsCompleted)
                    return;
                _cancelClaimed = true;
                SetCanceledLocked();
                if (_state == RequestEntryState.WaitingWritable)
                    ReleasePayloadLocked();
            }
        }

        private Exception CreateStateFailure() =>
            _state is RequestEntryState.WaitingWritable
                or RequestEntryState.Retrying
                ? CreateProtocolFailure()
                : new ZlinkRequestException(RequestResult.InternalError);

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
            if (Task.IsCompleted)
                return;
            TrySetResult(_reply);
        }

        private void SetExceptionLocked(Exception exception)
        {
            if (Task.IsCompleted)
                return;
            TrySetException(exception);
        }

        private void SetCanceledLocked()
        {
            if (Task.IsCompleted)
                return;
            TrySetCanceled(_cancellationToken);
        }

        private void ReleasePayloadLocked()
        {
            if (_payloadReleased || _retained is null)
                return;
            _payloadReleased = true;
            RequestReplySupport.DisposeParts(_retained);
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

    private enum RequestEntryState
    {
        Registered,
        WaitingWritable,
        WaitingRequest,
        FailedWaiting,
        Retrying,
        Terminal
    }
}

internal readonly record struct CompletionDrainResult(
    int TotalCount, int RequestCount);
