namespace Zlink.Framework.Runtime.Streams;

internal enum ZLinkStreamLivenessDecision
{
    None,
    SendHeartbeat,
    IdleTimeout,
    HeartbeatTimeout
}

internal sealed class ZLinkStreamSessionLiveness(TimeProvider? timeProvider = null)
{
    public static readonly TimeSpan SweepInterval = TimeSpan.FromSeconds(1);
    public static readonly TimeSpan HeartbeatInterval = TimeSpan.FromSeconds(1);
    public static readonly TimeSpan HeartbeatTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan IdleTimeout = TimeSpan.FromSeconds(30);

    private readonly TimeProvider _time = timeProvider ?? TimeProvider.System;
    private readonly object _gate = new();
    private long _lastApplicationInbound = (timeProvider ?? TimeProvider.System).GetTimestamp();
    private long _lastHeartbeatPing = (timeProvider ?? TimeProvider.System).GetTimestamp();
    private bool _heartbeatOutstanding;

    public void RecordApplicationInbound()
    {
        lock (_gate) _lastApplicationInbound = _time.GetTimestamp();
    }

    public void RecordHeartbeatPong()
    {
        lock (_gate) _heartbeatOutstanding = false;
    }

    public void RecordHeartbeatPing()
    {
        lock (_gate)
        {
            _lastHeartbeatPing = _time.GetTimestamp();
            _heartbeatOutstanding = true;
        }
    }

    public ZLinkStreamLivenessDecision Evaluate()
    {
        lock (_gate)
        {
            var now = _time.GetTimestamp();
            if (_heartbeatOutstanding
                && _time.GetElapsedTime(_lastHeartbeatPing, now) >= HeartbeatTimeout)
                return ZLinkStreamLivenessDecision.HeartbeatTimeout;

            if (_time.GetElapsedTime(_lastApplicationInbound, now) >= IdleTimeout)
                return ZLinkStreamLivenessDecision.IdleTimeout;

            if (!_heartbeatOutstanding
                && _time.GetElapsedTime(_lastHeartbeatPing, now) >= HeartbeatInterval)
                return ZLinkStreamLivenessDecision.SendHeartbeat;

            return ZLinkStreamLivenessDecision.None;
        }
    }
}
