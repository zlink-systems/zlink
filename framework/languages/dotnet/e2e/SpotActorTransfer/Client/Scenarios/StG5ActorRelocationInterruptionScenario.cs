// Verifies one Actor resumes service within the relocation interruption target.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StG5ActorRelocationInterruptionScenario
{
    internal static Task RunSmallAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, "small", expectTargetExceeded: false);

    internal static Task RunSlowCaptureAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, "slow-capture", expectTargetExceeded: true);

    internal static Task RunSlowRestoreAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, "slow-restore", expectTargetExceeded: true);

    internal static Task RunSlowCleanupAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(
            context,
            "slow-cleanup",
            expectTargetExceeded: false);

    private static async Task RunAsync(
        SpotActorTransferScenarioContext context,
        string profile,
        bool expectTargetExceeded)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"actor-st-g5-{profile}-{suffix}";
        _ = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            51,
            4 * 1024);
        var evidenceNodes = new[]
        {
            context.NodeA,
            context.NodeB,
            context.NodeC
        };
        var callbackBaseline = await ReadMembershipCallbackCountsAsync(
            context,
            evidenceNodes,
            actorId);

        await context.SetExclusivePlacementAsync(context.NodeB);
        var trafficScenario =
            $"ST-G5-ENTRY-ACTOR-{profile.ToUpperInvariant()}";
        var traffic = new RelocationBulkWorkload(
            context,
            trafficScenario,
            "actor",
            [actorId],
            operationsPerSecond: 20,
            submittingNode: context.NodeC,
            preservePerKindSubmissionOrder: true);
        var trafficTask = traffic.RunAsync(TimeSpan.FromSeconds(4));
        await Task.Delay(300);
        var relocation = await context.RelocateAsync(
            context.NodeA,
            TimeSpan.FromMinutes(2));
        var trafficResult = await trafficTask;
        ZlinkStreamAssert.Ensure(
            relocation.Outcome == "Relocated",
            $"ST-G5 expected Relocated, got {relocation.Outcome} "
            + $"({relocation.Reason}).");
        await RelocationBulkWorkloadVerification.VerifyAsync(
            context,
            trafficResult);

        var measurements = await context.WaitRelocationInterruptionAsync(
            context.NodeA,
            "actor",
            1,
            20_000,
            "entry");
        ZlinkStreamAssert.Ensure(
            measurements.Count >= 1,
            "ST-G5 did not observe the Entry Actor interruption.");
        var duration = measurements[^1].DurationSeconds;
        if (expectTargetExceeded)
        {
            ZlinkStreamAssert.Ensure(
                duration > 1,
                $"ST-G5 {profile} interruption did not exceed one second.");
            await context.WaitRuntimeEvidenceAsync(
                context.NodeA,
                20_000,
                "zlink.runtime.relocation.changed",
                "unit_kind=actor",
                "execution_mode=entry",
                "interruption_target_exceeded=True");
        }
        else
        {
            ZlinkStreamAssert.Ensure(
                duration <= 1,
                $"ST-G5 small interruption was {duration:F3}s.");
        }

        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var operationId = $"st-g5-{Guid.NewGuid():N}";
        var reply = await context.RequestActorWorkloadAsync(
            context.NodeC,
            new RelocationWorkloadCallReq(
                actorId,
                "ST-G5",
                1,
                operationId,
                now,
                now + 5_000));
        ZlinkStreamAssert.Ensure(
            reply.OperationId == operationId && reply.WithinDeadline,
            $"ST-G5 Actor '{actorId}' did not serve after relocation.");
        ZlinkStreamAssert.Ensure(
            !SpotActorTransferScenarioContext.IsNode(
                reply.NodeRid,
                "actor-a"),
            $"ST-G5 Actor '{actorId}' remained on the source.");
        var callbackAfter = await ReadMembershipCallbackCountsAsync(
            context,
            evidenceNodes,
            actorId);
        foreach (var kind in MembershipCallbackKinds)
            ZlinkStreamAssert.Ensure(
                callbackAfter.GetValueOrDefault(kind)
                == callbackBaseline.GetValueOrDefault(kind),
                $"ST-G5 infrastructure relocation invoked application "
                + $"membership callback '{kind}'.");

        var applicationObservedHandoffGapMilliseconds =
            await ReadApplicationObservedHandoffGapMillisecondsAsync(
                context,
                evidenceNodes,
                trafficScenario,
                actorId,
                sourceNodePrefix: "actor-a");
        Console.WriteLine(
            $"relocation_interruption_evidence selector={trafficScenario}"
            + " unit_kind=actor"
            + " execution_mode=entry"
            + $" duration_seconds={duration:F6}"
            + " source_last_handler_to_target_first_handler_or_reply_gap_ms="
            + applicationObservedHandoffGapMilliseconds
            + $" loss={trafficResult.RequestFailed + trafficResult.OneWayFailed}"
            + " duplicate=0"
            + $" request_count={trafficResult.RequestSucceeded}"
            + $" one_way_count={trafficResult.OneWaySucceeded}");
    }

    private static readonly string[] MembershipCallbackKinds =
    [
        "create",
        "admission",
        "entry_joined",
        "joined",
        "leave",
        "target_leave"
    ];

    private static async Task<Dictionary<string, int>>
        ReadMembershipCallbackCountsAsync(
            SpotActorTransferScenarioContext context,
            IEnumerable<ZLinkHttpClient> nodes,
            string actorId)
    {
        var counts = MembershipCallbackKinds.ToDictionary(
            static kind => kind,
            static _ => 0,
            StringComparer.Ordinal);
        foreach (var node in nodes)
        {
            var evidence = await context.GetEvidenceAsync(node);
            foreach (var item in evidence)
                if (StringComparer.Ordinal.Equals(item.ActorId, actorId)
                    && counts.ContainsKey(item.Kind))
                    counts[item.Kind] = checked(counts[item.Kind] + 1);
        }
        return counts;
    }

    private static async Task<long>
        ReadApplicationObservedHandoffGapMillisecondsAsync(
        SpotActorTransferScenarioContext context,
        IEnumerable<ZLinkHttpClient> nodes,
        string scenario,
        string actorId,
        string sourceNodePrefix)
    {
        var observed = new List<ActorEvidence>();
        foreach (var node in nodes)
        {
            var evidence = await context.GetEvidenceAsync(node);
            observed.AddRange(evidence
                .Where(item =>
                    StringComparer.Ordinal.Equals(item.Scenario, scenario)
                    && StringComparer.Ordinal.Equals(item.ActorId, actorId)
                    && item.Kind is "workload_request"
                        or "workload_one_way"));
        }

        var sourceLast = observed
            .Where(item => IsEvidenceNode(item.NodeRid, sourceNodePrefix))
            .Select(static item => item.ObservedUnixTimeMilliseconds)
            .DefaultIfEmpty(-1)
            .Max();
        var targetFirst = observed
            .Where(item => !IsEvidenceNode(
                item.NodeRid,
                sourceNodePrefix))
            .Select(static item => item.ObservedUnixTimeMilliseconds)
            .DefaultIfEmpty(-1)
            .Min();
        ZlinkStreamAssert.Ensure(
            sourceLast >= 0 && targetFirst >= 0,
            $"{scenario} did not observe both source and target handler "
            + "evidence.");
        ZlinkStreamAssert.Ensure(
            targetFirst >= sourceLast,
            $"{scenario} target handler evidence preceded the last source "
            + "handler completion.");
        return targetFirst - sourceLast;
    }

    private static bool IsEvidenceNode(
        string nodeRid,
        string diagnosticPrefix) =>
        StringComparer.Ordinal.Equals(nodeRid, diagnosticPrefix)
        || SpotActorTransferScenarioContext.IsNode(
            nodeRid,
            diagnosticPrefix);
}
