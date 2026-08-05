using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal enum ZLinkMeshPeerControlRetryResult
{
    Accepted,
    Backpressured,
    Stale,
    PermanentFailure
}

// Retains a complete infrastructure record after Core reports completion-lane
// back-pressure. The next attempt always submits the record from its first part.
// Admission and liveness records are coalesced by target, connection epoch, and
// command because a newer descriptor or the same outstanding probe supersedes
// the older retry.
internal sealed class ZLinkMeshPeerControlRetryQueue
{
    internal const int DefaultMaximumRecords = 4_096;
    internal const long DefaultMaximumBytes = 4 * 1024 * 1024;

    private readonly object _gate = new();
    private readonly int _maximumRecords;
    private readonly long _maximumBytes;
    private readonly Dictionary<RetryKey, PendingRecord> _pending = [];
    private readonly Queue<RetryKey> _ready = [];
    private long _pendingBytes;
    private long _version;
    private long _capacityRejectionCount;

    internal ZLinkMeshPeerControlRetryQueue(
        int maximumRecords = DefaultMaximumRecords,
        long maximumBytes = DefaultMaximumBytes)
    {
        if (maximumRecords <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumRecords));
        if (maximumBytes <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumBytes));
        _maximumRecords = maximumRecords;
        _maximumBytes = maximumBytes;
    }

    internal int Count
    {
        get
        {
            lock (_gate)
                return _pending.Count;
        }
    }

    internal long Bytes
    {
        get
        {
            lock (_gate)
                return _pendingBytes;
        }
    }

    internal long CapacityRejectionCount
    {
        get
        {
            lock (_gate)
                return _capacityRejectionCount;
        }
    }

    internal long NextIntentVersion()
    {
        lock (_gate)
            return checked(++_version);
    }

    internal bool TryRemember(
        RoutingId target,
        ulong connectionGeneration,
        ServiceWireConstants.Command command,
        byte[] payload,
        long intentVersion = 0)
    {
        ArgumentNullException.ThrowIfNull(payload);
        if (target.IsEmpty || payload.Length == 0)
            return false;

        lock (_gate)
        {
            var key = new RetryKey(target, connectionGeneration, command);
            var version = intentVersion == 0
                ? checked(++_version)
                : intentVersion > _version
                    ? (_version = intentVersion)
                    : intentVersion;
            var snapshot = new PendingRecord(
                payload.ToArray(),
                version,
                Queued: true);
            if (_pending.TryGetValue(key, out var current))
            {
                var nextBytes = checked(
                    _pendingBytes - current.Payload.LongLength
                    + snapshot.Payload.LongLength);
                if (nextBytes > _maximumBytes)
                {
                    _capacityRejectionCount = checked(_capacityRejectionCount + 1);
                    return false;
                }

                _pending[key] = snapshot;
                _pendingBytes = nextBytes;
                if (!current.Queued)
                    _ready.Enqueue(key);
                return true;
            }

            if (_pending.Count >= _maximumRecords
                || _pendingBytes > _maximumBytes - snapshot.Payload.LongLength)
            {
                _capacityRejectionCount = checked(_capacityRejectionCount + 1);
                return false;
            }

            _pending[key] = snapshot;
            _pendingBytes = checked(_pendingBytes + snapshot.Payload.LongLength);
            _ready.Enqueue(key);
            return true;
        }
    }

    internal int Flush(
        Func<RoutingId, ulong, byte[], ZLinkMeshPeerControlRetryResult> trySend)
    {
        ArgumentNullException.ThrowIfNull(trySend);

        var attempted = 0;
        int initialCount;
        lock (_gate)
            initialCount = _pending.Count;

        while (attempted < initialCount)
        {
            RetryKey key;
            PendingRecord snapshot;
            lock (_gate)
            {
                if (!TryTakeNextUnderLock(out key, out snapshot))
                    break;
            }

            attempted++;
            var result = trySend(
                key.Target,
                key.ConnectionGeneration,
                snapshot.Payload);

            lock (_gate)
            {
                if (!_pending.TryGetValue(key, out var current))
                    continue;

                // A concurrent producer replaces the dictionary value instead
                // of mutating the in-flight object. Never remove that newer
                // record merely because this older attempt succeeded.
                if (current.Version != snapshot.Version)
                    continue;

                switch (result)
                {
                    case ZLinkMeshPeerControlRetryResult.Accepted:
                    case ZLinkMeshPeerControlRetryResult.Stale:
                    case ZLinkMeshPeerControlRetryResult.PermanentFailure:
                        _pending.Remove(key);
                        _pendingBytes -= snapshot.Payload.LongLength;
                        break;
                    case ZLinkMeshPeerControlRetryResult.Backpressured:
                        _pending[key] = current with { Queued = true };
                        _ready.Enqueue(key);
                        break;
                    default:
                        throw new ArgumentOutOfRangeException(nameof(result));
                }
            }
        }

        return attempted;
    }

    internal void RemoveTarget(RoutingId target)
    {
        if (target.IsEmpty)
            return;

        lock (_gate)
        {
            var keys = _pending.Keys
                .Where(key => key.Target == target)
                .ToArray();
            foreach (var key in keys)
            {
                if (_pending.Remove(key, out var pending))
                    _pendingBytes -= pending.Payload.LongLength;
            }
            CompactReadyQueueUnderLock();
        }
    }

    internal void Remove(
        RoutingId target,
        ServiceWireConstants.Command command)
    {
        lock (_gate)
        {
            var keys = _pending.Keys
                .Where(key => key.Target == target && key.Command == command)
                .ToArray();
            foreach (var key in keys)
            {
                if (_pending.Remove(key, out var pending))
                    _pendingBytes -= pending.Payload.LongLength;
            }
            CompactReadyQueueUnderLock();
        }
    }

    internal void RemoveUpTo(
        RoutingId target,
        ulong connectionGeneration,
        ServiceWireConstants.Command command,
        long intentVersion)
    {
        if (target.IsEmpty || intentVersion <= 0)
            return;

        lock (_gate)
        {
            var key = new RetryKey(target, connectionGeneration, command);
            if (_pending.TryGetValue(key, out var pending)
                && pending.Version <= intentVersion)
            {
                _pending.Remove(key);
                _pendingBytes -= pending.Payload.LongLength;
                CompactReadyQueueUnderLock();
            }
        }
    }

    internal void Clear()
    {
        lock (_gate)
        {
            _pending.Clear();
            _ready.Clear();
            _pendingBytes = 0;
        }
    }

    private bool TryTakeNextUnderLock(
        out RetryKey key,
        out PendingRecord pending)
    {
        while (_ready.TryDequeue(out key))
        {
            if (!_pending.TryGetValue(key, out var candidate)
                || !candidate.Queued)
                continue;

            pending = candidate with { Queued = false };
            _pending[key] = pending;
            return true;
        }

        key = default;
        pending = null!;
        return false;
    }

    private void CompactReadyQueueUnderLock()
    {
        if (_ready.Count == 0)
            return;

        var retained = new HashSet<RetryKey>();
        var keys = _ready.ToArray();
        _ready.Clear();
        foreach (var key in keys)
        {
            if (_pending.TryGetValue(key, out var pending)
                && pending.Queued
                && retained.Add(key))
                _ready.Enqueue(key);
        }
    }

    private readonly record struct RetryKey(
        RoutingId Target,
        ulong ConnectionGeneration,
        ServiceWireConstants.Command Command);

    private sealed record PendingRecord(
        byte[] Payload,
        long Version,
        bool Queued);
}
