package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA9Scenario extends SpotServiceScenarioContext {
    private SmA9Scenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) {
        new SmA9Scenario(context).execute();
    }

    private void execute() {
        String spotId = "spot-sm-a9-" + UUID.randomUUID().toString().replace("-", "");
        setPlacementWeight(options().httpAEndpoint(), 100);
        setPlacementWeight(options().httpBEndpoint(), 0);
        try {
            // Arm both role servers because the manager call is routed to the
            // selected owner; the gate is intentionally local to onInitialize.
            postJson(options().httpAEndpoint(), "/spot/a9/start",
                new Contracts.GatedSpotCreateReq(spotId), Contracts.GateControlRes.class);
            postJson(options().httpBEndpoint(), "/spot/a9/start",
                new Contracts.GatedSpotCreateReq(spotId), Contracts.GateControlRes.class);
            Contracts.SpotPublicationProbeRes held = eventually(() ->
                postJson(options().httpAEndpoint(), "/spot/a9/probe",
                    new Contracts.GatedSpotCreateReq(spotId), Contracts.SpotPublicationProbeRes.class));
            ensure(!held.requestSucceeded(),
                "SM-A9 held creation became routable before the initialization gate was released");
            postJson(options().httpAEndpoint(), "/spot/a9/release",
                new Contracts.GatedSpotCreateReq(spotId), Contracts.GateControlRes.class);
            postJson(options().httpBEndpoint(), "/spot/a9/release",
                new Contracts.GatedSpotCreateReq(spotId), Contracts.GateControlRes.class);
            Contracts.SpotPublicationProbeRes published = eventually(() -> {
                Contracts.SpotPublicationProbeRes result = postJson(options().httpAEndpoint(), "/spot/a9/probe",
                    new Contracts.GatedSpotCreateReq(spotId), Contracts.SpotPublicationProbeRes.class);
                ensure(result.found() && result.requestSucceeded(), "SM-A9 spot is not routable after release");
                return result;
            });
            ensure(published.foundNodeRid().equals(published.requestNodeRid()),
                "SM-A9 Find and request selected different owners");
            System.out.println("scenario SM-A9 passed");
        } finally {
            setPlacementWeight(options().httpAEndpoint(), 100);
            setPlacementWeight(options().httpBEndpoint(), 100);
        }
    }
}
