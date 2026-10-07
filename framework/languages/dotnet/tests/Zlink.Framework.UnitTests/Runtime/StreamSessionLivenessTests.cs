namespace Zlink.Framework.UnitTests;

public sealed class StreamSessionLivenessTests
{
    [Fact]
    public async Task Older_inbound_timestamp_cannot_replace_newer_inbound()
    {
        using var time = new PausingTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);
        time.Advance(TimeSpan.FromSeconds(1));
        time.PauseNextTimestamp();
        var olderUpdate = Task.Run(liveness.RecordInbound);
        try
        {
            await time.TimestampCaptured.Task.WaitAsync(TimeSpan.FromSeconds(2));
            time.Advance(
                TimeSpan.FromSeconds(5) - TimeSpan.FromSeconds(1) - TimeSpan.FromMilliseconds(1)
            );
            liveness.RecordInbound();
        }
        finally
        {
            time.ReleaseTimestamp();
        }
        await olderUpdate.WaitAsync(TimeSpan.FromSeconds(2));
        time.Advance(TimeSpan.FromSeconds(1) + TimeSpan.FromMilliseconds(1));
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
            time.Advance(TimeSpan.FromSeconds(1));
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
            time.Advance(TimeSpan.FromSeconds(5) - TimeSpan.FromMilliseconds(1));
            liveness.RecordInbound();
            Assert.NotEqual(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
        }
        time.Advance(TimeSpan.FromSeconds(5));
        Assert.Equal(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Connection_without_inbound_times_out_at_the_connection_deadline()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);
        time.Advance(TimeSpan.FromSeconds(1));
        liveness.RecordHeartbeatPing();
        time.Advance(TimeSpan.FromSeconds(5) - TimeSpan.FromSeconds(1));
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
            time.Advance(TimeSpan.FromSeconds(5) - TimeSpan.FromMilliseconds(1));
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

        time.Advance(TimeSpan.FromSeconds(1));
        Assert.Equal(ZLinkStreamLivenessDecision.SendHeartbeat, liveness.Evaluate());
        liveness.RecordHeartbeatPing();

        time.Advance(TimeSpan.FromSeconds(5));

        Assert.Equal(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Heartbeat_pong_resets_the_inbound_deadline()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);

        time.Advance(TimeSpan.FromSeconds(1));
        liveness.RecordHeartbeatPing();
        time.Advance(TimeSpan.FromSeconds(5) - TimeSpan.FromMilliseconds(1));
        liveness.RecordInbound();

