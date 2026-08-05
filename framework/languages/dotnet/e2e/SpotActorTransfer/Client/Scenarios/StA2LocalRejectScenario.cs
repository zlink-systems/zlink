// Verifies ST-A2 Local Reject behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StA2LocalRejectScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-A2";
        var actorId = $"actor-local-reject-{Guid.NewGuid():N}";
        var spotId = $"spot-local-reject-{Guid.NewGuid():N}";
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            12);
        var source = context.NodeForRid(created.NodeRid);
        var target = await context.CreateSpotAsync(source, spotId, "reject");
        ZlinkStreamAssert.Ensure(
            string.Equals(
                target.NodeRid,
                created.NodeRid,
                StringComparison.Ordinal),
            $"{scenario} fixture did not create a same-node target.");

        var registration = await context.JoinAsync(
            source,
            actorId,
            new JoinTargetReq(scenario, spotId, "reject"));
        ZlinkStreamAssert.Ensure(
            registration.Accepted,
            $"{scenario} handler did not register Deferred Join.");

        var evidence = await context.WaitEvidenceAsync(
            source,
            [
                $"{scenario}|{actorId}|admission|spot={spotId}",
                $"{scenario}|{actorId}|reject_reply|{spotId}",
                $"{scenario}|{actorId}|typed_reject_reply|accepted=False;spot={spotId}"
            ]);
        var current = await context.GetActorRefAsync(source, actorId);
        ZlinkStreamAssert.Ensure(
            current.Generation == created.Generation
            && string.Equals(
                current.NodeRid,
                created.NodeRid,
                StringComparison.Ordinal),
            $"{scenario} rejected Join changed Actor identity or owner.");

        var marker = $"after-reject-{Guid.NewGuid():N}";
        await context.SendFromNodeAsync(
            context.NodeD,
            actorId,
            new HandoffPacket(scenario, marker));
        evidence = await context.WaitEvidenceAsync(
            source,
            [
                $"{scenario}|{actorId}|entry_handoff_packet|{marker}:state=12"
            ]);

        var forbiddenKinds = new HashSet<string>(
            [
                "leave",
                "joined",
                "authority_committed",
                "application_capture_started",
                "application_restore_started",
                "user_handoff_packet"
            ],
            StringComparer.Ordinal);
        ZlinkStreamAssert.Ensure(
            !evidence.Any(item =>
                item.ActorId == actorId
                && forbiddenKinds.Contains(item.Kind)),
            $"{scenario} rejected Join produced a relocation side effect.");
        ZlinkStreamAssert.Ensure(
            evidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "entry_handoff_packet"
                && item.Value == $"{marker}:state=12") == 1,
            $"{scenario} source membership did not handle the packet exactly once.");
        var admissionIndex = FindEvidenceIndex(
            evidence,
            scenario,
            actorId,
            "admission");
        var rejectionIndex = FindEvidenceIndex(
            evidence,
            scenario,
            actorId,
            "reject_reply");
        var packetIndex = FindEvidenceIndex(
            evidence,
            scenario,
            actorId,
            "entry_handoff_packet");
        ZlinkStreamAssert.Ensure(
            admissionIndex >= 0
            && admissionIndex < rejectionIndex
            && rejectionIndex < packetIndex,
            $"{scenario} terminal and source dispatch order is invalid.");
    }

    private static int FindEvidenceIndex(
        IReadOnlyList<ActorEvidence> evidence,
        string scenario,
        string actorId,
        string kind)
    {
        for (var index = 0; index < evidence.Count; index++)
        {
            var item = evidence[index];
            if (item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == kind)
                return index;
        }

        return -1;
    }
}
