// Verifies ST-A1 Local Accept behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StA1LocalAcceptScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        await context.ResetRelocationBlobMeasurementsAsync(
            context.NodeA,
            context.NodeB);
        var actorId = $"actor-local-ok-{Guid.NewGuid():N}";
        var spotId = $"spot-local-ok-{Guid.NewGuid():N}";
        var actor = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            11);
        var owner = context.NodeForRid(actor.NodeRid);
        await context.CreateSpotAsync(owner, spotId);

        var join = await context.JoinAsync(owner, actorId, new JoinTargetReq("ST-A1", spotId));
        ZlinkStreamAssert.Ensure(join.Accepted, "ST-A1 join was rejected.");

        await context.WaitEvidenceAsync(owner, [
            $"ST-A1|{actorId}|success_reply|{spotId}"
        ]);

        var probe = await context.ProbeAsync(owner, actorId, new ProbeReq("ST-A1", "after-joined"));
        ZlinkStreamAssert.Ensure(probe.NodeRid == actor.NodeRid,
            $"ST-A1 probe expected {actor.NodeRid}, got {probe.NodeRid}.");
        ZlinkStreamAssert.Ensure(probe.SpotId == spotId, "ST-A1 probe did not reach target spot.");

        var evidence = await context.WaitEvidenceAsync(owner, [
            $"ST-A1|{actorId}|admission|spot={spotId}",
            $"runtime|{actorId}|authority_committed|{spotId}",
            $"transfer|{actorId}|joined|{spotId}:11",
            $"transfer|{actorId}|leave|11",
            $"ST-A1|{actorId}|success_reply|{spotId}",
            $"ST-A1|{actorId}|packet_handler|after-joined"
        ]);
        ZlinkStreamAssert.Ensure(
            evidence.Any(item => SpotActorTransferScenarioContext.EvidenceText(item)
                .Contains($"ST-A1|{actorId}|packet_handler|after-joined", StringComparison.Ordinal)),
            "ST-A1 packet evidence missing.");

        var actorEvidence = evidence
            .Where(item => item.ActorId == actorId)
            .ToArray();
        //  The admission value packs spot, mode and input into one field, so it
        //  is matched by its spot prefix rather than by equality.
        var admission = Array.FindIndex(actorEvidence, item =>
            item.Scenario == "ST-A1"
            && item.Kind == "admission"
            && item.Value.StartsWith($"spot={spotId}", StringComparison.Ordinal));
        var authorityCommitted = Array.FindIndex(actorEvidence, item =>
            item.Scenario == "runtime"
            && item.Kind == "authority_committed"
            && item.Value == spotId);
        //  ActorEvidence is (Scenario, ActorId, Kind, Value, ...), so the
        //  transfer markers carry Scenario "transfer" with Kind "leave" and
        //  "joined". Matching Kind against "transfer" never resolved and left
        //  both indexes at -1, so this order check could not pass either way.
        var leave = Array.FindIndex(actorEvidence, item =>
            item.Scenario == "transfer"
            && item.Kind == "leave"
            && item.Value == "11");
        var joined = Array.FindIndex(actorEvidence, item =>
            item.Scenario == "transfer"
            && item.Kind == "joined"
            && item.Value == $"{spotId}:11");
        var successReply = Array.FindIndex(actorEvidence, item =>
            item.Scenario == "ST-A1"
            && item.Kind == "success_reply"
            && item.Value == spotId);
        //  30-implementation-gap §"location authority가 commit 순서를 소유한다"
        //  puts the CAS commit before the target OnJoinedActor and the source
        //  OnLeaveActor after it, so joined precedes leave.
        ZlinkStreamAssert.Ensure(
            admission >= 0
            && admission < authorityCommitted
            && authorityCommitted < joined
            && joined < leave
            && leave < successReply,
            "ST-A1 order must be admission -> authority_committed -> joined "
            + "-> leave -> success_reply.");

        var relocationArtifacts = (await context
                .GetRelocationBlobMeasurementsAsync(context.NodeA))
            .Concat(await context
                .GetRelocationBlobMeasurementsAsync(context.NodeB))
            .ToArray();
        ZlinkStreamAssert.Ensure(
            relocationArtifacts.Length == 0,
            "ST-A1 same-node Join must not create Relocation Store artifacts.");
    }
}
