using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Host;

internal enum ZLinkRelocationUnitOutcome
{
    Pending = 0,
    Completed = 1,
    TerminalFailure = 2
}

internal enum ZLinkRelocationCommitKnowledge
{
    NotCommitted = 0,
    Committed = 1,
    Unknown = 2
}

internal readonly record struct ZLinkRelocationUnitResult
{
    private ZLinkRelocationUnitResult(
        ZLinkRelocationUnitOutcome outcome,
        ZLinkRelocationCommitKnowledge commitKnowledge,
        ZLinkFrameworkRelocationReason? terminalReason,
        bool sourceTerminalized)
    {
        Outcome = outcome;
        CommitKnowledge = commitKnowledge;
        TerminalReason = terminalReason;
        SourceTerminalized = sourceTerminalized;
    }

    internal ZLinkRelocationUnitOutcome Outcome { get; }

    internal ZLinkRelocationCommitKnowledge CommitKnowledge { get; }

    internal ZLinkFrameworkRelocationReason? TerminalReason { get; }

    internal bool SourceTerminalized { get; }

    internal ulong CommittedUnitCount =>
        CommitKnowledge == ZLinkRelocationCommitKnowledge.Committed ? 1UL : 0UL;

    internal static ZLinkRelocationUnitResult Pending() =>
        new(
            ZLinkRelocationUnitOutcome.Pending,
            ZLinkRelocationCommitKnowledge.NotCommitted,
            null,
            true);

    internal static ZLinkRelocationUnitResult Completed() =>
        new(
            ZLinkRelocationUnitOutcome.Completed,
            ZLinkRelocationCommitKnowledge.Committed,
            null,
            true);

    internal static ZLinkRelocationUnitResult Terminal(
        ZLinkFrameworkRelocationReason reason,
        ZLinkRelocationCommitKnowledge commitKnowledge,
        bool sourceTerminalized) =>
        new(
            ZLinkRelocationUnitOutcome.TerminalFailure,
            commitKnowledge,
            reason,
            sourceTerminalized);
}

internal readonly record struct ZLinkRelocationWorkloadDrainControl(
    Func<bool> StopRequested,
    CancellationToken CancellationToken,
    DateTimeOffset AbsoluteDeadline = default);

internal sealed class ZLinkRelocationWorkloadCoordinator(
    Func<ZLinkSpotRelocationPhase, CancellationToken,
        ValueTask<ZLinkSpotDrainResult>> drainSpots,
    Func<DateTimeOffset, CancellationToken, ValueTask<ZLinkActorDrainResult>> drainActors)
{
    internal async ValueTask<ZLinkRelocationWorkloadDrainResult> DrainAsync(
        ZLinkRelocationWorkloadDrainControl control)
    {
        var shells = await drainSpots(
                ZLinkSpotRelocationPhase.PerActorShells,
                control.CancellationToken)
            .ConfigureAwait(false);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_phase_per_actor completed={shells.Completed} committed={shells.CommittedUnitCount}");
        if (!shells.Completed || control.StopRequested())
            return new ZLinkRelocationWorkloadDrainResult(
                false,
                shells.TerminalReason,
                shells.CommittedUnitCount,
                shells.SourceTerminalized,
                shells.CommitKnowledge);

        var actors = await drainActors(
                control.AbsoluteDeadline,
                control.CancellationToken)
            .ConfigureAwait(false);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_phase_actors completed={actors.Completed} committed={actors.CommittedUnitCount} reason={actors.TerminalReason}");
        var committed = checked(
            shells.CommittedUnitCount + actors.CommittedUnitCount);
        if (!actors.Completed || actors.TerminalReason is not null)
            return new ZLinkRelocationWorkloadDrainResult(
                actors.Completed,
                actors.TerminalReason,
                committed,
                shells.SourceTerminalized && actors.SourceTerminalized,
                CombineCommitKnowledge(
                    shells.CommitKnowledge,
                    actors.CommitKnowledge,
                    committed));
        if (control.StopRequested())
            return new ZLinkRelocationWorkloadDrainResult(
                false,
                null,
                committed,
                shells.SourceTerminalized && actors.SourceTerminalized,
                CombineCommitKnowledge(
                    shells.CommitKnowledge,
                    actors.CommitKnowledge,
                    committed));

        var aggregates = await drainSpots(
                ZLinkSpotRelocationPhase.Aggregates,
                control.CancellationToken)
            .ConfigureAwait(false);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_phase_aggregates completed={aggregates.Completed} committed={aggregates.CommittedUnitCount}");
        return new ZLinkRelocationWorkloadDrainResult(
            aggregates.Completed,
            aggregates.TerminalReason,
            checked(committed + aggregates.CommittedUnitCount),
            shells.SourceTerminalized
            && actors.SourceTerminalized
            && aggregates.SourceTerminalized,
                CombineCommitKnowledge(
                    CombineCommitKnowledge(
                        shells.CommitKnowledge,
                        actors.CommitKnowledge,
                        committed),
                aggregates.CommitKnowledge,
                checked(committed + aggregates.CommittedUnitCount)));
    }

    private static ZLinkRelocationCommitKnowledge CombineCommitKnowledge(
        ZLinkRelocationCommitKnowledge left,
        ZLinkRelocationCommitKnowledge right,
        ulong committedUnitCount)
    {
        if (left == ZLinkRelocationCommitKnowledge.Unknown
            || right == ZLinkRelocationCommitKnowledge.Unknown)
            return ZLinkRelocationCommitKnowledge.Unknown;
        return left == ZLinkRelocationCommitKnowledge.Committed
               || right == ZLinkRelocationCommitKnowledge.Committed
               || committedUnitCount != 0
            ? ZLinkRelocationCommitKnowledge.Committed
            : ZLinkRelocationCommitKnowledge.NotCommitted;
    }
}
