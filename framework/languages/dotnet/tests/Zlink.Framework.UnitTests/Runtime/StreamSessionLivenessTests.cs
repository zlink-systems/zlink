namespace Zlink.Framework.UnitTests;

public sealed class StreamSessionLivenessTests
{
    [Fact]
    public async Task Older_inbound_timestamp_cannot_replace_newer_inbound()
    {
        using var time = new PausingTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);
        time.Advance(ZLinkStreamSessionLiveness.HeartbeatInterval);
        time.PauseNextTimestamp();
        var olderUpdate = Task.Run(liveness.RecordInbound);
        try
        {
            await time.TimestampCaptured.Task.WaitAsync(TimeSpan.FromSeconds(2));
            time.Advance(
                ZLinkStreamSessionLiveness.HeartbeatTimeout
                    - ZLinkStreamSessionLiveness.HeartbeatInterval
                    - TimeSpan.FromMilliseconds(1)
            );
            liveness.RecordInbound();
        }
        finally
        {
            time.ReleaseTimestamp();
        }
        await olderUpdate.WaitAsync(TimeSpan.FromSeconds(2));
        time.Advance(ZLinkStreamSessionLiveness.HeartbeatInterval + TimeSpan.FromMilliseconds(1));
        Assert.NotEqual(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    private sealed class PausingTimeProvider : TimeProvider, IDisposable
    {
        private readonly ManualTimeProvider _time = new();
        private readonly ManualResetEventSlim _release = new(false);
        private int _pauseNext;
        public TaskCompletionSource TimestampCaptured { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public override long TimestampFrequency => _time.TimestampFrequency;

        public void Advance(TimeSpan duration) => _time.Advance(duration);

        public void PauseNextTimestamp() => Interlocked.Exchange(ref _pauseNext, 1);

        public void ReleaseTimestamp() => _release.Set();

        public override long GetTimestamp()
        {
            var timestamp = _time.GetTimestamp();
            if (Interlocked.Exchange(ref _pauseNext, 0) != 0)
            {
                TimestampCaptured.TrySetResult();
                if (!_release.Wait(TimeSpan.FromSeconds(2)))
                    throw new TimeoutException("Timestamp release was not signaled.");
            }
            return timestamp;
        }

        public void Dispose() => _release.Dispose();
    }

    [Fact]
    public void Unanswered_ping_does_not_suppress_the_next_periodic_ping()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);
        for (var cycle = 0; cycle < 3; cycle++)
        {
            time.Advance(ZLinkStreamSessionLiveness.HeartbeatInterval);
            Assert.Equal(ZLinkStreamLivenessDecision.SendHeartbeat, liveness.Evaluate());
            liveness.RecordHeartbeatPing();
        }
    }

    [Fact]
    public void Pong_before_each_inbound_deadline_keeps_the_connection_alive()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);
        for (var cycle = 0; cycle < 3; cycle++)
        {
            time.Advance(
                ZLinkStreamSessionLiveness.HeartbeatTimeout - TimeSpan.FromMilliseconds(1)
            );
            liveness.RecordInbound();
            Assert.NotEqual(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
        }
        time.Advance(ZLinkStreamSessionLiveness.HeartbeatTimeout);
        Assert.Equal(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Connection_without_inbound_times_out_at_the_connection_deadline()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);
        time.Advance(ZLinkStreamSessionLiveness.HeartbeatInterval);
        liveness.RecordHeartbeatPing();
        time.Advance(
            ZLinkStreamSessionLiveness.HeartbeatTimeout
                - ZLinkStreamSessionLiveness.HeartbeatInterval
        );
        Assert.Equal(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Data_before_each_inbound_deadline_keeps_the_connection_alive()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);
        liveness.RecordHeartbeatPing();
        for (var cycle = 0; cycle < 3; cycle++)
        {
            time.Advance(
                ZLinkStreamSessionLiveness.HeartbeatTimeout - TimeSpan.FromMilliseconds(1)
            );
            liveness.RecordInbound();
            liveness.RecordApplicationInbound();
            Assert.NotEqual(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
        }
    }

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
    public void Heartbeat_pong_resets_the_inbound_deadline()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);

        time.Advance(ZLinkStreamSessionLiveness.HeartbeatInterval);
        liveness.RecordHeartbeatPing();
        time.Advance(ZLinkStreamSessionLiveness.HeartbeatTimeout - TimeSpan.FromMilliseconds(1));
        liveness.RecordInbound();

        Assert.NotEqual(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Control_traffic_does_not_reset_application_idle_timeout()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);

        time.Advance(ZLinkStreamSessionLiveness.IdleTimeout);
        liveness.RecordInbound();

        Assert.Equal(ZLinkStreamLivenessDecision.IdleTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Application_traffic_resets_idle_timeout()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);

        time.Advance(ZLinkStreamSessionLiveness.IdleTimeout - TimeSpan.FromSeconds(1));
        liveness.RecordInbound();
        liveness.RecordApplicationInbound();
        time.Advance(TimeSpan.FromSeconds(1));

        Assert.NotEqual(ZLinkStreamLivenessDecision.IdleTimeout, liveness.Evaluate());
    }
}
