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
    internal const int DefaultTerminalCapacity = 64;
    private const ulong LossMaximum = 9_223_372_036_854_775_807UL;

    private readonly object _gate = new();
    private readonly Dictionary<string, RetainedStatus> _latestBySource =
        new(StringComparer.Ordinal);
    private readonly Queue<RetainedStatus> _terminalStatuses = [];
    private readonly Dictionary<string, int> _terminalCountBySource =
        new(StringComparer.Ordinal);
    private readonly SemaphoreSlim _available = new(0, 1);
    private readonly int _terminalCapacity;
    private readonly Func<TStatus, string> _sourceSelector;
    private readonly string _eventName;
    private bool _completed;
    private ulong _coalescedCount;
    private ulong _discardedTerminalCount;
    private ulong _nextPublishOrdinal = 1;

    internal ZLinkObservationQueue(
        Func<TStatus, string> sourceSelector,
        int terminalCapacity = DefaultTerminalCapacity,
        string eventName = "unknown")
    {
        if (terminalCapacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(terminalCapacity));
        ArgumentNullException.ThrowIfNull(sourceSelector);
        if (string.IsNullOrWhiteSpace(eventName))
            throw new ArgumentException("An event name is required.", nameof(eventName));
        _terminalCapacity = terminalCapacity;
        _sourceSelector = sourceSelector;
        _eventName = eventName;
    }

    internal void Publish(TStatus status, bool terminal)
    {
        ArgumentNullException.ThrowIfNull(status);
        var source = _sourceSelector(status);
        if (string.IsNullOrWhiteSpace(source))
            throw new InvalidOperationException(
                "An observation source key cannot be empty.");
        lock (_gate)
        {
            if (_completed)
                return;
            if (_nextPublishOrdinal == ulong.MaxValue)
                throw new InvalidOperationException(
                    "The observation publish ordinal is exhausted.");
            var retained = new RetainedStatus(
                source,
                status,
                _nextPublishOrdinal++);

            if (terminal)
            {
                if (_latestBySource.Remove(source))
                {
                    _coalescedCount = IncrementLossCounter(_coalescedCount);
                }

                if (_terminalStatuses.Count == _terminalCapacity)
                {
                    var discarded = _terminalStatuses.Dequeue();
                    ReleaseTerminalSourceUnderLock(discarded.Source);
                    _discardedTerminalCount =
                        IncrementLossCounter(_discardedTerminalCount);
                    ZLinkRuntimeMetrics.RecordObserverOverflow(_eventName);
                }
                _terminalStatuses.Enqueue(retained);
                _terminalCountBySource[source] =
                    _terminalCountBySource.GetValueOrDefault(source) + 1;
            }
            else
            {
                if (_terminalCountBySource.ContainsKey(source))
                {
                    _coalescedCount = IncrementLossCounter(_coalescedCount);
                    SignalIfNeededUnderLock();
                    return;
                }
                if (_latestBySource.ContainsKey(source))
                    _coalescedCount = IncrementLossCounter(_coalescedCount);
                _latestBySource[source] = retained;
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
        RetainedStatus? next = null;
        var intermediate = OldestIntermediateUnderLock();
        if (_terminalStatuses.Count != 0
            && (intermediate is null
                || _terminalStatuses.Peek().PublishOrdinal
                <= intermediate.PublishOrdinal))
        {
            next = _terminalStatuses.Dequeue();
            ReleaseTerminalSourceUnderLock(next.Source);
        }
        else if (intermediate is not null)
        {
            next = intermediate;
            _latestBySource.Remove(intermediate.Source);
        }

        if (next is null)
        {
            SignalIfNeededUnderLock();
            return null;
        }

        var observed = new ZLinkObservedStatus<TStatus>(
            next.Status,
            new ZLinkObservationLoss(
                _coalescedCount,
                _discardedTerminalCount));
        SignalIfNeededUnderLock();
        return observed;
    }

    private void SignalIfNeededUnderLock()
    {
        if ((_terminalStatuses.Count != 0
             || _latestBySource.Count != 0
             || _completed)
            && _available.CurrentCount == 0)
            _available.Release();
    }

    private RetainedStatus? OldestIntermediateUnderLock()
    {
        RetainedStatus? oldest = null;
        foreach (var candidate in _latestBySource.Values)
            if (oldest is null
                || candidate.PublishOrdinal < oldest.PublishOrdinal)
                oldest = candidate;
        return oldest;
    }

    private void ReleaseTerminalSourceUnderLock(string source)
    {
        var count = _terminalCountBySource[source];
        if (count == 1)
            _terminalCountBySource.Remove(source);
        else
            _terminalCountBySource[source] = count - 1;
    }

    internal static ulong IncrementLossCounter(ulong value) =>
        value >= LossMaximum ? LossMaximum : value + 1;

    private sealed record RetainedStatus(
        string Source,
        TStatus Status,
        ulong PublishOrdinal);
}
