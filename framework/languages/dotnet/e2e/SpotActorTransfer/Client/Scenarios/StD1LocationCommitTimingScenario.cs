// Verifies ST-D1 Location Commit Timing behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StD1LocationCommitTimingScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        await RunLocalLocationCommitTimingAsync(context);
        await RunRemoteLocationCommitTimingAsync(context);
    }

    private static async Task RunLocalLocationCommitTimingAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-location-local-{Guid.NewGuid():N}";
        var spotId = $"spot-location-local-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeA, spotId, "delay-joined");
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 51);
        var before = await context.GetActorRefAsync(context.NodeA, actorId);

        var joinTask = context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-D1", spotId));
        var waitingEvidence = await context.WaitEvidenceAsync(context.NodeA, [
            $"ST-D1|{actorId}|admission|spot={spotId}",
            $"ST-D1|{actorId}|joined_wait|{spotId}"
        ]);
        ZlinkStreamAssert.Ensure(
            !waitingEvidence.Any(item => SpotActorTransferScenarioContext.EvidenceText(item)
                .Contains($"ST-D1|{actorId}|success_reply|{spotId}", StringComparison.Ordinal)),
            "ST-D1 local join returned success before OnJoinedActorAsync completed.");
        var during = await context.GetActorRefAsync(context.NodeA, actorId);
        ZlinkStreamAssert.Ensure(
            during.Generation == before.Generation,
            $"ST-D1 local actor generation changed before joined completed. before={before.Generation}, during={during.Generation}");

        await context.ReleaseJoinedGateAsync(context.NodeA, spotId);
        var join = await joinTask;
        ZlinkStreamAssert.Ensure(join.Accepted, "ST-D1 local join was rejected.");
        var after = await context.GetActorRefAsync(context.NodeA, actorId);
        ZlinkStreamAssert.Ensure(after.Generation >= before.Generation, "ST-D1 local actor generation regressed after commit.");

        await context.WaitEvidenceAsync(context.NodeA, [
            $"ST-D1|{actorId}|joined_released|{spotId}",
            $"transfer|{actorId}|joined|{spotId}:51",
            $"ST-D1|{actorId}|success_reply|{spotId}"
        ]);
    }

    private static async Task RunRemoteLocationCommitTimingAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-location-remote-{Guid.NewGuid():N}";
        var spotId = $"spot-location-remote-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId, "delay-joined");
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 52);

        var joinTask = context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-D1", spotId));
        await context.WaitEvidenceAsync(context.NodeB, [
            $"ST-D1|{actorId}|admission|spot={spotId}",
            $"ST-D1|{actorId}|joined_wait|{spotId}"
        ]);
        //  Spec 15-spot-actor: "Application이 요청한 User Spot join은 target
        //  admission callback, commit 뒤 target joined와 source leave
        //  notification을 사용한다." Commit precedes the joined callback, so by
        //  the time joined_wait is observed the authority already names the
        //  target. Asserting the ref had NOT moved here contradicted that order.
        var sourceDuring = await context.GetActorRefAsync(context.NodeA, actorId);
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(sourceDuring.NodeRid, "actor-b"),
            $"ST-D1 remote ref was not committed before target joined ran. got={sourceDuring.NodeRid}");

        await context.ReleaseJoinedGateAsync(context.NodeB, spotId);
        var join = await joinTask;
        ZlinkStreamAssert.Ensure(join.Accepted, "ST-D1 remote join was rejected.");
        var targetAfter = await context.GetActorRefAsync(context.NodeB, actorId);
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(targetAfter.NodeRid, "actor-b"),
            $"ST-D1 remote target ref was not committed after joined completed. got={targetAfter.NodeRid}");

        //  Spec 15-spot-actor: "`Accepted`는 target Actor, `Rejected`와 commit 전
        //  `Failed`는 source Actor가 받는다." The source sees the leave
        //  notification; the Accepted completion belongs to the target, so
        //  success_reply is expected on NodeB, not NodeA.
        await context.WaitEvidenceAsync(context.NodeA, [
            $"transfer|{actorId}|leave|52"
        ]);
        await context.WaitEvidenceAsync(context.NodeB, [
            $"ST-D1|{actorId}|joined_released|{spotId}",
            $"transfer|{actorId}|joined|{spotId}:52",
            $"ST-D1|{actorId}|success_reply|{spotId}"
        ]);
    }
}
