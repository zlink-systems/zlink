using System.Diagnostics;
using System.Reflection;
using Xunit;
using Xunit.Abstractions;
using Systems.Zlink.Stream.Connector.Contracts;

namespace Zlink.NetworkSync.Interop;

public sealed class NetworkProfileTests(ITestOutputHelper output)
{
    private const ulong WarmupUs = 5_000_000;
    private const ulong DurationUs = 25_000_000;
    private const ulong InputPeriodUs = 80_000;
    private const ulong RenderPeriodUs = 10_000;
    private const int InputSamples = 250;
    private const int RenderSamples = 2000;
    private const string LateMetric = "late inputs";

    private static readonly FieldInfo ClientClock = typeof(NetworkSyncClient<int, int, int>)
        .GetField("_clock", BindingFlags.Instance | BindingFlags.NonPublic)!;

    private static readonly PropertyInfo ClientInputLead = typeof(MatchClock)
        .GetProperty(nameof(MatchClock.LeadUs), BindingFlags.Instance | BindingFlags.NonPublic)!;

    public static IEnumerable<object[]> Profiles =>
        from rate in new[] { 20, 30, 60 }
        from profile in NetworkProfile.All
        select new object[] { rate, profile.Name };

    [Theory]
    [MemberData(nameof(Profiles))]
    public async Task MeetsTimingBudgets(int rate, string name)
    {
        var profile = NetworkProfile.All.Single(p => p.Name == name);
        var result = await MeasureAsync(rate, profile, fixedHalfRtt: false);
        Assert.Empty(result.Failures());
    }

    [Fact]
    public async Task LateGateRejectsFixedHalfRtt()
    {
        var result = await MeasureAsync(60, NetworkProfile.All[^1], fixedHalfRtt: true);
        Assert.Equal(new[] { LateMetric }, result.Failures());
    }

    [Fact]
    public async Task SlowRecipientCoalescesAndRecoversAcrossResync()
    {
        var match = new VirtualMatch(20, NetworkProfile.All[0]);
        await match.StartAsync();
        var fast = await match.JoinAsync("fast");
        var slow = await match.JoinAsync("slow");
        using var fastClient = Attach(fast, match);
        using var slowClient = Attach(slow, match);
        await fastClient.Update();
        await slowClient.Update();

        for (ulong tick = 1; tick <= 6; tick++)
            await match.TickAsync(match.TickDeadline(tick));
        await match.AdvanceAsync(325_000);
        Assert.True(slowClient.TryGetInterpolation(out var ready) && !ready.IsHolding);

        // The first submission stays in flight through six later ticks.
        slow.PauseUntil = 675_000;
        for (ulong tick = 7; tick <= 13; tick++)
        {
            await match.TickAsync(match.TickDeadline(tick));
            if (tick == 9)
                Assert.True(slowClient.TryGetInterpolation(out var holding) && holding.IsHolding);
        }

        Assert.Equal(7, slow.SnapshotsSubmitted);
        Assert.Equal(13, fast.SnapshotsSubmitted);
        Assert.False(slowClient.TryGetInterpolation(out _));
        Assert.True(fastClient.TryGetInterpolation(out var uninterrupted) && !uninterrupted.IsHolding);

        await match.TickAsync(675_000);
        await match.AdvanceAsync(700_000);
        Assert.Equal(new[] { 6UL, 7UL, 13UL }, slow.Snapshots.TakeLast(3).Select(s => s.Anchor.Tick));
        Assert.True(slowClient.TryGetInterpolation(out var recovered) && !recovered.IsHolding);

        // Repeat a future input before its deadline and verify its action changes state once.
        var input = new NetworkSyncInputV1<int, int>
        {
            MatchId = MatchSettings.MatchId,
            Epoch = 1,
            TargetTick = 16,
            T1Us = match.Now,
            Actions = [new NetworkSyncAction<int> { ActionId = 0, Payload = 7 }],
        };
        fast.Send(NetworkSyncPackets.Input, input.ToJson());
        input.T1Us++;
        fast.Send(NetworkSyncPackets.Input, input.ToJson());
        for (ulong tick = 15; tick <= 16; tick++)
            await match.TickAsync(match.TickDeadline(tick));
        await match.AdvanceAsync(825_000);
        Assert.Equal(InputDisposition.Duplicate, fast.Feedback[input.T1Us].Disposition);
        Assert.Equal(7, fast.Snapshots[^1].State);

        const ulong resyncAt = 1_400_000;
        await match.TickAsync(resyncAt, resync: true);
        await match.AdvanceAsync(resyncAt + 25_000);
        var reset = fast.Snapshots[^1];
        Assert.Equal((2UL, 0UL, 7), (reset.Anchor.Epoch, reset.Anchor.Tick, reset.State));
        Assert.False(fastClient.TryGetInterpolation(out _));
        Assert.False(await fastClient.SendInputAsync(0, [11]));
        await fastClient.Update();

        input.T1Us = match.Now;
        input.TargetTick = 2;
        fast.Send(NetworkSyncPackets.Input, input.ToJson());
        for (ulong tick = 1; tick <= 4; tick++)
            await match.TickAsync(match.TickDeadline(tick));
        Assert.Equal(InputDisposition.Rejected, fast.Feedback[input.T1Us].Disposition);
        Assert.Equal(7, fast.Snapshots[^1].State);
        Assert.True(fastClient.TryGetInterpolation(out var afterReset) && !afterReset.IsHolding);

        Assert.True(await fastClient.SendInputAsync(0, [11]));
        var current = fast.LastInput!;
        Assert.Equal(2UL, current.Epoch);
        Assert.Equal(0UL, current.Actions.Single().ActionId);
        for (var tick = 5UL; tick <= current.TargetTick + 1; tick++)
            await match.TickAsync(match.TickDeadline(tick));
        Assert.Equal(18, fast.Snapshots[^1].State);
    }

