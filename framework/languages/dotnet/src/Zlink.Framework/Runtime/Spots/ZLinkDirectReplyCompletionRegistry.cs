using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Spots;

// Owns only direct-reply identity, bounded admission, atomic removal, and
// terminal retention. Message Follow forwarding and reply transport retries do
// not participate in this state machine.
internal sealed class ZLinkDirectReplyCompletionRegistry<TKey, TValue>
    where TKey : notnull
    where TValue : class
{
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<TKey, TValue> _pending = [];
    private readonly Dictionary<TKey, long> _terminals = [];
    private readonly Queue<TKey> _terminalOrder = [];
    private readonly int _capacity;
    private readonly TimeSpan _terminalRetention;
    private readonly TimeProvider _time;

    internal ZLinkDirectReplyCompletionRegistry(
        int capacity,
        TimeSpan terminalRetention,
        TimeProvider? timeProvider = null)
    {
        if (capacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        if (terminalRetention <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(terminalRetention));
        _capacity = capacity;
        _terminalRetention = terminalRetention;
        _time = timeProvider ?? TimeProvider.System;
    }

    internal bool TryRegister(TKey key, TValue value)
    {
        ArgumentNullException.ThrowIfNull(value);
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            RemoveExpiredTerminalsUnderLock(_time.GetTimestamp());
            if (_terminals.ContainsKey(key)
                || _pending.Count >= _capacity
                || _pending.ContainsKey(key))
                return false;
            _pending.Add(key, value);
            return true;
        }));
    }

    internal TValue? TryGet(TKey key)
    {
        return AwaitStateLane(_lane.RunAsync(() =>
            _pending.TryGetValue(key, out var value) ? value : null));
    }

    internal bool TryRemove(
        TKey key,
        TValue expected,
        bool rememberTerminal)
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_pending.TryGetValue(key, out var current)
                || !ReferenceEquals(current, expected))
                return false;
            _pending.Remove(key);
            if (rememberTerminal)
                RememberTerminalUnderLock(key, _time.GetTimestamp());
            return true;
        }));
    }

    private void RememberTerminalUnderLock(TKey key, long now)
    {
        RemoveExpiredTerminalsUnderLock(now);
        if (_terminals.TryAdd(key, now))
            _terminalOrder.Enqueue(key);
        else
            _terminals[key] = now;
        while (_terminals.Count > _capacity
               && _terminalOrder.TryDequeue(out var evicted))
            _terminals.Remove(evicted);
    }

    private void RemoveExpiredTerminalsUnderLock(long now)
    {
        while (_terminalOrder.TryPeek(out var oldest)
               && (!_terminals.TryGetValue(oldest, out var retainedAt)
                   || _time.GetElapsedTime(retainedAt, now) >= _terminalRetention))
        {
            _terminalOrder.Dequeue();
            _terminals.Remove(oldest);
        }
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
}
