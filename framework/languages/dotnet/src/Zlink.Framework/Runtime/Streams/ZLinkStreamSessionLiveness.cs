namespace Zlink.Framework.Runtime.Streams;

internal enum ZLinkStreamLivenessDecision
{
    None,
    SendHeartbeat,
    IdleTimeout,
    HeartbeatTimeout,
}

internal sealed class ZLinkStreamSessionLiveness(TimeProvider? timeProvider = null)
{
    public static readonly TimeSpan SweepInterval = TimeSpan.FromSeconds(1);
    public static readonly TimeSpan HeartbeatInterval = TimeSpan.FromSeconds(1);
    public static readonly TimeSpan HeartbeatTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan IdleTimeout = TimeSpan.FromSeconds(30);

    private readonly TimeProvider _time = timeProvider ?? TimeProvider.System;
    private long _lastApplicationInbound = (timeProvider ?? TimeProvider.System).GetTimestamp();
    private long _lastHeartbeatPing = (timeProvider ?? TimeProvider.System).GetTimestamp();
    private int _heartbeatOutstanding;

    public void RecordApplicationInbound()
    {
        Interlocked.Exchange(ref _lastApplicationInbound, _time.GetTimestamp());
    }

    public void RecordHeartbeatPong()
    {
        Volatile.Write(ref _heartbeatOutstanding, 0);
    }

    public void RecordHeartbeatPing()
    {
        Interlocked.Exchange(ref _lastHeartbeatPing, _time.GetTimestamp());
        Volatile.Write(ref _heartbeatOutstanding, 1);
    }

    public ZLinkStreamLivenessDecision Evaluate()
    {
        var now = _time.GetTimestamp();
        if (
            Volatile.Read(ref _heartbeatOutstanding) != 0
            && _time.GetElapsedTime(Volatile.Read(ref _lastHeartbeatPing), now) >= HeartbeatTimeout
        )
            return ZLinkStreamLivenessDecision.HeartbeatTimeout;

        if (_time.GetElapsedTime(Volatile.Read(ref _lastApplicationInbound), now) >= IdleTimeout)
            return ZLinkStreamLivenessDecision.IdleTimeout;

        if (
            Volatile.Read(ref _heartbeatOutstanding) == 0
            && _time.GetElapsedTime(Volatile.Read(ref _lastHeartbeatPing), now) >= HeartbeatInterval
        )
            return ZLinkStreamLivenessDecision.SendHeartbeat;

        return ZLinkStreamLivenessDecision.None;
    }
}
