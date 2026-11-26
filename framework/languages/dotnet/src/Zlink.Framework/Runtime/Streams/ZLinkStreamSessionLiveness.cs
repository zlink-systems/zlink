using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Streams;

internal enum ZLinkStreamLivenessDecision
{
    None,
    SendHeartbeat,
    IdleTimeout,
    HeartbeatTimeout,
}

internal sealed class ZLinkStreamSessionLiveness
{
    internal sealed class Reader(TimeProvider time, Action wakePoller)
    {
        // PACKET ingress identifies the peer only after a permit and a complete read.
        private long _permitResumedAt = long.MinValue;
        private int _wakeRequested;

        internal bool WakeRequested => Volatile.Read(ref _wakeRequested) != 0;

        internal void Wake()
        {
            if (Interlocked.Exchange(ref _wakeRequested, 1) == 0)
                wakePoller();
        }

        internal bool TakeWake() => Interlocked.Exchange(ref _wakeRequested, 0) != 0;

        internal long PermitResumedAt => Volatile.Read(ref _permitResumedAt);

        internal async ValueTask<ZLinkApplicationJobQueueLease> WaitForPermitAsync(
            ValueTask<ZLinkApplicationJobQueueLease> permit
        )
        {
            var resumedAt = _permitResumedAt;
            Volatile.Write(ref _permitResumedAt, WaitingForPermit);
            try
            {
                var lease = await permit.ConfigureAwait(false);
                resumedAt = time.GetTimestamp();
                return lease;
            }
            finally
            {
                Volatile.Write(ref _permitResumedAt, resumedAt);
            }
        }
    }

    private const long WaitingForPermit = long.MaxValue;
    private readonly ZLinkStreamNodeRegistration _options;
    private readonly Reader? _reader;

    private readonly TimeProvider _time;
    private long _lastApplicationInbound;
    private readonly long _createdAt;
    private TimeSpan _lastHeartbeatPhase;
    private long _lastInbound = long.MinValue;

    public ZLinkStreamSessionLiveness(
        TimeProvider? timeProvider = null,
        ZLinkStreamNodeRegistration? options = null,
        Reader? reader = null
    )
    {
        _options = options ?? new ZLinkStreamNodeRegistration { StreamNodeName = string.Empty };
        _reader = reader;
        _time = timeProvider ?? TimeProvider.System;
        _createdAt = _time.GetTimestamp();
        _lastApplicationInbound = _createdAt;
    }

    public void RecordApplicationInbound()
    {
        Interlocked.Exchange(ref _lastApplicationInbound, _time.GetTimestamp());
    }

    public void RecordInbound()
    {
        var timestamp = _time.GetTimestamp();
        var lastInbound = Volatile.Read(ref _lastInbound);
        while (timestamp > lastInbound)
        {
            var observed = Interlocked.CompareExchange(ref _lastInbound, timestamp, lastInbound);
            if (observed == lastInbound)
                return;
            lastInbound = observed;
        }
    }

    public (ZLinkStreamLivenessDecision Decision, TimeSpan Delay) Evaluate(
        long? connectedAt = null,
        long? timestamp = null,
        bool confirmExpiry = true
    )
    {
        var now = timestamp ?? _time.GetTimestamp();
        var established = connectedAt ?? _createdAt;
        var resumedAt = _reader?.PermitResumedAt ?? long.MinValue;
        var heartbeatTimeout = InboundRemaining(
            Math.Max(Volatile.Read(ref _lastInbound), established),
            _options.HeartbeatTimeout
        );
        if (heartbeatTimeout <= TimeSpan.Zero)
            return (ZLinkStreamLivenessDecision.HeartbeatTimeout, Timeout.InfiniteTimeSpan);

        var idleTimeout = InboundRemaining(
            Volatile.Read(ref _lastApplicationInbound),
            _options.IdleTimeout
        );
        if (idleTimeout <= TimeSpan.Zero)
            return (ZLinkStreamLivenessDecision.IdleTimeout, Timeout.InfiniteTimeSpan);

        var age = _time.GetElapsedTime(established, now);
        var interval = _options.HeartbeatInterval;
        var sinceLastPhase = age - _lastHeartbeatPhase;
        var decision = ZLinkStreamLivenessDecision.None;
        if (sinceLastPhase >= interval)
        {
            sinceLastPhase = TimeSpan.FromTicks(sinceLastPhase.Ticks % interval.Ticks);
            _lastHeartbeatPhase = age - sinceLastPhase;
            decision = ZLinkStreamLivenessDecision.SendHeartbeat;
        }
        return (
            decision,
            TimeSpan.FromTicks(
                Math.Min(
                    (interval - sinceLastPhase).Ticks,
                    Math.Min(heartbeatTimeout.Ticks, idleTimeout.Ticks)
                )
            )
        );

        TimeSpan InboundRemaining(long lastActivity, TimeSpan timeout)
        {
            if (!confirmExpiry || timeout <= TimeSpan.Zero || resumedAt == WaitingForPermit)
                return TimeSpan.MaxValue;

            var baseline = Math.Min(now, Math.Max(lastActivity, resumedAt));
            return timeout - _time.GetElapsedTime(baseline, now);
        }
    }
}