        Assert.NotEqual(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Configured_heartbeat_interval_and_timeout_control_decisions()
    {
        var time = new ManualTimeProvider();
        var options = new ZLinkStreamNodeRegistration { StreamNodeName = "test" };
        new ZLinkStreamNodeBuilder(options).SetHeartbeat(
            TimeSpan.FromSeconds(2),
            TimeSpan.FromSeconds(6)
        );
        var liveness = new ZLinkStreamSessionLiveness(time, options);
        time.Advance(TimeSpan.FromSeconds(1));
        Assert.Equal(ZLinkStreamLivenessDecision.None, liveness.Evaluate());
        time.Advance(TimeSpan.FromSeconds(1));
        Assert.Equal(ZLinkStreamLivenessDecision.SendHeartbeat, liveness.Evaluate());
        liveness.RecordHeartbeatPing();
        time.Advance(TimeSpan.FromMilliseconds(3999));
        Assert.NotEqual(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
        time.Advance(TimeSpan.FromMilliseconds(1));
        Assert.Equal(ZLinkStreamLivenessDecision.HeartbeatTimeout, liveness.Evaluate());
    }

    [Theory]
    [InlineData(0, 5000, "STREAM heartbeat interval must be positive.")]
    [InlineData(-1, 5000, "STREAM heartbeat interval must be positive.")]
    [InlineData(1000, 0, "STREAM heartbeat timeout must be positive.")]
    [InlineData(1000, -1, "STREAM heartbeat timeout must be positive.")]
    [InlineData(1000, 1000, "STREAM heartbeat timeout must be greater than interval.")]
    [InlineData(1000, 999, "STREAM heartbeat timeout must be greater than interval.")]
    public void Invalid_heartbeat_settings_are_configuration_errors_before_start(
        int interval,
        int timeout,
        string message
    )
    {
        var builder = new ZLinkStreamNodeBuilder(
            new ZLinkStreamNodeRegistration { StreamNodeName = "test" }
        );
        var error = Assert.Throws<ZLinkConfigurationException>(() =>
            builder.SetHeartbeat(
                TimeSpan.FromMilliseconds(interval),
                TimeSpan.FromMilliseconds(timeout)
            )
        );
        Assert.Equal(message, error.Message);
    }

    [Fact]
    public void Idle_setting_rejects_negative_and_accepts_zero_before_start()
    {
        var options = new ZLinkStreamNodeRegistration { StreamNodeName = "test" };
        var builder = new ZLinkStreamNodeBuilder(options);
        Assert.Equal(
            "STREAM idle timeout must not be negative.",
            Assert
                .Throws<ZLinkConfigurationException>(() =>
                    builder.SetIdleTimeout(TimeSpan.FromMilliseconds(-1))
                )
                .Message
        );
        Assert.Same(builder, builder.SetIdleTimeout(TimeSpan.Zero));
        Assert.Equal(TimeSpan.Zero, options.IdleTimeout);
    }

    [Fact]
    public void Large_positive_durations_do_not_expire_early()
    {
        var time = new ManualTimeProvider();
        var options = new ZLinkStreamNodeRegistration { StreamNodeName = "test" };
        new ZLinkStreamNodeBuilder(options)
            .SetHeartbeat(TimeSpan.MaxValue - TimeSpan.FromTicks(1), TimeSpan.MaxValue)
            .SetIdleTimeout(TimeSpan.MaxValue);
        var liveness = new ZLinkStreamSessionLiveness(time, options);
        time.Advance(TimeSpan.FromSeconds(35));
        Assert.Equal(ZLinkStreamLivenessDecision.None, liveness.Evaluate());
    }

    [Fact]
    public void Default_idle_timeout_is_disabled_with_heartbeat_traffic()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(time);
        for (var second = 1; second <= 35; second++)
        {
            time.Advance(TimeSpan.FromSeconds(1));
            liveness.RecordInbound();
            Assert.Equal(ZLinkStreamLivenessDecision.SendHeartbeat, liveness.Evaluate());
            liveness.RecordHeartbeatPing();
        }
    }

    [Fact]
    public void Control_traffic_does_not_reset_application_idle_timeout()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(
            time,
            new ZLinkStreamNodeRegistration
            {
                StreamNodeName = "test",
                IdleTimeout = TimeSpan.FromSeconds(30),
            }
        );

        time.Advance(TimeSpan.FromSeconds(30));
        liveness.RecordInbound();

        Assert.Equal(ZLinkStreamLivenessDecision.IdleTimeout, liveness.Evaluate());
    }

    [Fact]
    public void Application_traffic_resets_idle_timeout()
    {
        var time = new ManualTimeProvider();
        var liveness = new ZLinkStreamSessionLiveness(
            time,
            new ZLinkStreamNodeRegistration
            {
                StreamNodeName = "test",
                IdleTimeout = TimeSpan.FromSeconds(30),
            }
        );

        time.Advance(TimeSpan.FromSeconds(30) - TimeSpan.FromSeconds(1));
        liveness.RecordInbound();
        liveness.RecordApplicationInbound();
        time.Advance(TimeSpan.FromSeconds(1));

        Assert.NotEqual(ZLinkStreamLivenessDecision.IdleTimeout, liveness.Evaluate());
    }
}
