using System.Threading.Channels;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.NetworkSync.Server;

namespace Zlink.NetworkSync.Interop;

public sealed class TestClock : TimeProvider
{
    private long _now;
    public ulong Now => (ulong)GetTimestamp();
    public override long TimestampFrequency => 1_000_000;
    public override long GetTimestamp() => Interlocked.Read(ref _now);
    public void Set(ulong microseconds) => Interlocked.Exchange(ref _now, checked((long)microseconds));
}

public sealed class TestPlayer(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
}

public sealed record MatchSettings(int TickRate, TestClock Clock)
{
    public const string MatchId = "interop-match";
    public const int SnapshotRate = 20;
    public TaskCompletionSource Joined { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource ReleaseTurn { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public Channel<bool> Barriers { get; } = Channel.CreateUnbounded<bool>();
}

public sealed class TestMatch(IZLinkSpotContext context, MatchSettings settings, ILogger<TestMatch> logger)
    : NetworkSyncSpot<TestMatch, TestPlayer, int, int, int>(
        context, logger, settings.TickRate, MatchSettings.SnapshotRate, clock: settings.Clock)
{
    private int _actions;
    private MatchSettings Settings { get; } = settings;

    protected override void ConfigureMatch() =>
        Context.Handlers.AddActorPacket<BarrierHandler, TestPlayer>(nameof(SpotBarrier));
    protected override ValueTask<ZLinkSpotActorJoinResult> AdmitPlayerAsync(
        string actorId, ZLinkMessage request, CancellationToken ct) =>
        ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

    protected override ValueTask OnPlayerJoinedAsync(TestPlayer actor, CancellationToken ct)
    {
        Settings.Joined.TrySetResult();
        return ValueTask.CompletedTask;
    }

    protected override int GetMissingInput(string actorId) => 0;

    protected override void Step(long epoch, long tick, NetworkSyncInputs<int, int> inputs)
    {
        for (var i = 0; i < inputs.Count; i++)
        {
            var input = inputs[i];
            for (var j = 0; j < input.ActionCount; j++)
                _actions += input.GetAction(j).Payload;
        }
    }

    protected override int CaptureSnapshot() => _actions;

    public sealed class BarrierHandler : IZLinkSpotActorSendHandler<TestMatch, TestPlayer, SpotBarrier>
    {
        public async ValueTask HandleAsync(TestMatch spot, TestPlayer actor, IZLinkMessageContext context,
            SpotBarrier message, CancellationToken ct)
        {
            spot.Settings.Barriers.Writer.TryWrite(message.Hold);
            if (message.Hold)
                await spot.Settings.ReleaseTurn.Task.WaitAsync(ct);
        }
    }
}

public sealed record SpotBarrier(bool Hold = false);
