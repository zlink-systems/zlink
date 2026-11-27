using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Streams;
using Zlink.NetworkSync.Simulation;

namespace Zlink.NetworkSync.Server;

internal readonly record struct FullSnapshot<TState>(TickContext Tick, TState State);

internal readonly record struct InputSample(ulong TargetTick, ulong T1Us, long ReceivedAt, InputFeedback Result);

internal sealed class SnapshotRecipient<TState>(
    string actorId, IZLinkBoundSession session, ILogger logger)
{
    private Task? _admission;
    private PendingSnapshot? _pending;

    public ulong? LastAppliedInputTick { get; private set; }
    public bool UsedFallbackInput { get; private set; }
    public InputSample? Feedback { get; set; }

    public void RecordAppliedInput(long? tick, bool usedFallback)
    {
        LastAppliedInputTick = (ulong?)tick;
        UsedFallbackInput = usedFallback;
    }

    public void ResetEpoch()
    {
        RecordAppliedInput(null, false);
        Feedback = null;
    }

    public void Replace(FullSnapshot<TState> snapshot) =>
        _pending = new(snapshot, LastAppliedInputTick, UsedFallbackInput);

    public void Flush(string matchId, ulong tickUs, TimeProvider clock)
    {
        if (!IsAvailable() || _pending is not { } snapshot)
            return;

        _pending = null;
        var now = clock.GetTimestamp();
        var feedback = Feedback is { } sample ? ToWireFeedback(sample, now, clock) : (FeedbackV1?)null;
        Feedback = null;
        Begin(session.Send(new NetworkSyncSnapshotV1<TState>(
            Anchor(matchId, snapshot.Full.Tick, tickUs, now, clock),
            snapshot.Full.State, snapshot.LastAppliedInputTick, snapshot.UsedFallbackInput, feedback)));
    }

    public void ReplyToPing(NetworkSyncPingV1 ping, TickContext tick, ulong tickUs,
        long receivedAt, TimeProvider clock)
    {
        if (_pending.HasValue || !IsAvailable())
            return;

        var now = clock.GetTimestamp();
        Begin(session.Send(new NetworkSyncPongV1(
            Anchor(ping.MatchId, tick, tickUs, now, clock), ping.ProbeId, ping.T1Us,
            ElapsedUs(clock, receivedAt, now))));
    }

    public void Retire() => _ = ObserveAsync(_admission, logger, actorId);

    private bool IsAvailable()
    {
        if (_admission is { IsCompleted: false })
            return false;

        _ = ObserveAsync(_admission, logger, actorId);
        _admission = null;
        return true;
    }

    private void Begin(IZLinkBoundSessionSendCall call) => _admission = call.Async().AsTask();

    private static async Task ObserveAsync(Task? admission, ILogger logger, string actorId)
    {
        if (admission is null)
            return;

        try
        {
            await admission.ConfigureAwait(false);
        }
        catch (Exception error)
        {
            logger.LogWarning(error, "Network sync send admission failed for Actor {ActorId}.", actorId);
        }
    }

    private readonly record struct PendingSnapshot(
        FullSnapshot<TState> Full, ulong? LastAppliedInputTick, bool UsedFallbackInput);

    private static AnchorV1 Anchor(string matchId, TickContext tick, ulong tickUs, long now, TimeProvider clock) =>
        new(matchId, (ulong)tick.Epoch, (ulong)tick.Tick, tickUs, ElapsedUs(clock, tick.LogicalStartTimestamp, now));

    private static ulong ElapsedUs(TimeProvider clock, long start, long end) =>
        (ulong)(clock.GetElapsedTime(start, end).Ticks / TimeSpan.TicksPerMicrosecond);

    private static FeedbackV1 ToWireFeedback(InputSample sample, long now, TimeProvider clock)
    {
        var marginTicks = sample.Result.ArrivalMargin?.Ticks ?? 0;
        var marginUs = Math.DivRem(marginTicks, TimeSpan.TicksPerMicrosecond, out var remainder)
            + Math.Sign(remainder);
        return new(sample.TargetTick, sample.T1Us, ElapsedUs(clock, sample.ReceivedAt, now), marginUs,
            sample.Result.Disposition.ToWire());
    }
}
