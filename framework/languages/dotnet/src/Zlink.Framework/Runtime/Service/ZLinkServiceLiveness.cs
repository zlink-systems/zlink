namespace Zlink.Framework.Runtime.Service;

internal sealed class ZLinkServiceLiveness
{
    internal static readonly TimeSpan ProbeInterval = TimeSpan.FromSeconds(5);
    internal static readonly TimeSpan PeerTimeout = TimeSpan.FromSeconds(15);

    private ulong _nextProbeId;
    private ulong _outstandingProbeId;
    private long _nextProbeTimestamp;
    private long _deadlineTimestamp;
    private readonly TimeProvider _time;

    internal ZLinkServiceLiveness(long admittedTimestamp, TimeProvider? time = null)
    {
        _time = time ?? TimeProvider.System;
        _nextProbeTimestamp = admittedTimestamp;
        _deadlineTimestamp = Add(admittedTimestamp, PeerTimeout);
    }

    internal ulong OutstandingProbeId => _outstandingProbeId;
    internal long DeadlineTimestamp => Volatile.Read(ref _deadlineTimestamp);

    internal TimeSpan TimeUntilNextActivity(long timestamp)
    {
        var due = Math.Min(_nextProbeTimestamp, Volatile.Read(ref _deadlineTimestamp));
        return due <= timestamp
            ? TimeSpan.Zero
            : TimeSpan.FromMilliseconds(
                Math.Ceiling((due - timestamp) * 1000d / _time.TimestampFrequency)
            );
    }

    internal bool TryGetProbe(long timestamp, out ulong probeId)
    {
        if (timestamp < _nextProbeTimestamp)
        {
            probeId = 0;
            return false;
        }

        _nextProbeTimestamp = Add(timestamp, ProbeInterval);
        if (_outstandingProbeId == 0)
        {
            _nextProbeId++;
            if (_nextProbeId == 0)
                throw new InvalidOperationException("The liveness probe id space was exhausted.");
            _outstandingProbeId = _nextProbeId;
        }

        probeId = _outstandingProbeId;
        return true;
    }

    internal bool Acknowledge(ulong probeId, long timestamp)
    {
        RecordReceived(timestamp);
        if (probeId == 0 || probeId != _outstandingProbeId)
            return false;

        _outstandingProbeId = 0;
        return true;
    }

    internal void RecordReceived(long timestamp)
    {
        var deadline = Add(timestamp, PeerTimeout);
        var previous = Volatile.Read(ref _deadlineTimestamp);
        while (deadline > previous)
        {
            var observed = Interlocked.CompareExchange(ref _deadlineTimestamp, deadline, previous);
            if (observed == previous)
                return;
            previous = observed;
        }
    }

    internal void RecordReceived() => RecordReceived(_time.GetTimestamp());

    internal bool IsExpired(long timestamp) => timestamp >= Volatile.Read(ref _deadlineTimestamp);

    private long Add(long timestamp, TimeSpan duration)
    {
        var delta = (long)Math.Ceiling(duration.TotalSeconds * _time.TimestampFrequency);
        return checked(timestamp + delta);
    }
}
