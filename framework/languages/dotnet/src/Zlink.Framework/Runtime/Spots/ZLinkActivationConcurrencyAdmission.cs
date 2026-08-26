namespace Zlink.Framework.Runtime.Spots;

/// <summary>
/// Owns the node-wide permit count for object factory activation. Population
/// reservations and active objects are tracked by the Location Store; this
/// admission only bounds the work between native materialization and factory
/// initialization completion.
/// </summary>
internal sealed class ZLinkActivationConcurrencyAdmission
{
    private readonly int _limit;
    private readonly Action<int>? _activeChanged;
    private int _active;

    internal ZLinkActivationConcurrencyAdmission(
        int limit,
        Action<int>? activeChanged = null)
    {
        if (limit <= 0)
            throw new ArgumentOutOfRangeException(nameof(limit));
        _limit = limit;
        _activeChanged = activeChanged;
    }

    internal int Active
    {
        get => Volatile.Read(ref _active);
    }

    internal int Limit => _limit;

    internal void Acquire(string objectDescription)
    {
        while (true)
        {
            var active = Volatile.Read(ref _active);
            if (active >= _limit)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.CapacityExceeded,
                    $"Object activation concurrency limit was reached for {objectDescription}.",
                    ZLinkRetryAdvice.RetryAfterBackoff);

            var next = active + 1;
            if (Interlocked.CompareExchange(ref _active, next, active) != active)
                continue;

            _activeChanged?.Invoke(next);
            return;
        }
    }

    internal void Release()
    {
        while (true)
        {
            var active = Volatile.Read(ref _active);
            if (active <= 0)
                throw new InvalidOperationException(
                    "Object activation admission count became negative.");

            var next = active - 1;
            if (Interlocked.CompareExchange(ref _active, next, active) != active)
                continue;

            _activeChanged?.Invoke(next);
            return;
        }
    }

}
