namespace Zlink.Framework.Runtime.Spots;

/// <summary>
/// Owns the node-wide permit count for object factory activation. Population
/// reservations and active objects are tracked by the Location Store; this
/// admission only bounds the work between native materialization and factory
/// initialization completion.
/// </summary>
internal sealed class ZLinkActivationConcurrencyAdmission
{
    private readonly object _gate = new();
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
        get
        {
            lock (_gate) return _active;
        }
    }

    internal int Limit => _limit;

    internal void Acquire(string objectDescription)
    {
        int active;
        lock (_gate)
        {
            if (_active >= _limit)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.CapacityExceeded,
                    $"Object activation concurrency limit was reached for {objectDescription}.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            active = ++_active;
        }
        _activeChanged?.Invoke(active);
    }

    internal void Release()
    {
        int active;
        lock (_gate)
        {
            if (_active <= 0)
                throw new InvalidOperationException(
                    "Object activation admission count became negative.");
            active = --_active;
        }
        _activeChanged?.Invoke(active);
    }

}
