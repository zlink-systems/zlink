// SPDX-License-Identifier: LicenseRef-Zlink
using System.Text.Json.Nodes;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;

namespace Zlink.NetworkSync.Tests;

public sealed class TimelineTests : IDisposable
{
    private readonly TestConnector _transport;
    private readonly NetworkSyncClient<int, int, JsonNode> _client;
    private ulong _now;

    public TimelineTests()
    {
        var (connector, transport) = TestConnector.Create();
        _transport = transport;
        _client = new(() => _now);
        _client.Attach(connector, "match-1");
    }

    [Fact]
    public async Task InterpolatesThenHoldsUntilDeadline()
    {
        await PrepareAsync();
        _now = 124_000;
        Assert.True(_client.TryGetInterpolation(out var pair));
        Assert.Equal((42UL, 47UL), (pair.FromTick, pair.ToTick));
        Assert.Equal(0.5, pair.Ratio, 8);
        Assert.False(pair.IsHolding);

        _now = 248_999;
        Assert.True(_client.TryGetInterpolation(out pair));
        Assert.True(pair.IsHolding);
        Assert.Equal(47UL, pair.FromTick);
        _now++;
        Assert.False(_client.TryGetInterpolation(out _));
    }

    [Fact]
    public async Task WrongMatchAndExpiredEchoCannotSeedClock()
    {
        await _client.Update();
        _now = 1000;
        await SnapshotAsync(42);
        await SnapshotAsync(47);
        await PongAsync(match: "another");
        Assert.False(_client.TryGetInterpolation(out _));
        await PongAsync(t1: 1);
        Assert.False(_client.TryGetInterpolation(out _));

        _now = 2_000_000;
        await PongAsync();
        Assert.False(_client.TryGetInterpolation(out _));
        await _client.Update();
        _now += 1000;
        await PongAsync(t1: 2_000_000, probe: 1);
        _now = 2_124_000;
        Assert.True(_client.TryGetInterpolation(out var pair));
        Assert.Equal(0.5, pair.Ratio, 8);
    }

    [Fact]
    public async Task NonPositiveRttCannotBeReplayedLater()
    {
        await _client.Update();
        _now = 1000;
        await PongAsync(hold: 1000);
        await SnapshotAsync(42);
        await SnapshotAsync(47);
        _now = 124_000;
        await PongAsync(hold: 1000);
        Assert.False(_client.TryGetInterpolation(out _));
    }

    [Fact]
    public async Task PendingOlderProbeCannotMoveClockAfterNewerEcho()
    {
        await _client.Update();
        _now = 250_000;
        await _client.Update();
        _now += 1000;
        await PongAsync(t1: 250_000, probe: 1);
        await PongAsync(tick: 87);
        await SnapshotAsync(42);
        await SnapshotAsync(47);
        _now += 123_000;
        Assert.True(_client.TryGetInterpolation(out var pair));
        Assert.Equal(0.5, pair.Ratio, 8);
    }

    [Fact]
    public async Task ACurrentProbeFromALowerEpochCannotSeedClock()
    {
        await _client.Update();
        _now = 1000;
        await SnapshotAsync(42, epoch: 2);
        await SnapshotAsync(47, epoch: 2);
        await PongAsync(epoch: 1);
        Assert.False(_client.TryGetInterpolation(out _));

        await PongAsync(epoch: 2);
        _now += 123_000;
        Assert.True(_client.TryGetInterpolation(out var pair));
        Assert.Equal(0.5, pair.Ratio, 8);
    }

    [Fact]
    public async Task DuplicateEchoAndStaleSnapshotsCannotMoveTimeline()
    {
        await PrepareAsync(epoch: 2);
        _now = 124_000;
        await PongAsync(epoch: 2, tick: 90);
        await SnapshotAsync(90, epoch: 1);
        await SnapshotAsync(91, epoch: 3, match: "another");
        await PongAsync(epoch: 3, tick: 92);
        Assert.True(_client.TryGetInterpolation(out var pair));
        Assert.Equal((42UL, 47UL), (pair.FromTick, pair.ToTick));
        Assert.Equal(0.5, pair.Ratio, 8);
    }

    [Fact]
    public async Task WaitsForTwoDistinctSnapshotsInTheCurrentEpoch()
    {
        await _client.Update();
        _now = 1000;
        await PongAsync();
        await SnapshotAsync(42);
        await SnapshotAsync(42);
        _now = 124_000;
        Assert.False(_client.TryGetInterpolation(out _));
        await SnapshotAsync(47);
        Assert.True(_client.TryGetInterpolation(out _));

        await SnapshotAsync(0, epoch: 2);
        await SnapshotAsync(51, epoch: 1);
        Assert.False(_client.TryGetInterpolation(out _));
        Assert.False(await _client.SendInputAsync(5));

        await _client.Update();
        _now += 1000;
        await PongAsync(epoch: 2, tick: 0, probe: 1, t1: 124_000);
        await SnapshotAsync(5, epoch: 2);
        _now += 123_000;
        Assert.True(_client.TryGetInterpolation(out var pair));
        Assert.Equal((0UL, 5UL), (pair.FromTick, pair.ToTick));
    }

