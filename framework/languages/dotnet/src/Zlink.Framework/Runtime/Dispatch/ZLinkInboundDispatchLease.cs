namespace Zlink.Framework.Runtime.Dispatch;

// Owns one application payload reservation in the host-wide ingress budget.
// The reservation is queued until dispatch starts and becomes active only for
// the duration of the handler work.
internal sealed class ZLinkInboundDispatchLease : IDisposable
{
    private readonly ZLinkInboundDispatchBudget _budget;
    private readonly ulong _payloadBytes;
    private readonly bool _overageReservation;
    private int _started;
    private int _disposed;

    internal ZLinkInboundDispatchLease(
        ZLinkInboundDispatchBudget budget,
        ulong payloadBytes,
        bool overageReservation = false)
    {
        _budget = budget;
        _payloadBytes = payloadBytes;
        _overageReservation = overageReservation;
    }

    internal void StartDispatch()
    {
        if (Interlocked.Exchange(ref _started, 1) == 0)
            _budget.HandlerStarted(_payloadBytes);
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        _budget.Completed(
            _payloadBytes,
            Volatile.Read(ref _started) != 0,
            _overageReservation);
    }
}
