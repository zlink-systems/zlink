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
    internal object SyncRoot { get; } = new();

    private readonly Dictionary<string, LinkedList<ReceiveEntry>> _messages = new(
        StringComparer.Ordinal
    );

    private readonly Dictionary<string, int> _counts = new(StringComparer.Ordinal);
    private TaskCompletionSource<bool> _arrived = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );

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
        lock (SyncRoot)
        {
            return _counts.GetValueOrDefault(name);
        }
    }

    private void CountLocked(string name) => _counts[name] = _counts.GetValueOrDefault(name) + 1;

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
        TaskCompletionSource<bool> arrived;
        lock (SyncRoot)
        {
            if (connectionGeneration <= _establishedGeneration)
                return;

            _establishedGeneration = connectionGeneration;
            _connectionGeneration = connectionGeneration;
            _counts.Clear();
            // Reset removes the dispatch index before detaching each receive index.
            foreach (var messages in _messages.Values)
            {
                foreach (var entry in messages)
                    entry.RemoveQueued();
                messages.Clear();
            }
            _messages.Clear();
            arrived = _arrived;
            _arrived = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
        }

        arrived.TrySetResult(true);
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
        TaskCompletionSource<bool> arrived;
        lock (SyncRoot)
        {
            if (_connectionGeneration == 0)
                return;

            _connectionGeneration = 0;
            arrived = _arrived;
            _arrived = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
        }

        arrived.TrySetResult(true);
    }

    /// <summary>
    ///     Keeps an arrival in the receive queue and counts it in the same step, so a counted
    ///     message is already observable to a wait (stream-connector spec §10). It stays until
    ///     a handler's dispatch or a wait takes it; the returned node names it for the
    ///     dispatch.
    /// </summary>
    public LinkedListNode<ReceiveEntry> Record(
        ZlinkStreamMessage<ZlinkStreamEncodedPayload> message
    ) => Record(new ReceiveEntry(message));

    public LinkedListNode<ReceiveEntry> Record(ReceiveEntry entry)
    {
        var message = entry.Message;
        TaskCompletionSource<bool> arrived;
        LinkedListNode<ReceiveEntry> node;
        lock (SyncRoot)
        {
            if (!_messages.TryGetValue(message.Name, out var messages))
            {
                messages = new LinkedList<ReceiveEntry>();
                _messages.Add(message.Name, messages);
            }

            node = messages.AddLast(entry);
            entry.Node = node;
            CountLocked(message.Name);
            arrived = _arrived;
            _arrived = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
        }

        arrived.TrySetResult(true);
        return node;
    }

    /// <summary>Whether no dispatch or wait has taken the message at <paramref name="node" />.</summary>
    public bool IsUnread(LinkedListNode<ReceiveEntry> node)
    {
        lock (SyncRoot)
            return node.List is not null;
    }

    /// <summary>
    ///     Takes the message at <paramref name="node" /> for a dispatch, or returns
    ///     <see langword="false" /> when a wait or the next connection took it first.
    /// </summary>
    public bool TryTake(LinkedListNode<ReceiveEntry> node)
    {
        lock (SyncRoot)
        {
            if (node.List is null)
                return false;
            RemoveLocked(node);
            return true;
        }
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
        lock (SyncRoot)
        {
            observedGeneration = _connectionGeneration;
        }

        // The last message this wait examined and rejected. The next scan continues after it,
        // so each unread message meets the predicate once while messages keep arriving.
        LinkedListNode<ReceiveEntry>? examined = null;
        while (!timeoutSource.IsCancellationRequested)
        {
            var pending = TryTakeOrWait(
                name,
                predicate,
                observedGeneration,
                ref examined,
                timeoutSource.Token
            );
            if (pending.Message is not null)
                return pending.Message;

            if (pending.ConnectionLost)
            {
                if (observedGeneration == 0)
                {
                    // The wait started before any connection was established, so it has
                    // no earlier connection to lose: the first one to be established is
                    // the connection it observes.
                    lock (SyncRoot)
                    {
                        observedGeneration = _connectionGeneration;
                    }

                    examined = null;
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

    /// <summary>
    ///     Takes the first unread message after <paramref name="examined" /> that matches, or
    ///     returns the arrival to wait for when none does.
    /// </summary>
    /// <remarks>
    ///     The predicate is caller code, and on the typed surface it decodes the payload
    ///     first. It runs outside the lock the receive path takes for every arrival, so a
    ///     slow predicate delays this wait and nothing else. An arrival appends after the
    ///     scan position and does not restart the scan. Only another wait taking the message
    ///     at that position does, because the position then has no successor to continue
    ///     from.
    /// </remarks>
    private PendingMessage TryTakeOrWait(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate,
        long observedGeneration,
        ref LinkedListNode<ReceiveEntry>? examined,
        CancellationToken cancellationToken
    )
    {
        LinkedListNode<ReceiveEntry>? cursor;
        lock (SyncRoot)
        {
            if (_connectionGeneration != observedGeneration)
                return PendingMessage.ConnectionEnded;

            cursor = NextCandidateLocked(name, examined);
            if (cursor is null)
                return new PendingMessage(null, _arrived.Task.WaitAsync(cancellationToken), false);
        }

        while (true)
        {
            var matches = predicate is null || predicate(cursor.Value.Message);
            lock (SyncRoot)
            {
                if (_connectionGeneration != observedGeneration)
                    return PendingMessage.ConnectionEnded;

                // A node still in a list is still unread; another wait may have taken it
                // while the predicate ran.
                if (cursor.List is not null)
                {
                    if (matches)
                    {
                        RemoveLocked(cursor);
                        return new PendingMessage(cursor.Value.Message, null, false);
                    }

                    examined = cursor;
                }

                cursor = NextCandidateLocked(name, examined);
                if (cursor is null)
                    return new PendingMessage(
                        null,
                        _arrived.Task.WaitAsync(cancellationToken),
                        false
                    );
            }
        }
    }

    private LinkedListNode<ReceiveEntry>? NextCandidateLocked(
        string name,
        LinkedListNode<ReceiveEntry>? examined
    )
    {
        if (examined?.List is not null)
            return examined.Next;
        return _messages.TryGetValue(name, out var messages) ? messages.First : null;
    }

    private void RemoveLocked(LinkedListNode<ReceiveEntry> node)
    {
        var messages = node.List!;
        messages.Remove(node);
        node.Value.RemoveQueued();
        if (messages.Count == 0)
            _messages.Remove(node.Value.Message.Name);
    }

    // The packet owns its message once; both queue nodes are removable indexes.
    internal class ReceiveEntry(ZlinkStreamMessage<ZlinkStreamEncodedPayload> message)
        : ZlinkStreamDispatchEntry
    {
        public ZlinkStreamMessage<ZlinkStreamEncodedPayload> Message { get; } = message;

        public LinkedListNode<ReceiveEntry>? Node { get; set; }

        public override bool ReportErrors => false;

        public override int PendingCallbacks => 0;

        public override Func<CancellationToken, ValueTask>? Take(out bool keep)
        {
            keep = Node?.List is not null;
            return null;
        }
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
