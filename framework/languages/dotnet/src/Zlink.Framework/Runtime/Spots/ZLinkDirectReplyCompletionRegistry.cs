namespace Zlink.Framework.Runtime.Spots;

// Owns only direct-reply identity, bounded admission, atomic removal, and
// terminal retention. Message Follow forwarding and reply transport retries do
// not participate in this state machine.
internal sealed class ZLinkDirectReplyCompletionRegistry<TKey, TValue>
    where TKey : notnull
    where TValue : class
{
    private readonly object _gate = new();
    private readonly Dictionary<TKey, TValue> _pending = [];
    private readonly Dictionary<TKey, DateTimeOffset> _terminals = [];
    private readonly Queue<TKey> _terminalOrder = [];
    private readonly int _capacity;
    private readonly TimeSpan _terminalRetention;

    internal ZLinkDirectReplyCompletionRegistry(
        int capacity,
        TimeSpan terminalRetention)
    {
        if (capacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        if (terminalRetention <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(terminalRetention));
        _capacity = capacity;
        _terminalRetention = terminalRetention;
    }

    internal bool TryRegister(TKey key, TValue value)
    {
        ArgumentNullException.ThrowIfNull(value);
        lock (_gate)
        {
            RemoveExpiredTerminalsUnderLock(DateTimeOffset.UtcNow);
            if (_terminals.ContainsKey(key)
                || _pending.Count >= _capacity
                || _pending.ContainsKey(key))
                return false;
            _pending.Add(key, value);
            return true;
        }
    }

    internal bool TryGet(TKey key, out TValue value)
    {
        lock (_gate)
            return _pending.TryGetValue(key, out value!);
    }

    internal bool TryRemove(
        TKey key,
        TValue expected,
        bool rememberTerminal)
    {
        lock (_gate)
        {
            if (!_pending.TryGetValue(key, out var current)
                || !ReferenceEquals(current, expected))
                return false;
            _pending.Remove(key);
            if (rememberTerminal)
                RememberTerminalUnderLock(key, DateTimeOffset.UtcNow);
            return true;
        }
    }

    private void RememberTerminalUnderLock(TKey key, DateTimeOffset now)
    {
        RemoveExpiredTerminalsUnderLock(now);
        if (_terminals.TryAdd(key, now + _terminalRetention))
            _terminalOrder.Enqueue(key);
        else
            _terminals[key] = now + _terminalRetention;
        while (_terminals.Count > _capacity
               && _terminalOrder.TryDequeue(out var evicted))
            _terminals.Remove(evicted);
    }

    private void RemoveExpiredTerminalsUnderLock(DateTimeOffset now)
    {
        while (_terminalOrder.TryPeek(out var oldest)
               && (!_terminals.TryGetValue(oldest, out var expiresAt)
                   || expiresAt <= now))
        {
            _terminalOrder.Dequeue();
            _terminals.Remove(oldest);
        }
    }
}
