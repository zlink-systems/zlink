namespace Zlink.Framework.Runtime.Host;

internal readonly record struct ZLinkRemoteRelayFrameKey(
    byte RouteKind,
    string ActorId,
    ulong ActorGeneration,
    string BindingIdentity,
    string SourceNodeRid,
    ulong SourceNodeGeneration,
    string SourceSessionRid,
    string RequestSourceOwnerId,
    ulong RequestSourceLeaseGeneration,
    string RequestSourceNodeRid,
    ulong RequestSourceNodeGeneration,
    ulong OperationHigh,
    ulong OperationLow,
    ulong ReplyRequestId,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration);

internal sealed class ZLinkRemoteRelayFrameAssembler : IDisposable
{
    private const int MaxAssemblies = 1_024;
    private const int MaxPartsPerFrame = 1_024;
    private const long MaxFrameBytes = 16L * 1024 * 1024;
    private const long MaxBufferedBytes = 16L * 1024 * 1024;

    private readonly object _gate = new();
    private readonly Dictionary<ZLinkRemoteRelayFrameKey, PendingFrame> _pending = [];
    private readonly TimeSpan _timeout;
    private readonly Func<CancellationToken> _getShutdownToken;
    private long _bufferedBytes;
    private bool _disposed;

    internal ZLinkRemoteRelayFrameAssembler(
        TimeSpan timeout,
        Func<CancellationToken> getShutdownToken)
    {
        _timeout = timeout;
        _getShutdownToken = getShutdownToken;
    }

    internal bool TryAppend(
        ZLinkRemoteRelayFrameKey key,
        byte[] part,
        bool hasMore,
        out CompletedFrame? completed)
    {
        ArgumentNullException.ThrowIfNull(part);
        completed = null;
        lock (_gate)
        {
            if (_disposed || _getShutdownToken().IsCancellationRequested)
                return false;

            if (!_pending.TryGetValue(key, out var pending))
            {
                if (!hasMore)
                {
                    if (part.LongLength > MaxFrameBytes)
                        return false;
                    completed = new CompletedFrame(key, null, [part]);
                    return true;
                }
                if (_pending.Count >= MaxAssemblies
                    || part.LongLength > MaxFrameBytes
                    || _bufferedBytes + part.LongLength > MaxBufferedBytes)
                    return false;

                pending = new PendingFrame();
                _pending.Add(key, pending);
                pending.Parts.Add(part);
                pending.Bytes = part.LongLength;
                _bufferedBytes += part.LongLength;
                pending.Expiry = ExpireAsync(key, pending);
                return true;
            }

            if (pending.Completing)
                return false;

            // A failed terminal submit can be retried in either form used by
            // the runtime: the Message Follow worker resubmits only its terminal
            // part, while the session coordinator resubmits the full frame.
            // A new non-terminal part starts that full-frame retry and replaces
            // the retained prefix so retry policy does not leak to either caller.
            if (pending.RestartOnNextPrefix && hasMore)
            {
                var retainedWithoutPrefix = _bufferedBytes - pending.Bytes;
                if (part.LongLength > MaxFrameBytes
                    || retainedWithoutPrefix + part.LongLength > MaxBufferedBytes)
                    return false;

                _bufferedBytes = retainedWithoutPrefix + part.LongLength;
                pending.Parts.Clear();
                pending.Parts.Add(part);
                pending.Bytes = part.LongLength;
                pending.RestartOnNextPrefix = false;
                return true;
            }

            if (pending.Parts.Count + 1 > MaxPartsPerFrame
                || pending.Bytes + part.LongLength > MaxFrameBytes
                || _bufferedBytes + part.LongLength > MaxBufferedBytes)
                return false;

            if (hasMore)
            {
                if (_bufferedBytes + part.LongLength > MaxBufferedBytes)
                    return false;
                pending.Parts.Add(part);
                pending.Bytes += part.LongLength;
                _bufferedBytes += part.LongLength;
                return true;
            }

            pending.RestartOnNextPrefix = false;
            pending.Completing = true;
            pending.TerminalBytes = part.LongLength;
            _bufferedBytes += part.LongLength;
            var parts = new byte[pending.Parts.Count + 1][];
            pending.Parts.CopyTo(parts, 0);
            parts[^1] = part;
            completed = new CompletedFrame(key, pending, parts);
            return true;
        }
    }

    internal void Commit(CompletedFrame completed)
    {
        ArgumentNullException.ThrowIfNull(completed);
        if (completed.Pending is null)
            return;
        lock (_gate)
        {
            if (_pending.TryGetValue(completed.Key, out var current)
                && ReferenceEquals(current, completed.Pending))
                Remove(completed.Key, current);
        }
    }

    internal void Reject(CompletedFrame completed)
    {
        ArgumentNullException.ThrowIfNull(completed);
        if (completed.Pending is null)
            return;
        lock (_gate)
        {
            if (_pending.TryGetValue(completed.Key, out var current)
                && ReferenceEquals(current, completed.Pending))
            {
                current.Completing = false;
                _bufferedBytes -= current.TerminalBytes;
                current.TerminalBytes = 0;
                current.RestartOnNextPrefix = true;
            }
        }
    }

    internal void Clear()
    {
        PendingFrame[] removed;
        lock (_gate)
        {
            removed = _pending.Values.ToArray();
            _pending.Clear();
            _bufferedBytes = 0;
        }
        foreach (var pending in removed)
            pending.Cancel();
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed)
                return;
            _disposed = true;
        }
        Clear();
    }

    private async Task ExpireAsync(
        ZLinkRemoteRelayFrameKey key,
        PendingFrame expected)
    {
        using var expiry = CancellationTokenSource.CreateLinkedTokenSource(
            _getShutdownToken(),
            expected.Cancellation.Token);
        try
        {
            await Task.Delay(_timeout, expiry.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        lock (_gate)
        {
            if (_pending.TryGetValue(key, out var current)
                && ReferenceEquals(current, expected))
                Remove(key, current);
        }
    }

    private void Remove(
        ZLinkRemoteRelayFrameKey key,
        PendingFrame pending)
    {
        _pending.Remove(key);
        _bufferedBytes -= pending.Bytes + pending.TerminalBytes;
        pending.Cancel();
    }

    internal sealed class CompletedFrame(
        ZLinkRemoteRelayFrameKey key,
        PendingFrame? pending,
        byte[][] parts)
    {
        internal ZLinkRemoteRelayFrameKey Key { get; } = key;
        internal PendingFrame? Pending { get; } = pending;
        internal IReadOnlyList<byte[]> Parts { get; } = parts;
    }

    internal sealed class PendingFrame
    {
        internal List<byte[]> Parts { get; } = [];
        internal CancellationTokenSource Cancellation { get; } = new();
        internal long Bytes { get; set; }
        internal long TerminalBytes { get; set; }
        internal bool Completing { get; set; }
        internal bool RestartOnNextPrefix { get; set; }
        internal Task? Expiry { get; set; }

        internal void Cancel()
        {
            Cancellation.Cancel();
            Cancellation.Dispose();
        }
    }
}
