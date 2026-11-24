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
    private readonly ZLinkStreamNodeRegistration _options;

    private readonly TimeProvider _time;
    private long _lastApplicationInbound;
    private readonly long _createdAt;
    private TimeSpan _lastHeartbeatPhase;
    private long _lastInbound = long.MinValue;

    public ZLinkStreamSessionLiveness(
        TimeProvider? timeProvider = null,
        ZLinkStreamNodeRegistration? options = null
    )
    {
        _options = options ?? new ZLinkStreamNodeRegistration { StreamNodeName = string.Empty };
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

    public (ZLinkStreamLivenessDecision Decision, TimeSpan Delay) Evaluate(long? connectedAt = null)
    {
        var now = _time.GetTimestamp();
        var established = connectedAt ?? _createdAt;
        var inboundBaseline = Math.Min(now, Math.Max(Volatile.Read(ref _lastInbound), established));
        var heartbeatTimeout =
            _options.HeartbeatTimeout - _time.GetElapsedTime(inboundBaseline, now);
        if (heartbeatTimeout <= TimeSpan.Zero)
            return (ZLinkStreamLivenessDecision.HeartbeatTimeout, Timeout.InfiniteTimeSpan);

        var idleTimeout = TimeSpan.MaxValue;
        if (_options.IdleTimeout > TimeSpan.Zero)
        {
            var applicationBaseline = Math.Min(now, Volatile.Read(ref _lastApplicationInbound));
            idleTimeout = _options.IdleTimeout - _time.GetElapsedTime(applicationBaseline, now);
            if (idleTimeout <= TimeSpan.Zero)
                return (ZLinkStreamLivenessDecision.IdleTimeout, Timeout.InfiniteTimeSpan);
        }

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
    }
}
