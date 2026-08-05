package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmG2Scenario extends SpotServiceScenarioContext {
    private SmG2Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmG2Scenario(context).execute();
    }

    private void execute() {
        String endpointA = options().httpAEndpoint();
        String endpointB = options().httpBEndpoint();
        String key = "key-sm-g2-" + UUID.randomUUID().toString().replace("-", "");
        String firstSpot = "spot-" + key + "-a";
        String secondSpot = "spot-" + key + "-b";

        Contracts.CreateSpotRes firstCreated = postJson(
            endpointA,
            "/spot/create",
            new Contracts.CreateSpotReq(firstSpot),
            Contracts.CreateSpotRes.class);
        Contracts.StateRes firstReply = postJson(
            endpointA,
            "/spot/state/request",
            new Contracts.SpotStateRouteReq(firstSpot, "owner-remap-first", 1),
            Contracts.StateRes.class);
        Contracts.EvidenceSnapshot firstEvidence = postJson(
            endpointA,
            "/evidence/wait",
            new Contracts.EvidenceWaitReq(
                List.of("StateReq|play-a|" + firstSpot + "|owner-remap-first-1"),
                10_000),
            Contracts.EvidenceSnapshot.class);

        Contracts.CreateSpotRes secondCreated = postJson(
            endpointB,
            "/spot/create",
            new Contracts.CreateSpotReq(secondSpot),
            Contracts.CreateSpotRes.class);
        Contracts.StateRes secondReply = postJson(
            endpointB,
            "/spot/state/request",
            new Contracts.SpotStateRouteReq(secondSpot, "owner-remap-second", 1),
            Contracts.StateRes.class);
        Contracts.EvidenceSnapshot secondEvidence = postJson(
            endpointB,
            "/evidence/wait",
            new Contracts.EvidenceWaitReq(
                List.of("StateReq|play-b|" + secondSpot + "|owner-remap-second-1"),
                10_000),
            Contracts.EvidenceSnapshot.class);

        ensure("play-a".equals(firstCreated.nodeRid()), "SM-G2 first owner create node mismatch");
        ensure("play-b".equals(secondCreated.nodeRid()), "SM-G2 remapped owner create node mismatch");
        ensure("play-a".equals(firstReply.nodeRid()), "SM-G2 first owner request node mismatch");
        ensure("play-b".equals(secondReply.nodeRid()), "SM-G2 remapped owner request node mismatch");
        ensure(!containsSpotEvidence(firstEvidence, secondSpot), "SM-G2 remapped owner leaked to play-a");
        ensure(!containsSpotEvidence(secondEvidence, firstSpot), "SM-G2 first owner leaked to play-b");
        System.out.println("scenario SM-G2 passed");

    }
}
