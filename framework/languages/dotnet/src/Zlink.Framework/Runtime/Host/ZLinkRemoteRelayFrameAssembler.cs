using Zlink.Framework.Runtime.Execution;

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

    private readonly ZLinkStateLane _lane = new();
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

    internal async ValueTask<ZLinkRemoteRelayFrameAppendResult> TryAppendAsync(
        ZLinkRemoteRelayFrameKey key,
        byte[] part,
        bool hasMore)
    {
        ArgumentNullException.ThrowIfNull(part);
        var shutdownToken = _getShutdownToken();
        var prepared = await _lane.RunAsync(() =>
        {
            if (_disposed || shutdownToken.IsCancellationRequested)
                return new AppendPreparation(new(false, null), null);

            if (!_pending.TryGetValue(key, out var pending))
            {
                if (!hasMore)
                {
                    if (part.LongLength > MaxFrameBytes)
                        return new AppendPreparation(new(false, null), null);
                    return new AppendPreparation(
                        new(true, new CompletedFrame(key, null, [part])),
                        null);
                }
                if (_pending.Count >= MaxAssemblies
                    || part.LongLength > MaxFrameBytes
                    || _bufferedBytes + part.LongLength > MaxBufferedBytes)
                    return new AppendPreparation(new(false, null), null);

                pending = new PendingFrame();
                _pending.Add(key, pending);
                pending.Parts.Add(part);
                pending.Bytes = part.LongLength;
                _bufferedBytes += part.LongLength;
                return new AppendPreparation(
                    new(true, null),
                    new ExpiryStart(key, pending, shutdownToken, pending.Cancellation.Token));
            }

            if (pending.Completing)
                return new AppendPreparation(new(false, null), null);

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
                    return new AppendPreparation(new(false, null), null);

                _bufferedBytes = retainedWithoutPrefix + part.LongLength;
                pending.Parts.Clear();
                pending.Parts.Add(part);
                pending.Bytes = part.LongLength;
                pending.RestartOnNextPrefix = false;
                return new AppendPreparation(new(true, null), null);
            }

            if (pending.Parts.Count + 1 > MaxPartsPerFrame
                || pending.Bytes + part.LongLength > MaxFrameBytes
                || _bufferedBytes + part.LongLength > MaxBufferedBytes)
                return new AppendPreparation(new(false, null), null);

            if (hasMore)
            {
                if (_bufferedBytes + part.LongLength > MaxBufferedBytes)
                    return new AppendPreparation(new(false, null), null);
                pending.Parts.Add(part);
                pending.Bytes += part.LongLength;
                _bufferedBytes += part.LongLength;
                return new AppendPreparation(new(true, null), null);
            }

            pending.RestartOnNextPrefix = false;
            pending.Completing = true;
            pending.TerminalBytes = part.LongLength;
            _bufferedBytes += part.LongLength;
            var parts = new byte[pending.Parts.Count + 1][];
            pending.Parts.CopyTo(parts, 0);
            parts[^1] = part;
            return new AppendPreparation(
                new(true, new CompletedFrame(key, pending, parts)),
                null);
        }).ConfigureAwait(false);
        if (prepared.Expiry is { } expiry)
            ArmExpiry(expiry);
        return prepared.Result;
    }

    internal ValueTask CommitAsync(CompletedFrame completed)
    {
        ArgumentNullException.ThrowIfNull(completed);
        if (completed.Pending is null)
            return ValueTask.CompletedTask;
        return _lane.RunAsync(() =>
        {
            if (_pending.TryGetValue(completed.Key, out var current)
                && ReferenceEquals(current, completed.Pending))
                Remove(completed.Key, current);
        });
    }

    internal ValueTask RejectAsync(CompletedFrame completed)
    {
        ArgumentNullException.ThrowIfNull(completed);
        if (completed.Pending is null)
            return ValueTask.CompletedTask;
        return _lane.RunAsync(() =>
        {
            if (_pending.TryGetValue(completed.Key, out var current)
                && ReferenceEquals(current, completed.Pending))
            {
                current.Completing = false;
                _bufferedBytes -= current.TerminalBytes;
                current.TerminalBytes = 0;
                current.RestartOnNextPrefix = true;
            }
        });
    }

    internal async ValueTask ClearAsync()
    {
        var removed = await _lane.RunAsync(() =>
        {
            var cleared = _pending.Values.ToArray();
            _pending.Clear();
            _bufferedBytes = 0;
            return cleared;
        }).ConfigureAwait(false);
        foreach (var pending in removed)
            pending.Cancel();
    }

    public void Dispose()
    {
        var removed = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disposed)
                return Array.Empty<PendingFrame>();
            _disposed = true;
            var cleared = _pending.Values.ToArray();
            _pending.Clear();
            _bufferedBytes = 0;
            return cleared;
        }));
        foreach (var pending in removed)
            pending.Cancel();
        AwaitStateLane(_lane.DisposeAsync());
    }

    private void ArmExpiry(ExpiryStart expiry)
    {
        using (ExecutionContext.SuppressFlow())
            _ = Task.Run(() => ExpireAsync(expiry));
    }

    private async Task ExpireAsync(ExpiryStart expiry)
    {
        using var linkedExpiry = CancellationTokenSource.CreateLinkedTokenSource(
            expiry.ShutdownToken,
            expiry.PendingToken);
        try
        {
            await Task.Delay(_timeout, linkedExpiry.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        try
        {
            await _lane.RunAsync(() =>
            {
                if (_pending.TryGetValue(expiry.Key, out var current)
                    && ReferenceEquals(current, expiry.Pending))
                    Remove(expiry.Key, current);
            }).ConfigureAwait(false);
        }
        catch (ObjectDisposedException) { }
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

    private readonly record struct AppendPreparation(
        ZLinkRemoteRelayFrameAppendResult Result,
        ExpiryStart? Expiry);

    private readonly record struct ExpiryStart(
        ZLinkRemoteRelayFrameKey Key,
        PendingFrame Pending,
        CancellationToken ShutdownToken,
        CancellationToken PendingToken);

    internal sealed class PendingFrame
    {
        internal List<byte[]> Parts { get; } = [];
        internal CancellationTokenSource Cancellation { get; } = new();
        internal long Bytes { get; set; }
        internal long TerminalBytes { get; set; }
        internal bool Completing { get; set; }
        internal bool RestartOnNextPrefix { get; set; }
        internal void Cancel()
        {
            Cancellation.Cancel();
            Cancellation.Dispose();
        }
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();
}

internal readonly record struct ZLinkRemoteRelayFrameAppendResult(
    bool Accepted,
    ZLinkRemoteRelayFrameAssembler.CompletedFrame? Completed);
