namespace Zlink.Framework.Runtime.Spots;

/// <summary>
/// The MeshNode's single activation admission record (MeshNode §5.1 "Pending activation").
/// Actor creation, User Spot creation, Instance Spot cold activation and relocation-target
/// Restore each hold one <see cref="Lease"/> from the moment the target MeshNode receives the
/// operation until Ready, target commit, rejection, failure or cleanup. Entry Spot and Actor
/// Join never acquire one. Status, <c>IsAvailable</c> headroom and the limit all read this
/// record; population reservations are tracked separately by the Location Store.
/// </summary>
internal sealed class ZLinkActivationConcurrencyAdmission
{
    private readonly int _limit;
    private readonly Action<int>? _activeChanged;
    private int _active;

    internal ZLinkActivationConcurrencyAdmission(int limit, Action<int>? activeChanged = null)
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

    /// <summary>
    /// Takes one admission for <paramref name="objectDescription"/>, or rejects the operation
    /// with <see cref="ZLinkFrameworkErrorKind.Unavailable"/> when the limit is reached.
    /// </summary>
    internal Lease Acquire(string objectDescription)
    {
        while (true)
        {
            var active = Volatile.Read(ref _active);
            if (active >= _limit)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Object activation concurrency limit was reached for {objectDescription}.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );

            var next = active + 1;
            if (Interlocked.CompareExchange(ref _active, next, active) != active)
                continue;

            _activeChanged?.Invoke(next);
            return new Lease(this);
        }
    }

    private void Release()
    {
        while (true)
        {
            var active = Volatile.Read(ref _active);
            if (active <= 0)
                throw new InvalidOperationException(
                    "Object activation admission count became negative."
                );

            var next = active - 1;
            if (Interlocked.CompareExchange(ref _active, next, active) != active)
                continue;

            _activeChanged?.Invoke(next);
            return;
        }
    }

    /// <summary>
    /// One held admission. The operation that ends it (Ready, commit, rejection, failure or
    /// cleanup) releases it; a second release of the same lease is a no-op.
    /// </summary>
    internal sealed class Lease
    {
        private ZLinkActivationConcurrencyAdmission? _owner;

        internal Lease(ZLinkActivationConcurrencyAdmission owner) => _owner = owner;

        internal void Release() => Interlocked.Exchange(ref _owner, null)?.Release();
    }
}
