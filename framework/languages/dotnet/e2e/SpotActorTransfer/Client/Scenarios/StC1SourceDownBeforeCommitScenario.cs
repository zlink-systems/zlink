// Verifies ST-C1 Source Down Before Commit behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StC1SourceDownBeforeCommitScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-source-down-before-commit-{Guid.NewGuid():N}";
        var spotId = $"spot-source-down-before-commit-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId);
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 62);

        var joinTask = context.JoinRawAsync(context.NodeA, actorId, new JoinTargetReq("ST-C1", spotId));
        await context.WaitEvidenceAsync(context.NodeB, [
            $"ST-C1|{actorId}|admission|spot={spotId}"
        ]);
        await context.WaitEvidenceAsync(context.NodeA, [
            $"transfer|{actorId}|transfer_out|62",
            $"ST-C1|{actorId}|before_commit_gate|62"
        ]);

        await context.CrashNodeAAndWaitUnavailableAsync();
        JoinResponse? response = null;
        try
        {
            response = await joinTask.WaitAsync(TimeSpan.FromSeconds(3));
        }
        catch (Exception ex) when (
            ex is TimeoutException or HttpRequestException or TaskCanceledException
            || ex is Zlink.Framework.Contracts.Errors.ZLinkFrameworkException)
        {
            // SIGKILL may abort the HTTP request before the app endpoint can return a failure body.
        }
        if (response is not null)
            ZlinkStreamAssert.Ensure(response.Accepted,
                "ST-C1 deferred join intent was not acknowledged before source shutdown.");

        // The runtime cleanup marker is the bounded proof required by the common
        // scenario; draining the target would also wait for its independent Spot
        // lifecycle and would not isolate admission cleanup.
        await context.WaitRuntimeEvidenceAsync(context.NodeB, 30000,
            $"pending_admission_expired actor={actorId}");
        var targetEvidence = await context.GetEvidenceAsync(context.NodeB);
        ZlinkStreamAssert.Ensure(
            !targetEvidence.Any(item => SpotActorTransferScenarioContext.EvidenceText(item)
                .Contains($"transfer|{actorId}|transfer_in|62", StringComparison.Ordinal)),
            "ST-C1 target should not transfer in without commit.");
        ZlinkStreamAssert.Ensure(
            !targetEvidence.Any(item => SpotActorTransferScenarioContext.EvidenceText(item)
                .Contains($"transfer|{actorId}|joined|{spotId}", StringComparison.Ordinal)),
            "ST-C1 target should not join without commit.");
        ZlinkStreamAssert.Ensure(
            !targetEvidence.Any(item => SpotActorTransferScenarioContext.EvidenceText(item)
                .Contains($"ST-C1|{actorId}|packet_handler|", StringComparison.Ordinal)),
            "ST-C1 target should not dispatch actor packets without commit.");
    }
}
