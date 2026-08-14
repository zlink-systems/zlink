// Verifies that a target crash after relocation publication does not trigger
// automatic recovery on a restarted process.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.Framework.Contracts.Errors;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StB5TargetCrashAggregateUnavailableScenario
{
    private static readonly TimeSpan ReplacementReadinessTimeout = TimeSpan.FromSeconds(45);

    internal static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-B5";
        var suffix = Guid.NewGuid().ToString("N");
        var spotIdPrefix = $"st-b5-target-crash-{suffix}";

        RelocationBulkSpotCreateRes created;
        await context.SetExclusivePlacementAsync(context.NodeA);
        try
        {
            created = await context.CreateBulkSpotsAsync(
                context.NodeA,
                new RelocationBulkSpotCreateReq(
                    scenario,
                    spotIdPrefix,
                    Count: 1,
                    ApplicationStateBytes: 4 * 1024,
                    InstanceSpot: false,
                    MaxConcurrency: 1,
                    ActorsPerSpot: 1,
                    PerActor: false,
                    ActorApplicationStateBytes: 4 * 1024));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }

        var spotId = created.SpotIds.Single();
        var actorId = created.ActorIds.Single();
        var initial = await context.GetRelocationLocationsAsync(
            context.NodeC,
            [actorId],
            [spotId]);
        ZlinkStreamAssert.Ensure(
            initial.Count == 2
            && initial.All(location =>
                SpotActorTransferScenarioContext.IsNode(
                    location.NodeRid,
                    "actor-a")),
            $"{scenario} did not create the complete source aggregate.");

        await context.SetExclusivePlacementAsync(context.NodeB);
        var targetProbe = await context.CreateBulkSpotsAsync(
            context.NodeA,
            new RelocationBulkSpotCreateReq(
                scenario + "-TARGET-PROBE",
                $"st-b5-target-probe-{suffix}",
                Count: 1,
                ApplicationStateBytes: 0,
                InstanceSpot: false,
                MaxConcurrency: 1));
        var failedTargetRid = targetProbe.NodeRids.Single();
        await context.ClosePayloadSpotAsync(
            context.NodeA,
            targetProbe.SpotIds.Single());

        await context.ArmTargetPublicationGateAsync(
            context.NodeB,
            actorId,
            scenario);
        var relocationTask = context.RelocateAsync(
            context.NodeA,
            TimeSpan.FromSeconds(20));
        await context.WaitEvidenceAsync(
            context.NodeB,
            [
                $"transfer|{actorId}|application_state_restored|",
                $"{scenario}|{actorId}|target_publication_gate|opaque-delete-batch|"
            ]);
        Console.Error.WriteLine("st_b5_target_publication_gate");
        Console.Error.Flush();
        await WaitForRunnerAckAsync(
            context.Options.TargetCrashCompletedAckFile,
            TimeSpan.FromSeconds(60));
        await SpotActorTransferScenarioContext.WaitUnavailableAsync(
            context.Options.NodeBUrl,
            "target process exit");

        await ObserveRelocationBestEffortAsync(relocationTask);

        var actorOperationId = Guid.NewGuid().ToString("N");
        var spotOperationId = Guid.NewGuid().ToString("N");
        var actorError = await CaptureErrorAsync(
            () => context.RequestActorWorkloadProbeAsync(
                context.NodeC,
                CreateRequest(
                    actorId,
                    scenario + "-ACTOR-UNAVAILABLE",
                    actorOperationId)));
        var spotError = await CaptureErrorAsync(
            () => context.RequestSpotWorkloadProbeAsync(
                context.NodeC,
                CreateRequest(
                    spotId,
                    scenario + "-SPOT-UNAVAILABLE",
                    spotOperationId)));

        ZlinkStreamAssert.Ensure(
            actorError == nameof(ZLinkFrameworkErrorKind.Unavailable),
            $"{scenario} Actor request after target crash returned '{actorError}'.");
        ZlinkStreamAssert.Ensure(
            spotError == nameof(ZLinkFrameworkErrorKind.Unavailable),
            $"{scenario} Spot request after target crash returned '{spotError}'.");

        await SpotActorTransferScenarioContext.WaitAvailableAsync(
            context.Options.NodeBUrl,
            "replacement actor-b startup",
            ReplacementReadinessTimeout);

        // A restarted target may be RouteMesh-ready, but it must not recreate
        // the committed aggregate or execute either post-crash operation.
        var sourceEvidence = await context.GetEvidenceAsync(context.NodeA);
        var replacementEvidence = await context.GetEvidenceAsync(context.NodeB);
        ZlinkStreamAssert.Ensure(
            sourceEvidence.All(item =>
                !item.Value.Contains(actorOperationId, StringComparison.Ordinal)
                && !item.Value.Contains(spotOperationId, StringComparison.Ordinal)),
            $"{scenario} the source executed a post-crash operation.");
        ZlinkStreamAssert.Ensure(
            replacementEvidence.All(item =>
                !item.Value.Contains(actorOperationId, StringComparison.Ordinal)
                && !item.Value.Contains(spotOperationId, StringComparison.Ordinal)),
            $"{scenario} a restarted target executed a post-crash operation.");
        ZlinkStreamAssert.Ensure(
            !replacementEvidence.Any(item =>
                item.Scenario == scenario
                && item.ActorId is not null
                && item.Kind is ("application_state_restored"
                    or "spot_application_state_restored")),
            $"{scenario} a restarted target restored the crashed aggregate.");

        Console.WriteLine(
            $"required_gate=spotwide_target_crash_unavailable status=passed"
            + $" spot={spotId} actor={actorId}"
            + $" failed_target={failedTargetRid}"
            + " replacement_recovery=false actor=Unavailable spot=Unavailable");
    }

    private static async Task ObserveRelocationBestEffortAsync(
        Task relocationTask)
    {
        try
        {
            await relocationTask.ConfigureAwait(false);
        }
        catch (Exception)
        {
            // A target process loss may close the source-side request before
            // its one-way cutover result is observed by this E2E client.
        }
    }

    private static async Task WaitForRunnerAckAsync(
        string? path,
        TimeSpan timeout)
    {
        ZlinkStreamAssert.Ensure(
            !string.IsNullOrWhiteSpace(path),
            "ST-B5 runner acknowledgement file is required.");
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            if (File.Exists(path))
                return;
            await Task.Delay(20);
        }

        throw new TimeoutException(
            $"ST-B5 runner acknowledgement was not observed: {path}");
    }

    private static RelocationWorkloadCallReq CreateRequest(
        string targetId,
        string requestScenario,
        string operationId)
    {
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        return new RelocationWorkloadCallReq(
            targetId,
            requestScenario,
            Sequence: 1,
            operationId,
            now,
            now + 5_000,
            TimeoutMilliseconds: 2_000);
    }

    private static async Task<string> CaptureErrorAsync(
        Func<Task<RelocationWorkloadProbeRes>> operation)
    {
        try
        {
            var result = await operation().ConfigureAwait(false);
            return result.Succeeded
                ? "Succeeded"
                : result.ErrorKind ?? "Unknown";
        }
        catch (ZLinkFrameworkException exception)
        {
            return exception.Kind.ToString();
        }
        catch (Exception exception)
        {
            return exception.GetType().Name;
        }
    }
}
