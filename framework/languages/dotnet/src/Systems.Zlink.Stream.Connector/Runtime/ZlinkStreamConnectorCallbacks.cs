namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamConnectorCallbacks(
    ZlinkStreamTaskRunner taskRunner,
    ZlinkStreamDispatchMode dispatchMode
)
{
    private readonly object _dispatchGate = new();
    private readonly LinkedList<ZlinkStreamDispatchEntry> _dispatchQueue = new();

    private readonly ZlinkStreamHandlerList<
        Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>
    > _connectionStateChanged = new();

    private readonly ZlinkStreamHandlerList<
        Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>
    > _disconnected = new();

    private readonly ZlinkStreamHandlerList<
        Func<ZlinkStreamError, CancellationToken, ValueTask>
    > _errorReceived = new();
    private readonly ZlinkStreamHandlerList<
        Action<ZlinkStreamRequestSendingContext>
    > _requestSending = new();
    private readonly ZlinkStreamHandlerList<
        Func<ZlinkStreamReplyReceivedContext, CancellationToken, ValueTask>
    > _replyReceived = new();

    private bool _accepting = true;

    public int PendingDispatchCount
    {
        get
        {
            lock (_dispatchGate)
            {
                var count = 0;
                foreach (var entry in _dispatchQueue)
                    if (entry.PendingCallbacks > 0)
                        count++;
                return count;
            }
        }
    }

    public bool IsCurrentCallback => ZlinkStreamCallbackExecutionContext.IsActiveCallbackFor(this);

    public IDisposable EnterCallback() => ZlinkStreamCallbackExecutionContext.EnterCallback(this);

    public void Complete()
    {
        lock (_dispatchGate)
        {
            _accepting = false;
            _dispatchQueue.Clear();
        }

        _connectionStateChanged.Clear();
        _disconnected.Clear();
        _errorReceived.Clear();
        _requestSending.Clear();
        _replyReceived.Clear();
    }

    public IDisposable AddErrorReceived(
        Func<ZlinkStreamError, CancellationToken, ValueTask> handler
    ) => _errorReceived.Add(handler);

    public IDisposable AddDisconnected(
        Func<ZlinkStreamDisconnected, CancellationToken, ValueTask> handler
    ) => _disconnected.Add(handler);

    public IDisposable AddConnectionStateChanged(
        Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask> handler
    ) => _connectionStateChanged.Add(handler);

    public IDisposable AddRequestSending(Action<ZlinkStreamRequestSendingContext> handler) =>
        _requestSending.Add(handler);

    public IDisposable AddReplyReceived(
        Func<ZlinkStreamReplyReceivedContext, CancellationToken, ValueTask> handler
    ) => _replyReceived.Add(handler);

    public void NotifyRequestSending(ZlinkStreamRequestSendingContext context)
    {
        foreach (var registration in _requestSending.Snapshot())
        {
            if (registration.IsRemoved)
                continue;
            try
            {
                registration.Handler(context);
            }
            catch (Exception ex)
            {
                // Failure reporting uses the same error subscribers as dispatched callbacks.
                // It runs separately so an error handler cannot delay this request.
                try
                {
                    taskRunner.RunDetached(_ =>
                        ReportUserCallbackErrorAsync(ex, CancellationToken.None)
                    );
                }
                catch (ObjectDisposedException) { }
            }
        }
    }

    public ValueTask NotifyReplyReceivedAsync(
        ZlinkStreamReplyReceivedContext context,
        CancellationToken cancellationToken
    ) =>
        DispatchSubscribersAsync(
            _replyReceived.Snapshot(),
            (handler, token) => handler(context, token),
            cancellationToken,
            true
        );

    public async ValueTask PublishErrorAsync(
        ZlinkStreamError error,
        CancellationToken cancellationToken
    )
    {
        var handlers = _errorReceived.Snapshot();
        await DispatchSubscribersAsync(
                handlers,
                async (handler, dispatchedToken) =>
                {
                    await handler(error, dispatchedToken).ConfigureAwait(false);
                },
                cancellationToken,
                true
            )
            .ConfigureAwait(false);
    }

    private async ValueTask DispatchSubscribersAsync<THandler>(
        IReadOnlyList<ZlinkStreamHandlerList<THandler>.Registration> handlers,
        Func<THandler, CancellationToken, ValueTask> invoke,
        CancellationToken cancellationToken,
        bool reportErrors
    )
        where THandler : Delegate
    {
        if (handlers.Count == 0)
            return;

        // One entry per handler. A registration disposed after this snapshot was taken
        // neither runs on the dispatch nor counts as pending (stream-connector spec §7).
        var entries = new ZlinkStreamDispatchEntry[handlers.Count];
        for (var index = 0; index < handlers.Count; index++)
        {
            var registration = handlers[index];
            entries[index] = new CallbackEntry(
                token => invoke(registration.Handler, token),
                reportErrors,
                () => !registration.IsRemoved
            );
        }

        await DispatchEntriesAsync(entries, cancellationToken, null).ConfigureAwait(false);
    }

    public async ValueTask NotifyDisconnectedAsync(
        ZlinkStreamCloseReason closeReason,
        CancellationToken cancellationToken
    )
    {
        var handlers = _disconnected.Snapshot();
        await DispatchSubscribersAsync(
                handlers,
                async (handler, dispatchedToken) =>
                {
                    await handler(new ZlinkStreamDisconnected(closeReason), dispatchedToken)
                        .ConfigureAwait(false);
                },
                cancellationToken,
                true
            )
            .ConfigureAwait(false);
    }

    public async ValueTask NotifyConnectionStateChangedAsync(
        ZlinkStreamConnectionStateChanged change,
        CancellationToken cancellationToken
    )
    {
        var handlers = _connectionStateChanged.Snapshot();
        await DispatchSubscribersAsync(
                handlers,
                async (handler, dispatchedToken) =>
                {
                    await handler(change, dispatchedToken).ConfigureAwait(false);
                },
                cancellationToken,
                true
            )
            .ConfigureAwait(false);
    }

    /// <param name="isLive">
    ///     Whether the handler behind <paramref name="callback" /> is still registered; a
    ///     callback whose handler was removed neither runs nor counts as pending.
    /// </param>
    public ValueTask DispatchUserCallbackAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken,
        bool reportErrors = true,
        Func<bool>? isLive = null
    )
    {
        if (callback is null)
            throw new ArgumentNullException(nameof(callback));

        return DispatchEntriesAsync(
            [new CallbackEntry(callback, reportErrors, isLive)],
            cancellationToken,
            null
        );
    }

    /// <summary>
    ///     Hands <paramref name="callbacks" /> to the dispatch mode in one step and runs
    ///     <paramref name="handedOff" /> in that same step: under the dispatch queue lock in
    ///     <see cref="ZlinkStreamDispatchMode.Manual" />, before the first callback runs in
    ///     <see cref="ZlinkStreamDispatchMode.Immediate" />. What <paramref name="handedOff" />
    ///     records is therefore never visible before <c>Dispatch</c> can run the callbacks.
    /// </summary>
    public ValueTask DispatchUserCallbacksAsync(
        IReadOnlyList<Func<CancellationToken, ValueTask>> callbacks,
        CancellationToken cancellationToken,
        bool reportErrors,
        Action? handedOff
    )
    {
        var entries = new ZlinkStreamDispatchEntry[callbacks.Count];
        for (var index = 0; index < callbacks.Count; index++)
            entries[index] = new CallbackEntry(callbacks[index], reportErrors, null);
        return DispatchEntriesAsync(entries, cancellationToken, handedOff);
    }

    /// <summary>
    ///     Hands <paramref name="entries" /> to the dispatch mode and runs
    ///     <paramref name="handedOff" /> in the same step. <c>Immediate</c> dispatches each
    ///     entry now; one that has nothing to run yet - a packet no handler takes - stays
    ///     queued for a later dispatch. <c>Manual</c> queues them all for the next pump.
    /// </summary>
    public async ValueTask DispatchEntriesAsync(
        IReadOnlyList<ZlinkStreamDispatchEntry> entries,
        CancellationToken cancellationToken,
        Action? handedOff
    )
    {
        if (dispatchMode == ZlinkStreamDispatchMode.Immediate)
        {
            handedOff?.Invoke();
            foreach (var entry in entries)
            {
                Func<CancellationToken, ValueTask>? work;
                lock (_dispatchGate)
                {
                    work = entry.Take(out var keep);
                    if (work is null && keep && _accepting)
                        _dispatchQueue.AddLast(entry);
                }

                if (work is not null)
                    await InvokeUserCallbackAsync(work, cancellationToken, entry.ReportErrors)
                        .ConfigureAwait(false);
            }
            return;
        }

        lock (_dispatchGate)
        {
            if (_accepting)
                foreach (var entry in entries)
                    _dispatchQueue.AddLast(entry);
            handedOff?.Invoke();
        }
    }

    /// <summary>
    ///     A handler was registered. In <c>Immediate</c> the registration is the dispatch
    ///     point for what is queued, so the packets it now takes run here; in <c>Manual</c>
    ///     they wait for the next pump (stream-connector spec §7, §10).
    /// </summary>
    public void HandlerRegistered()
    {
        if (dispatchMode == ZlinkStreamDispatchMode.Immediate)
            DispatchAsync(CancellationToken.None).AsTask().GetAwaiter().GetResult();
    }

    internal ValueTask InvokeUserCallbackInlineAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken
    ) => InvokeUserCallbackAsync(callback, cancellationToken, reportErrors: true);

    /// <summary>
    ///     Runs the queued entries in order. Each entry decides at this point what it runs:
    ///     a packet goes to the handlers registered now, and one no handler takes stays
    ///     queued for a later handler or a wait (stream-connector spec §7, §10).
    /// </summary>
    public async ValueTask DispatchAsync(CancellationToken cancellationToken)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Func<CancellationToken, ValueTask>? work = null;
            var reportErrors = false;
            lock (_dispatchGate)
            {
                for (var node = _dispatchQueue.First; node is not null; )
                {
                    var next = node.Next;
                    work = node.Value.Take(out var keep);
                    if (work is not null || !keep)
                        _dispatchQueue.Remove(node);
                    if (work is not null)
                    {
                        reportErrors = node.Value.ReportErrors;
                        break;
                    }
                    node = next;
                }
            }

            if (work is null)
                return;
            await InvokeUserCallbackAsync(work, cancellationToken, reportErrors)
                .ConfigureAwait(false);
        }
    }

    public void QueueRequestCallback<TResult>(
        Func<ValueTask<ZlinkStreamRequestCompletion>> request,
        Func<ZlinkStreamEncodedPayload, TResult> success,
        Func<ZlinkStreamError, TResult> failure,
        Action<TResult> callback
    )
    {
        taskRunner.RunDetached(async _ =>
        {
            Func<CancellationToken, ValueTask> completion;
            try
            {
                var reply = await request().ConfigureAwait(false);
                completion = _ =>
                {
                    callback(reply.Error is { } error ? failure(error) : success(reply.Payload!));
                    return default(ValueTask);
                };
            }
            catch (ZlinkStreamException ex)
            {
                completion = _ =>
                {
                    callback(failure(ex.Error));
                    return default(ValueTask);
                };
            }
            catch (Exception ex)
            {
                // Write failures are classified by the frame sender. Other
                // exceptions retain their cause instead of claiming a write
                // failed when the request never reached the transport.
                var code = ex switch
                {
                    ArgumentException => ZlinkStreamErrorCode.ValidationFailed,
                    IOException => ZlinkStreamErrorCode.Disconnected,
                    _ => ZlinkStreamErrorCode.UserCallbackFailed,
                };
                var error = new ZlinkStreamError(code, ex.Message, ex);
                completion = _ =>
                {
                    callback(failure(error));
                    return default(ValueTask);
                };
            }

            await DispatchUserCallbackAsync(completion, CancellationToken.None)
                .ConfigureAwait(false);
        });
    }

    private ValueTask InvokeUserCallbackAsync(
        Func<CancellationToken, ValueTask> callback,
        CancellationToken cancellationToken,
        bool reportErrors
    )
    {
        using var permit = ZlinkStreamCallbackExecutionContext.EnterCallback(this);
        try
        {
            var completion = callback(cancellationToken);
            if (completion.IsCompletedSuccessfully)
                completion.GetAwaiter().GetResult();
            else
                _ = ObserveUserCallbackCompletionAsync(completion, cancellationToken, reportErrors);
        }
        catch (Exception ex)
        {
            if (reportErrors)
                _ = ReportUserCallbackErrorAsync(ex, cancellationToken);
        }
        return default;
    }

    private async Task ObserveUserCallbackCompletionAsync(
        ValueTask completion,
        CancellationToken cancellationToken,
        bool reportErrors
    )
    {
        try
        {
            await completion.ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            if (reportErrors)
                await ReportUserCallbackErrorAsync(ex, cancellationToken).ConfigureAwait(false);
        }
    }

    private async ValueTask ReportUserCallbackErrorAsync(
        Exception exception,
        CancellationToken cancellationToken
    )
    {
        var handlers = _errorReceived.Snapshot();
        if (handlers.Count == 0)
            return;

        var error = new ZlinkStreamError(
            ZlinkStreamErrorCode.UserCallbackFailed,
            "User callback failed.",
            exception
        );

        await DispatchSubscribersAsync(
                handlers,
                (handler, dispatchedToken) => handler(error, dispatchedToken),
                cancellationToken,
                false
            )
            .ConfigureAwait(false);
    }

    private sealed class CallbackEntry(
        Func<CancellationToken, ValueTask> callback,
        bool reportErrors,
        Func<bool>? isLive
    ) : ZlinkStreamDispatchEntry
    {
        public override bool ReportErrors => reportErrors;

        public override int PendingCallbacks => isLive is null || isLive() ? 1 : 0;

        public override Func<CancellationToken, ValueTask>? Take(out bool keep)
        {
            keep = false;
            return isLive is null || isLive() ? callback : null;
        }
    }
}

