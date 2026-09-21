namespace Systems.Zlink.Stream.Connector.Runtime;

/// <summary>
///     Unread receive history plus the per-packet-name arrival counters behind
///     <see cref="IZlinkStreamConnector.ReceivedCount" />.
/// </summary>
/// <remarks>
///     The two are deliberately separate. The history holds what no handler took and a
///     wait surface consumes from it, while the counters record how many packets of a
///     name arrived on the current connection and never fall when something consumes one
///     (stream-connector spec §10).
/// </remarks>
internal sealed class ZlinkStreamReceivedMessages
{
    private readonly object _gate = new();

    private readonly Dictionary<
        string,
        List<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>
    > _messages = new(StringComparer.Ordinal);

    private readonly Dictionary<string, int> _counts = new(StringComparer.Ordinal);
    private TaskCompletionSource _arrived = new(TaskCreationOptions.RunContinuationsAsynchronously);

    /// <summary>
    ///     Advances on every change to the history. A wait picks its message from a copy
    ///     taken under the lock and takes it only while this value still matches, so the
    ///     predicate never runs with the lock held.
    /// </summary>
    private long _version;

    /// <summary>
    ///     Generation of the connection a wait observes: the one
    ///     <see cref="ResetForConnection" /> last established, or 0 once
    ///     <see cref="ConnectionEnded" /> has ended it and until the next one is
    ///     established.
    /// </summary>
    private long _connectionGeneration;

    /// <summary>
    ///     Highest generation <see cref="ResetForConnection" /> has applied. Kept apart from
    ///     <see cref="_connectionGeneration" /> so that an ending, which sets that one back
    ///     to 0, does not let a superseded attach re-establish an older generation.
    /// </summary>
    private long _establishedGeneration;

    /// <summary>
    ///     Number of messages with <paramref name="name" /> that arrived on the current
    ///     connection, whichever surface took them and whichever dispatch mode is in use.
    /// </summary>
    public int Count(string name)
    {
        lock (_gate)
        {
            return _counts.GetValueOrDefault(name);
        }
    }

    /// <summary>
    ///     Counts one arrival. Called for every received message, including the ones an
    ///     <c>On</c> handler takes and therefore never enter the unread history.
    /// </summary>
    public void CountArrival(string name)
    {
        lock (_gate)
        {
            _counts[name] = _counts.GetValueOrDefault(name) + 1;
        }
    }

    /// <summary>
    ///     Rebaselines the history on a connection that is established: every counter
    ///     restarts at zero and what the previous connection left unconsumed is dropped
    ///     (stream-connector spec §10).
    /// </summary>
    /// <param name="connectionGeneration">
    ///     Monotonic number of the connection being established. A call carrying a
    ///     generation that is not newer than the applied one comes from an attach another
    ///     connection already superseded, and does nothing.
    /// </param>
    /// <remarks>
    ///     Resetting the counters alone would let the counters and the history describe
    ///     different connections, and a wait would take a packet that arrived before the
    ///     drop as one of the new connection's (spec §10). Waits are not ended here: the
    ///     ones of the previous connection ended with it in <see cref="ConnectionEnded" />,
    ///     and a wait started since then wakes from here to observe this connection.
    /// </remarks>
    public void ResetForConnection(long connectionGeneration)
    {
        TaskCompletionSource arrived;
        lock (_gate)
        {
            if (connectionGeneration <= _establishedGeneration)
                return;

            _establishedGeneration = connectionGeneration;
            _connectionGeneration = connectionGeneration;
            _counts.Clear();
            _messages.Clear();
            _version++;
            arrived = _arrived;
            _arrived = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        }

        arrived.TrySetResult();
    }

    /// <summary>
    ///     Records that the connection the waits observe has ended — a transport loss, a
    ///     server close, or <c>Close</c> — and wakes them so they end as
    ///     <see cref="ZlinkStreamErrorCode.Disconnected" />.
    /// </summary>
    /// <remarks>
    ///     Stream-connector spec §10.1.1: the release belongs to the ending of the
    ///     connection, not to the establishment of the next one. Released here, a wait
    ///     does not hang until its own timeout when no next connection comes (reconnect
    ///     off, attempts spent) and does not rebind to the next one when it does. The
    ///     history and the counters stay: they are rebaselined by the next
    ///     <see cref="ResetForConnection" />, not by the ending (spec §10).
    /// </remarks>
    public void ConnectionEnded()
    {
        TaskCompletionSource arrived;
        lock (_gate)
        {
            if (_connectionGeneration == 0)
                return;

            _connectionGeneration = 0;
            arrived = _arrived;
            _arrived = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        }

        arrived.TrySetResult();
    }

