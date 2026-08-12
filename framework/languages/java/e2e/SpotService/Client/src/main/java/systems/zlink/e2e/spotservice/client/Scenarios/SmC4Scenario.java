package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmC4Scenario extends SpotServiceScenarioContext {
    private SmC4Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmC4Scenario(context).execute();
    }

    private void execute() {
        Contracts.OutboundRes reply = eventually(() -> requestOutbound("room-a", "c5-cross-node"));
        ensure("room-a".equals(reply.spotRid()), "SM-C5 wrong publisher spot");
        ensure("play-a".equals(reply.nodeRid()), "SM-C5 wrong publisher node");
        waitForPlayBEvidence(List.of("SpotMeshMsg|play-b|room-b|publish:c5-cross-node"));
        System.out.println("scenario SM-C5 passed");

    }
}
