package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmF6Scenario extends SpotServiceScenarioContext {
    private SmF6Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmF6Scenario(context).execute();
    }

    private void execute() {
        String endpointA = options().multiAHttpEndpoint();
        String endpointB = options().multiBHttpEndpoint();
        String key = UUID.randomUUID().toString().replace("-", "");
        String sourceSpot = "spot-sm-f6-source-" + key;
        String targetSpot = "spot-sm-f6-target-" + key;
        String actorId = "actor-sm-f6-" + key;
        String marker = "sm-f6-" + key;

        Contracts.CreateSpotRes target = postJson(
            endpointB,
            "/spot/create-user-local",
            new Contracts.CreateSpotReq(targetSpot),
            Contracts.CreateSpotRes.class);
        ensure(targetSpot.equals(target.spotRid()), "SM-F6 target spot create mismatch");
        ensure("multi-node-b".equals(target.nodeRid()), "SM-F6 target node mismatch");

        Contracts.SpotOnlyMeshRes mesh = postJson(
            endpointA,
            "/spot/spot-only/request-send",
            new Contracts.SpotOnlyMeshReq(sourceSpot, targetSpot, marker),
            Contracts.SpotOnlyMeshRes.class);
        ensure(sourceSpot.equals(mesh.sourceSpotRid()), "SM-F6 source spot mismatch");
        ensure(targetSpot.equals(mesh.targetSpotRid()), "SM-F6 request target mismatch");
        ensure(mesh.targetValue() == 7, "SM-F6 target request value mismatch");

        Contracts.SpotOnlyJoinRes join = postJson(
            endpointA,
            "/actor/spot-only-join",
            new Contracts.SpotOnlyJoinReq(targetSpot, actorId, marker),
            Contracts.SpotOnlyJoinRes.class);
        ensure(join.accepted(), "SM-F6 spot-only actor join was rejected");
        ensure(actorId.equals(join.actorId()), "SM-F6 actor join id mismatch");
        ensure(targetSpot.equals(join.targetSpotRid()), "SM-F6 actor join target mismatch");

        waitForEvidence(
            endpointB,
            List.of(
                "MultiNodeStateReq|multi-node-b|" + targetSpot + "|7",
                "MultiNodeStateMsg|multi-node-b|" + targetSpot + "|sm-f6-send-" + marker,
                "SpotOnlyActorJoined|multi-node-b|" + targetSpot + "|" + actorId));
        System.out.println("scenario SM-F6 passed");

    }
}