    public void Record(ZlinkStreamMessage<ZlinkStreamEncodedPayload> message)
    {
        TaskCompletionSource arrived;
        lock (_gate)
        {
            if (!_messages.TryGetValue(message.Name, out var messages))
            {
                messages = [];
                _messages.Add(message.Name, messages);
            }

            messages.Add(message);
            _version++;
            arrived = _arrived;
            _arrived = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        }

        arrived.TrySetResult();
    }

    /// <summary>
    ///     Consumes the first unread message matching <paramref name="name" /> and
    ///     <paramref name="predicate" />, or returns <see langword="null" /> when
    ///     <paramref name="timeout" /> elapses first.
    /// </summary>
    /// <remarks>
    ///     A timeout is not an exception here. Each wait surface decides what its own
    ///     timeout means — <c>WaitFor</c> fails on it while <c>ExpectNone</c> succeeds —
    ///     and reports that decision as <see cref="ZlinkStreamErrorCode.ValidationFailed" />
    ///     (stream-connector spec §10.1). Losing the connection the wait observes is the
    ///     one case this method decides itself, because the wait has no place left to
    ///     observe: that is <see cref="ZlinkStreamErrorCode.Disconnected" /> for every
    ///     surface (spec §10.1).
    /// </remarks>
    public async ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>?> WaitForAsync(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        using var timeoutSource = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken
        );
        timeoutSource.CancelAfter(timeout);

        long observedGeneration;
        lock (_gate)
        {
            observedGeneration = _connectionGeneration;
        }

        while (!timeoutSource.IsCancellationRequested)
        {
            var pending = TryTakeOrWait(name, predicate, observedGeneration, timeoutSource.Token);
            if (pending.Message is not null)
                return pending.Message;

            if (pending.ConnectionLost)
            {
                if (observedGeneration == 0)
                {
                    // The wait started before any connection was established, so it has
                    // no earlier connection to lose: the first one to be established is
                    // the connection it observes.
                    lock (_gate)
                    {
                        observedGeneration = _connectionGeneration;
                    }

                    continue;
                }

                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.Disconnected,
                    $"The connection this wait for '{name}' observed ended before the message arrived."
                );
            }

            try
            {
                await pending.WaitTask!.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (timeoutSource.IsCancellationRequested
                    && !cancellationToken.IsCancellationRequested
                )
            {
                break;
            }
        }

        cancellationToken.ThrowIfCancellationRequested();
        return null;
    }

    private PendingMessage TryTakeOrWait(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate,
        long observedGeneration,
        CancellationToken cancellationToken
    )
    {
        while (true)
        {
            long observedVersion;
            TaskCompletionSource arrived;
            ZlinkStreamMessage<ZlinkStreamEncodedPayload>[] candidates;
            lock (_gate)
            {
                if (_connectionGeneration != observedGeneration)
                    return PendingMessage.ConnectionEnded;

                observedVersion = _version;
                arrived = _arrived;
                candidates = _messages.TryGetValue(name, out var messages) ? [.. messages] : [];
            }

            // The predicate is caller code, and on the typed surface it decodes the
            // payload first. It runs here, outside the lock the receive path takes for
            // every arrival, so a slow predicate delays this wait and nothing else.
            var index = IndexOfMatch(candidates, predicate);
            if (index < 0)
                return new PendingMessage(null, arrived.Task.WaitAsync(cancellationToken), false);

            lock (_gate)
            {
                // The history changed while the predicate ran, so the copy the choice was
                // made from no longer describes it. Choose again from what is there now.
                if (_version != observedVersion)
                    continue;

                var messages = _messages[name];
                var message = messages[index];
                messages.RemoveAt(index);
                if (messages.Count == 0)
                    _messages.Remove(name);
                _version++;
                return new PendingMessage(message, null, false);
            }
        }
    }

    private static int IndexOfMatch(
        ZlinkStreamMessage<ZlinkStreamEncodedPayload>[] candidates,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate
    )
    {
        for (var index = 0; index < candidates.Length; index++)
            if (predicate is null || predicate(candidates[index]))
                return index;

        return -1;
    }

    private readonly record struct PendingMessage(
        ZlinkStreamMessage<ZlinkStreamEncodedPayload>? Message,
        Task? WaitTask,
        bool ConnectionLost
    )
    {
        internal static PendingMessage ConnectionEnded { get; } = new(null, null, true);
    }
}
