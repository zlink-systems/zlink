// Verifies PerActor relocation recreates the stateless Spot shell and preserves Actor service.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StG3PerActorShellRelocationScenario
{
    internal static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-G3";
        var runId = Guid.NewGuid().ToString("N");
        var spotPrefix = $"st-g3-peractor-{runId}";

        await context.SetExclusivePlacementAsync(context.NodeA);
        RelocationBulkSpotCreateRes created;
        try
        {
            created = await context.CreateBulkSpotsAsync(
                context.NodeA,
                new RelocationBulkSpotCreateReq(
                    scenario,
                    spotPrefix,
                    Count: 1,
                    ApplicationStateBytes: 4 * 1024,
                    InstanceSpot: false,
                    MaxConcurrency: 8,
                    ActorsPerSpot: 100,
                    PerActor: true));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }

        ZlinkStreamAssert.Ensure(
            created.SpotIds.Length == 1
            && created.ActorIds.Length == 100
            && created.NodeRids.All(rid =>
                SpotActorTransferScenarioContext.IsNode(
                    rid,
                    "actor-a")),
            "ST-G3 did not create the PerActor shell and members on actor-a.");
        var before = await context.GetRelocationLocationsAsync(
            context.NodeC,
            created.ActorIds,
            created.SpotIds);

        await context.SetExclusivePlacementAsync(context.NodeB);
        try
        {
            var relocation = await context.RelocateAsync(
                context.NodeA,
                TimeSpan.FromMinutes(2));
            ZlinkStreamAssert.Ensure(
                string.Equals(
                    relocation.Outcome,
                    "Relocated",
                    StringComparison.OrdinalIgnoreCase),
                $"ST-G3 expected Relocated, got {relocation.Outcome} "
                + $"({relocation.Reason}).");

            var after = await context.GetRelocationLocationsAsync(
                context.NodeC,
                created.ActorIds,
                created.SpotIds);
            var beforeByKey = before.ToDictionary(
                item => $"{item.ObjectKind}:{item.ObjectId}",
                StringComparer.Ordinal);
            ZlinkStreamAssert.Ensure(
                after.Count == before.Count
                && after.All(item =>
                    SpotActorTransferScenarioContext.IsNode(
                        item.NodeRid,
                        "actor-b")
                    && beforeByKey[
                            $"{item.ObjectKind}:{item.ObjectId}"]
                        .ObjectGeneration
                    == item.ObjectGeneration),
                "ST-G3 did not preserve generation or exact shell target "
                + "for every PerActor member.");

            var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            foreach (var actorId in created.ActorIds)
            {
                var reply = await context.RequestActorWorkloadAsync(
                    context.NodeC,
                    new RelocationWorkloadCallReq(
                        actorId,
                        scenario,
                        1,
                        $"st-g3-actor-{actorId}",
                        now,
                        now + 30_000,
                        30_000));
                ZlinkStreamAssert.Ensure(
                    SpotActorTransferScenarioContext.IsNode(
                        reply.NodeRid,
                        "actor-b"),
                    $"ST-G3 Actor '{actorId}' did not dispatch on actor-b.");
            }

            var spotReply = await context.RequestSpotWorkloadAsync(
                context.NodeC,
                new RelocationWorkloadCallReq(
                    created.SpotIds[0],
                    scenario,
                    1,
                    "st-g3-spot",
                    now,
                    now + 30_000,
                    30_000));
            ZlinkStreamAssert.Ensure(
                SpotActorTransferScenarioContext.IsNode(
                    spotReply.NodeRid,
                    "actor-b"),
                "ST-G3 PerActor shell did not dispatch on actor-b.");

            var sourceEvidence = await context.GetEvidenceAsync(context.NodeA);
            var targetEvidence = await context.GetEvidenceAsync(context.NodeB);
            foreach (var actorId in created.ActorIds)
            {
                var captured = sourceEvidence.Where(item =>
                        item.ActorId == actorId
                        && item.Kind == "application_payload")
                    .ToArray();
                var restored = targetEvidence.Where(item =>
                        item.ActorId == actorId
                        && item.Kind == "relocation_payload_restored")
                    .ToArray();
                var transferredIn = targetEvidence.Count(item =>
                    item.ActorId == actorId
                    && item.Kind == "transfer_in");
                ZlinkStreamAssert.Ensure(
                    captured.Length == 1
                    && restored.Length == 1
                    && captured[0].Value == restored[0].Value,
                    $"ST-G3 Actor '{actorId}' relocation payload was not "
                    + "restored exactly once with the captured bytes.");
                ZlinkStreamAssert.Ensure(
                    transferredIn == 1,
                    $"ST-G3 Actor '{actorId}' transfer-in callback count "
                    + $"was {transferredIn}, expected 1.");
            }
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync(context.NodeA);
        }
    }
}
