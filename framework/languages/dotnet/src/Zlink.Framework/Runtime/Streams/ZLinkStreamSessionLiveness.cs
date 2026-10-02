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
    public static readonly TimeSpan SweepInterval = TimeSpan.FromSeconds(1);
    public static readonly TimeSpan HeartbeatInterval = TimeSpan.FromSeconds(1);
    public static readonly TimeSpan HeartbeatTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan IdleTimeout = TimeSpan.FromSeconds(30);

    private readonly TimeProvider _time;
    private long _lastApplicationInbound;
    private long _lastHeartbeatPing;
    private long _lastInbound;

    public ZLinkStreamSessionLiveness(TimeProvider? timeProvider = null)
    {
        _time = timeProvider ?? TimeProvider.System;
        var connectedAt = _time.GetTimestamp();
        _lastApplicationInbound = connectedAt;
        _lastHeartbeatPing = connectedAt;
        _lastInbound = connectedAt;
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

    public void RecordHeartbeatPing()
    {
        Interlocked.Exchange(ref _lastHeartbeatPing, _time.GetTimestamp());
    }

    public ZLinkStreamLivenessDecision Evaluate(long? connectedAt = null)
    {
        var now = _time.GetTimestamp();
        var inboundBaseline = Volatile.Read(ref _lastInbound);
        var pingBaseline = Volatile.Read(ref _lastHeartbeatPing);
        if (connectedAt is { } connectedTimestamp)
        {
            inboundBaseline = Math.Max(inboundBaseline, connectedTimestamp);
            pingBaseline = Math.Max(pingBaseline, connectedTimestamp);
        }
        if (_time.GetElapsedTime(inboundBaseline, now) >= HeartbeatTimeout)
            return ZLinkStreamLivenessDecision.HeartbeatTimeout;

        if (_time.GetElapsedTime(Volatile.Read(ref _lastApplicationInbound), now) >= IdleTimeout)
            return ZLinkStreamLivenessDecision.IdleTimeout;

        if (_time.GetElapsedTime(pingBaseline, now) >= HeartbeatInterval)
            return ZLinkStreamLivenessDecision.SendHeartbeat;

        return ZLinkStreamLivenessDecision.None;
    }
}
