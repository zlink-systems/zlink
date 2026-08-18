namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    private readonly ZLinkRelocationShutdownTracking _shutdownTracking = new();

    /// <summary>
    /// Spec 30 §11 — true only while every relocation unit this source
    /// started has reached its Message Follow route removal point (S4) and
    /// its cutover retransmission window has ended. A source-local
    /// observation, never a completion signal from a target. True when this
    /// source has not started a relocation.
    /// </summary>
    internal bool SafeToShutdown => _shutdownTracking.SafeToShutdown;

    /// <summary>
    /// Fires whenever <see cref="SafeToShutdown"/> flips, so the host status
    /// surface (24 §"State") can publish the change instead of requiring a
    /// poll.
    /// </summary>
    internal event Action? SafeToShutdownChanged
    {
        add => _shutdownTracking.Changed += value;
        remove => _shutdownTracking.Changed -= value;
    }

    /// <summary>
    /// Registers one relocation unit's SafeToShutdown obligation at seal
    /// time — not at S1/cutover submit — so a shutdown query racing the
    /// seal-to-cutover window never observes SafeToShutdown=true while a
    /// unit is still actively transferring (spec 30 §11, 28 §7.1). Dispose
    /// releases the obligation exactly once; the returned token is safe to
    /// dispose more than once.
    /// </summary>
    internal IDisposable BeginPendingRelocationUnit() => _shutdownTracking.Begin();
}

/// <summary>
/// Tracks the count of relocation units this source has sealed but not yet
/// released — i.e. units still short of SafeToShutdown's two conditions
/// (S4 reached and the retransmission window ended). Thread-safe.
/// </summary>
internal sealed class ZLinkRelocationShutdownTracking
{
    private int _pendingUnits;

    internal event Action? Changed;

    internal bool SafeToShutdown => Volatile.Read(ref _pendingUnits) == 0;

    internal IDisposable Begin()
    {
        if (Interlocked.Increment(ref _pendingUnits) == 1)
            Changed?.Invoke();
        return new Token(this);
    }

    private void Release()
    {
        if (Interlocked.Decrement(ref _pendingUnits) == 0)
            Changed?.Invoke();
    }

    private sealed class Token(ZLinkRelocationShutdownTracking owner) : IDisposable
    {
        private int _released;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _released, 1) == 0)
                owner.Release();
        }
    }
}