    private async Task<Measurement> MeasureAsync(int rate, NetworkProfile profile, bool fixedHalfRtt)
    {
        var elapsed = Stopwatch.StartNew();
        using var process = Process.GetCurrentProcess();
        var cpuBefore = process.TotalProcessorTime;
        var allocationBefore = GC.GetTotalAllocatedBytes();
        var match = new VirtualMatch(rate, profile);
        await match.StartAsync();
        var peer = await match.JoinAsync("player");
        using var client = Attach(peer, match);
        var clock = (MatchClock)ClientClock.GetValue(client)!;
        var sent = new List<NetworkSyncInputV1<int, int>>(InputSamples);
        var renders = 0;
        var missing = 0;
        var maxErrorUs = 0m;
        var slot = 0UL;
        await client.Update();

        // The tick timer, input and render schedules use absolute virtual deadlines.
        for (ulong at = RenderPeriodUs; at < DurationUs; at += RenderPeriodUs)
        {
            while (match.TickDeadline(slot + 1) <= at)
                await match.TickAsync(match.TickDeadline(++slot));
            await match.AdvanceAsync(at);
            await client.Update();
            if (at % InputPeriodUs == WarmupUs % InputPeriodUs)
            {
                if (fixedHalfRtt)
                    ClientInputLead.SetValue(clock, (decimal)(profile.UpUs + profile.SpotQueueUs + profile.DownUs) / 2);
                var submitted = at >= WarmupUs
                    ? await client.SendInputAsync(0, [1])
                    : await client.SendInputAsync(0);
                if (at >= WarmupUs)
                {
                    Assert.True(submitted, $"No input at {at} µs.");
                    sent.Add(peer.LastInput!);
                }
            }
            if (at < WarmupUs)
                continue;

            Assert.True(clock.HasSample);
            var trueTick = (decimal)at * rate / 1_000_000;
            maxErrorUs = Math.Max(maxErrorUs, Math.Abs(clock.TickAt(at) - trueTick) * 1_000_000 / rate);
            renders++;
            if (!client.TryGetInterpolation(out var pair) || pair.IsHolding)
                missing++;
            else
            {
                Assert.True(pair.FromTick < pair.ToTick);
                Assert.InRange(pair.Ratio, 0, 1);
            }
        }

        var end = DurationUs + 500_000;
        while (match.TickDeadline(slot + 1) <= end)
            await match.TickAsync(match.TickDeadline(++slot));
        await match.AdvanceAsync(end);
        Assert.Equal(InputSamples, sent.Count);
        Assert.Equal(RenderSamples, renders);
        Assert.Equal(profile.SpotQueueUs, peer.MaxSpotWaitUs);
        Assert.NotEmpty(peer.Pongs);
        var late = 0;
        var missingFeedback = 0;
        foreach (var packet in sent)
        {
            if (!peer.Feedback.TryGetValue(packet.T1Us, out var feedback))
            {
                missingFeedback++;
                continue;
            }

            Assert.Equal(packet.TargetTick, feedback.TargetTick);
            Assert.True(feedback.Disposition is InputDisposition.Accepted or InputDisposition.Late,
                $"Unexpected input disposition {feedback.Disposition} at {packet.T1Us} µs.");
            Assert.Equal(feedback.ArrivalMarginUs <= 0, feedback.Disposition == InputDisposition.Late);
            if (feedback.Disposition == InputDisposition.Late)
                late++;
        }

        var errorLimitUs = Math.Abs((decimal)profile.UpUs - profile.DownUs) / 2 + 1_000_000m / rate + peer.ObservedJitterUs;
        output.WriteLine($"{profile.Name} {rate} Hz fixedHalfRtt={fixedHalfRtt} " +
                         $"tickError={maxErrorUs:F2}/{errorLimitUs:F2} us late={late}/{sent.Count} " +
                         $"missingFeedback={missingFeedback} interpolationShortage={missing}/{renders} " +
                         $"spotWait={peer.MaxSpotWaitUs} us jitter={peer.ObservedJitterUs} us " +
                         $"payloadBytes={match.PayloadBytes} peakEvents={match.PeakEvents} " +
                         $"elapsedMs={elapsed.Elapsed.TotalMilliseconds:F1} cpuMs={(process.TotalProcessorTime - cpuBefore).TotalMilliseconds:F1} " +
                         $"allocatedBytes={GC.GetTotalAllocatedBytes() - allocationBefore}");
        if (missingFeedback == 0)
            Assert.Equal(sent.Count - late, peer.Snapshots[^1].State);
        return new Measurement(maxErrorUs, errorLimitUs, late, missing, missingFeedback);
    }

    private static NetworkSyncClient<int, int, int> Attach(VirtualMatch.Peer peer, VirtualMatch match)
    {
        var client = new NetworkSyncClient<int, int, int>(() => match.Now);
        client.Attach(peer.Connector(), MatchSettings.MatchId);
        return client;
    }

    private sealed record Measurement(decimal ErrorUs, decimal LimitUs, int Late, int Missing, int MissingFeedback)
    {
        public IEnumerable<string> Failures()
        {
            if (ErrorUs > LimitUs)
                yield return "tick estimate";
            if (Late * 100 >= InputSamples)
                yield return LateMetric;
            if (Missing * 100 >= RenderSamples)
                yield return "interpolation shortage";
            if (MissingFeedback != 0)
                yield return "missing input feedback";
        }
    }
}
