using System.Runtime.CompilerServices;

using Zlink.Framework.Contracts.Configuration;

namespace Zlink.Framework.Runtime.Diagnostics;

/// <summary>
/// Keeps one latest intermediate status and a bounded queue of terminal
/// statuses for one observer. The queue never blocks the producer.
/// </summary>
internal sealed class ZLinkObservationQueue<TStatus>
    where TStatus : class
{
    private const ulong LossMaximum = 9_223_372_036_854_775_807UL;

    private readonly object _gate = new();
    private readonly Queue<TStatus> _terminalStatuses = [];
    private readonly SemaphoreSlim _available = new(0, 1);
    private readonly int _terminalCapacity;
    private readonly Func<TStatus, ulong> _sequenceSelector;
    private readonly string _eventName;
    private TStatus? _latestStatus;
    private bool _hasLatestStatus;
    private bool _completed;
    private ulong _coalescedCount;
    private ulong _discardedTerminalCount;

    internal ZLinkObservationQueue(
        int terminalCapacity,
        Func<TStatus, ulong> sequenceSelector,
        string eventName = "unknown")
    {
        if (terminalCapacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(terminalCapacity));
        ArgumentNullException.ThrowIfNull(sequenceSelector);
        if (string.IsNullOrWhiteSpace(eventName))
            throw new ArgumentException("An event name is required.", nameof(eventName));
        _terminalCapacity = terminalCapacity;
        _sequenceSelector = sequenceSelector;
        _eventName = eventName;
    }

    internal void Publish(TStatus status, bool terminal)
    {
        ArgumentNullException.ThrowIfNull(status);
        lock (_gate)
        {
            if (_completed)
                return;

            if (terminal)
            {
                if (_hasLatestStatus)
                {
                    _latestStatus = null;
                    _hasLatestStatus = false;
                    _coalescedCount = Increment(_coalescedCount);
                }

                if (_terminalStatuses.Count == _terminalCapacity)
                {
                    _terminalStatuses.Dequeue();
                    _discardedTerminalCount =
                        Increment(_discardedTerminalCount);
                    ZLinkRuntimeMetrics.RecordObserverOverflow(_eventName);
                }
                _terminalStatuses.Enqueue(status);
            }
            else
            {
                if (_hasLatestStatus)
                    _coalescedCount = Increment(_coalescedCount);
                _latestStatus = status;
                _hasLatestStatus = true;
            }

            SignalIfNeededUnderLock();
        }
    }

    internal async IAsyncEnumerable<ZLinkObservedStatus<TStatus>> ReadAllAsync(
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        while (true)
        {
            await _available.WaitAsync(cancellationToken).ConfigureAwait(false);
            ZLinkObservedStatus<TStatus>? next;
            lock (_gate)
            {
                next = TakeNextUnderLock();
                if (next is null && !_completed)
                {
                    SignalIfNeededUnderLock();
                    continue;
                }
            }

            if (next is null)
                yield break;
            yield return next.Value;
        }
    }

    internal void Complete()
    {
        lock (_gate)
        {
            if (_completed)
                return;
            _completed = true;
            SignalIfNeededUnderLock();
        }
    }

    private ZLinkObservedStatus<TStatus>? TakeNextUnderLock()
    {
        TStatus? status = null;
        if (_terminalStatuses.Count != 0
            && (!_hasLatestStatus
                || _sequenceSelector(_terminalStatuses.Peek())
                    <= _sequenceSelector(_latestStatus!)))
        {
            status = _terminalStatuses.Dequeue();
        }
        else if (_hasLatestStatus)
        {
            status = _latestStatus;
            _latestStatus = null;
            _hasLatestStatus = false;
        }

        if (status is null)
        {
            SignalIfNeededUnderLock();
            return null;
        }

        var observed = new ZLinkObservedStatus<TStatus>(
            status,
            new ZLinkObservationLoss(
                _coalescedCount,
                _discardedTerminalCount));
        SignalIfNeededUnderLock();
        return observed;
    }

    private void SignalIfNeededUnderLock()
    {
        if ((_terminalStatuses.Count != 0 || _hasLatestStatus || _completed)
            && _available.CurrentCount == 0)
            _available.Release();
    }

    private static ulong Increment(ulong value) =>
        value == LossMaximum ? LossMaximum : value + 1;
}
