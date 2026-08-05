package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmQ9Scenario extends SpotServiceScenarioContext {
    private SmQ9Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmQ9Scenario(context).execute();
    }

    private void execute() {
        String endpointA = options().multiAHttpEndpoint();
        String endpointB = options().multiBHttpEndpoint();
        String spotA = "spot-sm-q9-a-" + UUID.randomUUID().toString().replace("-", "");
        String spotB = "spot-sm-q9-b-" + UUID.randomUUID().toString().replace("-", "");

        Contracts.MultiNodeCreateSpotRes createdA = postJson(
            endpointA,
            "/spot/create-local",
            new Contracts.MultiNodeCreateSpotReq(spotA, 0),
            Contracts.MultiNodeCreateSpotRes.class);
        Contracts.MultiNodeStateRes firstA = postJson(
            endpointA,
            "/spot/state/request",
            new Contracts.MultiNodeStateRouteReq(spotA, 11),
            Contracts.MultiNodeStateRes.class);
        Contracts.MultiNodeStateRes directA = postJson(
            endpointA,
            "/spot/state/request",
            new Contracts.MultiNodeStateRouteReq(spotA, 0),
            Contracts.MultiNodeStateRes.class);
        Contracts.EvidenceSnapshot evidenceA = postJson(
            endpointA,
            "/evidence/wait",
            new Contracts.EvidenceWaitReq(
                List.of("MultiNodeStateReq|" + createdA.nodeRid() + "|" + spotA + "|11"),
                10_000),
            Contracts.EvidenceSnapshot.class);

        Contracts.MultiNodeCreateSpotRes createdB = postJson(
            endpointB,
            "/spot/create-local",
            new Contracts.MultiNodeCreateSpotReq(spotB, 0),
            Contracts.MultiNodeCreateSpotRes.class);
        Contracts.MultiNodeStateRes firstB = postJson(
            endpointB,
            "/spot/state/request",
            new Contracts.MultiNodeStateRouteReq(spotB, 17),
            Contracts.MultiNodeStateRes.class);
        Contracts.MultiNodeStateRes directB = postJson(
            endpointB,
            "/spot/state/request",
            new Contracts.MultiNodeStateRouteReq(spotB, 0),
            Contracts.MultiNodeStateRes.class);
        Contracts.EvidenceSnapshot evidenceB = postJson(
            endpointB,
            "/evidence/wait",
            new Contracts.EvidenceWaitReq(
                List.of("MultiNodeStateReq|" + createdB.nodeRid() + "|" + spotB + "|17"),
                10_000),
            Contracts.EvidenceSnapshot.class);

        ensure(createdA.spotRid().equals(spotA), "multi-node A create spot mismatch");
        ensure("multi-node-a".equals(createdA.nodeRid()), "multi-node A create reply node mismatch");
        ensure(firstA.value() == 11, "multi-node A route-to-spot reply value mismatch");
        ensure(directA.spotRid().equals(spotA), "multi-node A direct spot reply target mismatch");
        ensure("multi-node-a".equals(directA.nodeRid()), "multi-node A direct spot reply node mismatch");
        ensure(directA.value() == 11, "multi-node A direct spot reply value mismatch");
        ensure(countEvidence(evidenceA, "MultiNodeStateReq", spotA, "11") >= 2,
            "multi-node A did not process both route-to-spot requests");

        ensure(createdB.spotRid().equals(spotB), "multi-node B create spot mismatch");
        ensure("multi-node-b".equals(createdB.nodeRid()), "multi-node B create reply node mismatch");
        ensure(firstB.value() == 17, "multi-node B route-to-spot reply value mismatch");
        ensure(directB.spotRid().equals(spotB), "multi-node B direct spot reply target mismatch");
        ensure("multi-node-b".equals(directB.nodeRid()), "multi-node B direct spot reply node mismatch");
        ensure(directB.value() == 17, "multi-node B direct spot reply value mismatch");
        ensure(countEvidence(evidenceB, "MultiNodeStateReq", spotB, "17") >= 2,
            "multi-node B did not process both route-to-spot requests");

        System.out.println("multi-node diagnostic passed");

    }
}
