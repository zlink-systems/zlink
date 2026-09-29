using System.Diagnostics;

namespace Zlink.Framework.Runtime.Service;

internal sealed class ZLinkServiceLiveness
{
    internal static readonly TimeSpan ProbeInterval = TimeSpan.FromSeconds(5);
    internal static readonly TimeSpan PeerTimeout = TimeSpan.FromSeconds(15);

    private ulong _nextProbeId;
    private ulong _outstandingProbeId;
    private long _nextProbeTimestamp;
    private long _deadlineTimestamp;

    internal ZLinkServiceLiveness(long admittedTimestamp)
    {
        _nextProbeTimestamp = Add(admittedTimestamp, ProbeInterval);
        _deadlineTimestamp = Add(admittedTimestamp, PeerTimeout);
    }

    internal ulong OutstandingProbeId => _outstandingProbeId;
    internal long DeadlineTimestamp => _deadlineTimestamp;

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
        if (probeId == 0 || probeId != _outstandingProbeId)
            return false;

        _outstandingProbeId = 0;
        _deadlineTimestamp = Add(timestamp, PeerTimeout);
        return true;
    }

    internal bool IsExpired(long timestamp) => timestamp >= _deadlineTimestamp;

    private static long Add(long timestamp, TimeSpan duration)
    {
        var delta = (long)Math.Ceiling(duration.TotalSeconds * Stopwatch.Frequency);
        return checked(timestamp + delta);
    }
}