/// <summary>
///     One item of the dispatch queue: a callback, or a received packet whose handlers are
///     decided when it is dispatched (stream-connector spec §7, §10).
/// </summary>
internal abstract class ZlinkStreamDispatchEntry
{
    public abstract bool ReportErrors { get; }

    /// <summary>Callbacks the entry runs if it is dispatched with the handlers registered now.</summary>
    public abstract int PendingCallbacks { get; }

    /// <summary>
    ///     Returns what the entry runs when it is dispatched now, or <see langword="null" />
    ///     with <paramref name="keep" /> telling whether it stays queued for a later dispatch.
    /// </summary>
    public abstract Func<CancellationToken, ValueTask>? Take(out bool keep);
}

/// <summary>
///     Ordered set of lifecycle handlers where registering hands back the value that
///     removes the registration (stream-connector spec §7).
/// </summary>
/// <remarks>
///     A C# <c>event</c> cannot satisfy that contract: <c>+=</c> returns nothing, so a
///     client whose subscription lives as long as one screen has to keep the delegate
///     itself to unsubscribe later.
/// </remarks>
internal sealed class ZlinkStreamHandlerList<THandler>
    where THandler : Delegate
{
    private readonly object _gate = new();
    private Registration[] _handlers = [];

    public IDisposable Add(THandler handler)
    {
        if (handler is null)
            throw new ArgumentNullException(nameof(handler));
        var registration = new Registration(handler);

        lock (_gate)
        {
            var next = new Registration[_handlers.Length + 1];
            Array.Copy(_handlers, next, _handlers.Length);
            next[^1] = registration;
            _handlers = next;
        }

        return new Subscription(() => Remove(registration));
    }

    /// <summary>
    ///     Returns the registrations in registration order. Each one still reports whether
    ///     it has been removed, so a handler disposed between this snapshot and a later
    ///     manual dispatch is skipped instead of invoked.
    /// </summary>
    /// <remarks>
    ///     Every write publishes a new array, so reading the reference is the whole
    ///     snapshot and a lock would add nothing to it.
    /// </remarks>
    public IReadOnlyList<Registration> Snapshot()
    {
        return Volatile.Read(ref _handlers);
    }

    public void Clear()
    {
        lock (_gate)
        {
            foreach (var registration in _handlers)
                registration.Remove();
            _handlers = [];
        }
    }

    private void Remove(Registration registration)
    {
        lock (_gate)
        {
            registration.Remove();
            var index = Array.IndexOf(_handlers, registration);
            if (index < 0)
                return;

            var next = new Registration[_handlers.Length - 1];
            if (index > 0)
                Array.Copy(_handlers, 0, next, 0, index);
            if (index < _handlers.Length - 1)
                Array.Copy(_handlers, index + 1, next, index, _handlers.Length - index - 1);

            _handlers = next;
        }
    }

    internal sealed class Registration(THandler handler)
    {
        private int _removed;

        public THandler Handler { get; } = handler;

        public bool IsRemoved => Volatile.Read(ref _removed) != 0;

        public void Remove() => Volatile.Write(ref _removed, 1);
    }

    private sealed class Subscription(Action dispose) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
                dispose();
        }
    }
}
