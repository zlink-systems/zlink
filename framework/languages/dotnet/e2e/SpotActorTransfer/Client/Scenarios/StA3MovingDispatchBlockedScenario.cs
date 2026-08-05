// Verifies ST-A3 Moving Dispatch Blocked behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StA3MovingDispatchBlockedScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-A3";
        var actorId = $"actor-local-moving-{Guid.NewGuid():N}";
        var spotId = $"spot-local-moving-{Guid.NewGuid():N}";
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            13);
        var source = context.NodeForRid(created.NodeRid);
        var target = await context.CreateSpotAsync(
            source,
            spotId,
            "delay-joined");
        ZlinkStreamAssert.Ensure(
            string.Equals(
                target.NodeRid,
                created.NodeRid,
                StringComparison.Ordinal),
            $"{scenario} fixture did not create a same-node target.");

        var registration = await context.JoinAsync(
            source,
            actorId,
            new JoinTargetReq(scenario, spotId));
        ZlinkStreamAssert.Ensure(
            registration.Accepted,
            $"{scenario} handler did not register Deferred Join.");
        var waitingEvidence = await context.WaitEvidenceAsync(
            source,
            [
                $"{scenario}|{actorId}|admission|spot={spotId}",
                $"{scenario}|{actorId}|joined_wait|{spotId}"
            ]);
        ZlinkStreamAssert.Ensure(
            !waitingEvidence.Any(item =>
                item.ActorId == actorId
                && (item.Kind == "leave"
                    || item.Kind == "joined"
                    || item.Kind == "success_reply"
                    || item.Kind == "packet_handler")),
            $"{scenario} membership or Actor dispatch crossed the joined callback gate.");

        const string blockedMarker = "during-joined-wait";
        var blockedProbe = context.ProbeAsync(
            source,
            actorId,
            new ProbeReq(scenario, blockedMarker));
        var submittedEvidence = await context.WaitEvidenceAsync(
            source,
            [
                $"{scenario}|{actorId}|probe_submitted|{blockedMarker}"
            ]);
        ZlinkStreamAssert.Ensure(
            !blockedProbe.IsCompleted
            && !submittedEvidence.Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "packet_handler"
                && item.Value == blockedMarker),
            $"{scenario} Actor packet completed while OnJoinedActorAsync was blocked.");

        var release = await context.ReleaseJoinedGateAsync(source, spotId);
        ZlinkStreamAssert.Ensure(
            release.Released,
            $"{scenario} joined gate was already released.");

        var blocked = await blockedProbe;
        ZlinkStreamAssert.Ensure(
            blocked.SpotId == spotId
            && blocked.StateVersion == 13,
            $"{scenario} blocked packet did not resume on the target membership.");

        const string followUpMarker = "after-joined";
        var followUp = await context.ProbeFromNodeAsync(
            context.NodeD,
            actorId,
            new ProbeReq(scenario, followUpMarker));
        ZlinkStreamAssert.Ensure(
            followUp.Succeeded
            && followUp.Reply is
            {
                SpotId: var followUpSpotId,
                StateVersion: 13
            }
            && followUpSpotId == spotId,
            $"{scenario} follow-up packet did not use the target membership.");

        var completedEvidence = await context.WaitEvidenceAsync(
            source,
            [
                $"{scenario}|{actorId}|joined_released|{spotId}",
                $"transfer|{actorId}|joined|{spotId}:13",
                $"{scenario}|{actorId}|success_reply|{spotId}",
                $"{scenario}|{actorId}|packet_handler|{blockedMarker}",
                $"{scenario}|{actorId}|packet_handler|{followUpMarker}"
            ]);
        foreach (var marker in new[] { blockedMarker, followUpMarker })
            ZlinkStreamAssert.Ensure(
                completedEvidence.Count(item =>
                    item.Scenario == scenario
                    && item.ActorId == actorId
                    && item.Kind == "packet_handler"
                    && item.Value == marker) == 1,
                $"{scenario} packet '{marker}' was not handled exactly once.");

        var forbiddenKinds = new HashSet<string>(
            [
                "application_capture_started",
                "application_restore_started",
                "transfer_out",
                "transfer_in",
                "target_factory",
                "completed",
                "message_follow_relay"
            ],
            StringComparer.Ordinal);
        ZlinkStreamAssert.Ensure(
            !completedEvidence.Any(item =>
                item.ActorId == actorId
                && forbiddenKinds.Contains(item.Kind)),
            $"{scenario} local membership change used relocation machinery.");
        EnsureEvidenceOrder(
            completedEvidence,
            scenario,
            actorId,
            ("admission", $"spot={spotId}"),
            ("joined_wait", spotId),
            ("joined_released", spotId),
            ("joined", $"{spotId}:13"),
            ("success_reply", spotId),
            ("packet_handler", blockedMarker),
            ("packet_handler", followUpMarker));
    }

    private static void EnsureEvidenceOrder(
        IReadOnlyList<ActorEvidence> evidence,
        string scenario,
        string actorId,
        params (string Kind, string ValuePrefix)[] expected)
    {
        var previous = -1;
        foreach (var (kind, valuePrefix) in expected)
        {
            var current = -1;
            for (var index = previous + 1; index < evidence.Count; index++)
            {
                var item = evidence[index];
                if ((item.Scenario == scenario || item.Scenario == "transfer")
                    && item.ActorId == actorId
                    && item.Kind == kind
                    && item.Value.StartsWith(
                        valuePrefix,
                        StringComparison.Ordinal))
                {
                    current = index;
                    break;
                }
            }

            ZlinkStreamAssert.Ensure(
                current > previous,
                $"{scenario} evidence order is invalid at '{kind}'.");
            previous = current;
        }
    }
}
