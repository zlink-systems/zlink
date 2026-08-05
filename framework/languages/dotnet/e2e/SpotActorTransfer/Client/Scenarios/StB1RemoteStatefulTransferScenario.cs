// Verifies ST-B1 Remote Stateful Transfer behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StB1RemoteStatefulTransferScenario
{
    public static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-B1";
        var actorId = $"actor-remote-ok-{Guid.NewGuid():N}";
        var spotId = $"spot-remote-ok-{Guid.NewGuid():N}";
        var createdActor = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            stateVersion: 21,
            applicationStateBytes: 4 * 1024);
        var source = context.NodeForRid(createdActor.NodeRid);
        var (target, targetPrefix) =
            context.OtherActorNode(createdActor.NodeRid);
        var spot = await context.CreateSpotAsync(target, spotId);
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(
                spot.NodeRid,
                targetPrefix),
            $"{scenario} target Spot was not placed on the selected "
            + "remote node.");
        await context.ResetRelocationBlobMeasurementsAsync(
            context.NodeA,
            context.NodeB,
            context.NodeC);

        var joinTask = context.JoinAsync(
            source,
            actorId,
            new JoinTargetReq(scenario, spotId));
        await context.WaitEvidenceAsync(
            source,
            [$"{scenario}|{actorId}|defer_registered|{spotId}"]);

        var queuedMarker = $"accepted-before-seal-{Guid.NewGuid():N}";
        await context.SendFromNodeAsync(
            context.NodeD,
            actorId,
            new HandoffPacket(scenario, queuedMarker));
        foreach (var node in new[] { source, target })
        {
            ZlinkStreamAssert.Ensure(
                !(await context.GetEvidenceAsync(node)).Any(item =>
                    item.Scenario == scenario
                    && item.ActorId == actorId
                    && item.Kind == "handoff_packet"
                    && item.Value == queuedMarker),
                $"{scenario} packet accepted before the seal was handled "
                + "before relocation started.");
        }

        var released = await context.ReleaseTransferGateAsync(
            source,
            actorId);
        ZlinkStreamAssert.Ensure(
            released.Released,
            $"{scenario} transfer gate was not released.");
        var registration = await joinTask;
        ZlinkStreamAssert.Ensure(
            registration.Accepted,
            $"{scenario} deferred Join registration was rejected.");

        await context.WaitEvidenceAsync(target, [
            $"{scenario}|{actorId}|admission|spot={spotId}",
            $"transfer|{actorId}|application_restore_started|actor",
            $"transfer|{actorId}|relocation_payload_restored|",
            $"transfer|{actorId}|transfer_in|21",
            $"transfer|{actorId}|joined|{spotId}:21",
            $"{scenario}|{actorId}|success_reply|{spotId}",
            $"{scenario}|{actorId}|handoff_packet|{queuedMarker}"
        ]);
        var owner = await context.WaitActorOwnerAsync(
            target,
            actorId,
            spot.NodeRid);
        ZlinkStreamAssert.Ensure(
            owner.Generation == createdActor.Generation,
            $"{scenario} ObjectGeneration changed from "
            + $"{createdActor.Generation} to {owner.Generation}.");

        await context.WaitEvidenceAsync(source, [
            $"transfer|{actorId}|application_capture_started|actor",
            $"transfer|{actorId}|application_payload|",
            $"transfer|{actorId}|transfer_out|21",
            $"transfer|{actorId}|leave|21"
        ]);

        var probe = await context.ProbeAsync(
            context.NodeD,
            actorId,
            new ProbeReq(scenario, "after-relocation"));
        ZlinkStreamAssert.Ensure(
            probe.NodeRid == spot.NodeRid,
            $"{scenario} probe expected {spot.NodeRid}, got "
            + $"{probe.NodeRid}.");
        ZlinkStreamAssert.Ensure(
            probe.StateVersion == 21,
            $"{scenario} state version expected 21, got "
            + $"{probe.StateVersion}.");
        ZlinkStreamAssert.Ensure(
            probe.SpotId == spotId,
            $"{scenario} probe expected Spot '{spotId}', got "
            + $"'{probe.SpotId}'.");
        await context.WaitEvidenceAsync(
            target,
            [$"{scenario}|{actorId}|packet_handler|after-relocation"]);

        var sourceEvidence = await context.GetEvidenceAsync(source);
        var targetEvidence = await context.GetEvidenceAsync(target);
        var captured = sourceEvidence.SingleOrDefault(item =>
            item.ActorId == actorId
            && item.Kind == "application_payload");
        var restored = targetEvidence.SingleOrDefault(item =>
            item.ActorId == actorId
            && item.Kind == "relocation_payload_restored");
        ZlinkStreamAssert.Ensure(
            captured is not null
            && restored is not null
            && captured.Value == restored.Value,
            $"{scenario} target did not restore the exact captured "
            + "application payload.");
        ZlinkStreamAssert.Ensure(
            targetEvidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "handoff_packet"
                && item.Value == queuedMarker) == 1,
            $"{scenario} accepted packet was not replayed exactly once.");
        ZlinkStreamAssert.Ensure(
            !sourceEvidence.Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "handoff_packet"
                && item.Value == queuedMarker),
            $"{scenario} accepted packet ran on the previous owner.");
        EnsureEvidenceOrder(
            targetEvidence,
            scenario,
            actorId,
            ("admission", $"spot={spotId}"),
            ("application_restore_started", "actor"),
            ("relocation_payload_restored", "bytes="),
            ("transfer_in", "21"),
            ("joined", $"{spotId}:21"),
            ("success_reply", spotId),
            ("handoff_packet", queuedMarker),
            ("packet_handler", "after-relocation"));

        var measurements = new List<RelocationBlobMeasurement>();
        foreach (var node in new[]
                 {
                     context.NodeA,
                     context.NodeB,
                     context.NodeC
                 })
            measurements.AddRange(
                await context.GetRelocationBlobMeasurementsAsync(node));
        var puts = measurements
            .Where(static item => item.Operation == "put")
            .ToArray();
        var reads = measurements
            .Where(static item => item.Operation == "read")
            .ToArray();
        ZlinkStreamAssert.Ensure(
            puts.Any(put => reads.Any(read =>
                read.OpaqueReferenceSha256
                    == put.OpaqueReferenceSha256
                && read.EncodedBytes == put.EncodedBytes
                && read.PayloadSha256 == put.PayloadSha256)),
            $"{scenario} did not read the immutable Relocation Store "
            + "payload written before authority publication.");
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
            var index = -1;
            for (var candidate = 0;
                 candidate < evidence.Count;
                 candidate++)
            {
                var item = evidence[candidate];
                if (item.ActorId == actorId
                    && item.Kind == kind
                    && item.Value.StartsWith(
                        valuePrefix,
                        StringComparison.Ordinal)
                    && (item.Scenario == scenario
                        || item.Scenario == "transfer"))
                {
                    index = candidate;
                    break;
                }
            }

            ZlinkStreamAssert.Ensure(
                index > previous,
                $"{scenario} evidence order is invalid at '{kind}'.");
            previous = index;
        }
    }
}