    [Fact]
    public async Task SendUsesEchoAdjustedRttAndEpochLocalActionIds()
    {
        await PrepareAsync(hold: 800);
        _now = 58_700;
        Assert.True(await _client.SendInputAsync(5, new[] { 20, 21 }));
        var first = Assert.IsType<JsonObject>(DecodeLastInput());
        Assert.Equal("49", (string?)first["target_tick"]);
        Assert.Equal("47", (string?)first["last_snapshot_tick"]);
        Assert.Equal("0", (string?)first["actions"]![0]!["action_id"]);
        Assert.Equal("1", (string?)first["actions"]![1]!["action_id"]);
        Assert.False(await _client.SendInputAsync(5, new[] { 22 }));

        _now += 10_000;
        Assert.True(await _client.SendInputAsync(6, new[] { 22 }));
        Assert.Equal("2", (string?)DecodeLastInput()["actions"]![0]!["action_id"]);
        var sentCount = _transport.Sent.Count;
        await _client.Update();
        Assert.Equal(sentCount, _transport.Sent.Count);

        await SnapshotAsync(0, epoch: 2);
        await _client.Update();
        _now += 1000;
        await PongAsync(epoch: 2, tick: 0, probe: 1, t1: 68_700);
        await SnapshotAsync(5, epoch: 2);
        Assert.True(await _client.SendInputAsync(7, new[] { 23 }));
        Assert.Equal("0", (string?)DecodeLastInput()["actions"]![0]!["action_id"]);
    }

    [Theory]
    [InlineData(-1000, "51")]
    [InlineData(100000, "50")]
    public async Task AdjustsInputLeadUsingArrivalMargin(long margin, string expectedTarget)
    {
        await PrepareAsync(hold: 800);
        _now = 58_700;
        Assert.True(await _client.SendInputAsync(5));

        _now = 59_700;
        await SnapshotAsync(48);
        var snapshot = Fixture("snapshot");
        SetAnchor(snapshot, 1, 48, "match-1");
        var feedback = snapshot["input_feedback"]!;
        feedback["target_tick"] = "49";
        feedback["t1_us"] = "58700";
        feedback["hold_us"] = "1";
        feedback["arrival_margin_us"] = margin.ToString();
        feedback["disposition"] = margin < 0 ? "Late" : "Accepted";
        await _transport.Receive("NetworkSyncSnapshotV1", snapshot.ToJsonString());

        _now = 66_500;
        Assert.True(await _client.SendInputAsync(6));
        Assert.Equal(expectedTarget, (string?)DecodeLastInput()["target_tick"]);
    }

    [Fact]
    public async Task DisposeUnregistersClientHandlers()
    {
        await PrepareAsync();
        Assert.Equal(2, _transport.Subscriptions);
        _client.Dispose();
        Assert.Equal(0, _transport.Subscriptions);
        Assert.False(_client.TryGetInterpolation(out _));
    }

    public void Dispose() => _client.Dispose();

    private async Task PrepareAsync(ulong epoch = 1, ulong hold = 5)
    {
        await _client.Update();
        _now = 1000;
        await PongAsync(epoch: epoch, hold: hold);
        await SnapshotAsync(42, epoch);
        await SnapshotAsync(47, epoch);
    }

    private JsonNode DecodeLastInput()
    {
        var sent = _transport.Sent[^1];
        Assert.Equal("NetworkSyncInputV1", sent.Name);
        return sent.Payload.FromJson<JsonNode>();
    }

    private ValueTask PongAsync(
        ulong epoch = 1, ulong tick = 42, ulong probe = 0, ulong t1 = 0, ulong hold = 5, string match = "match-1")
    {
        var pong = Fixture("pong");
        SetAnchor(pong, epoch, tick, match);
        pong["probe_id"] = probe.ToString();
        pong["t1_us"] = t1.ToString();
        pong["hold_us"] = hold.ToString();
        return _transport.Receive("NetworkSyncPongV1", pong.ToJsonString());
    }

    private ValueTask SnapshotAsync(ulong tick, ulong epoch = 1, string match = "match-1")
    {
        var snapshot = Fixture("snapshot-null");
        SetAnchor(snapshot, epoch, tick, match);
        return _transport.Receive("NetworkSyncSnapshotV1", snapshot.ToJsonString());
    }

    private static void SetAnchor(JsonNode packet, ulong epoch, ulong tick, string match)
    {
        var anchor = packet["anchor"]!;
        anchor["epoch"] = epoch.ToString();
        anchor["tick"] = tick.ToString();
        anchor["tick_us"] = "10000";
        anchor["match_id"] = match;
    }

    private static JsonNode Fixture(string name) => JsonNode.Parse(
        JsonNode.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "fixtures.json")))!
            .AsArray().Single(row => (string?)row!["name"] == name)!["payload"]!.GetValue<string>())!;
}
