package Scenarios;
import systems.zlink.e2e.spotservice.client.Scenarios;
import java.util.List;

import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmC6Scenario extends SpotServiceScenarioContext {
    private SmC6Scenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) {
        new SmC6Scenario(context).runScenario();
    }

    private void runScenario() {
        String suffix = UUID.randomUUID().toString().replace("-", "");
        String readySpot = "sm-c6-ready-" + suffix;
        String blockedSpot = "sm-c6-blocked-" + suffix;

        setPlacementWeight(options().httpAEndpoint(), 10_000);
        setPlacementWeight(options().httpBEndpoint(), 0);
        postJson(options().httpAEndpoint(), "/spot/create",
            new Contracts.CreateSpotReq(readySpot), Contracts.CreateSpotRes.class);
        setPlacementWeight(options().httpAEndpoint(), 0);
        setPlacementWeight(options().httpBEndpoint(), 10_000);
        postJson(options().httpBEndpoint(), "/spot/create",
            new Contracts.CreateSpotReq(blockedSpot), Contracts.CreateSpotRes.class);
        setPlacementWeight(options().httpAEndpoint(), 100);
        setPlacementWeight(options().httpBEndpoint(), 100);

        postJson(options().httpBEndpoint(), "/spot/c6/arm",
            new Contracts.GatedSpotCreateReq(blockedSpot), Contracts.OperationAccepted.class);
        sendOutbound(readySpot, "sm-c6-marker");

        waitForEvidence(options().httpAEndpoint(),
            List.of("SpotMeshMsg|" + readySpot + "|publish:sm-c6-marker"));
        waitForEvidence(options().httpBEndpoint(),
            List.of("SpotBackpressureEntered|" + blockedSpot + "|publish:sm-c6-marker"));
        Contracts.EvidenceSnapshot beforeRelease = readEvidence(options().httpBEndpoint());
        long blockedMessages = beforeRelease.entries().stream()
            .filter(entry -> entry.marker().equals("SpotMeshMsg"))
            .filter(entry -> entry.spotRid().equals(blockedSpot))
            .filter(entry -> entry.value().equals("publish:sm-c6-marker"))
            .count();
        ensure(blockedMessages == 0,
            "SM-C6 delivered the blocked target before its gate was released");

        postJson(options().httpBEndpoint(), "/spot/c6/release",
            new Contracts.GatedSpotCreateReq(blockedSpot), Contracts.OperationAccepted.class);
        waitForEvidence(options().httpBEndpoint(),
            List.of("SpotBackpressureResumed|" + blockedSpot + "|publish:sm-c6-marker"));
        Contracts.EvidenceSnapshot afterRelease = readEvidence(options().httpBEndpoint());
        long resumedMessages = afterRelease.entries().stream()
            .filter(entry -> entry.marker().equals("SpotMeshMsg"))
            .filter(entry -> entry.spotRid().equals(blockedSpot))
            .filter(entry -> entry.value().equals("publish:sm-c6-marker"))
            .count();
        ensure(resumedMessages == 1,
            "SM-C6 delivered the blocked target more than once after release");
        System.out.println("scenario SM-C6 passed");
    }
}
