namespace Zlink.Framework.UnitTests;

public sealed class StreamSessionLivenessTests
{
    [Fact]
    public void Heartbeat_timeout_wins_after_an_unanswered_server_ping()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);

        time.Advance(ZLinkStreamSessionLiveness.HeartbeatInterval);
        Assert.Equal(ZLinkStreamLivenessDecision.SendHeartbeat, liveness.Evaluate());
        liveness.RecordHeartbeatPing();

        time.Advance(ZLinkStreamSessionLiveness.HeartbeatTimeout);

        Assert.Equal(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Heartbeat_pong_clears_the_outstanding_deadline()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);

        time.Advance(ZLinkStreamSessionLiveness.HeartbeatInterval);
        liveness.RecordHeartbeatPing();
        time.Advance(ZLinkStreamSessionLiveness.HeartbeatTimeout - TimeSpan.FromMilliseconds(1));
        liveness.RecordHeartbeatPong();

        Assert.NotEqual(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Control_traffic_does_not_reset_application_idle_timeout()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);

        time.Advance(ZLinkStreamSessionLiveness.IdleTimeout);
        liveness.RecordHeartbeatPong();

        Assert.Equal(ZLinkStreamLivenessDecision.IdleTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Application_traffic_resets_idle_timeout()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);

        time.Advance(ZLinkStreamSessionLiveness.IdleTimeout - TimeSpan.FromSeconds(1));
        liveness.RecordApplicationInbound();
        time.Advance(TimeSpan.FromSeconds(1));

        Assert.NotEqual(ZLinkStreamLivenessDecision.IdleTimeout, liveness.Evaluate());
    }
}
