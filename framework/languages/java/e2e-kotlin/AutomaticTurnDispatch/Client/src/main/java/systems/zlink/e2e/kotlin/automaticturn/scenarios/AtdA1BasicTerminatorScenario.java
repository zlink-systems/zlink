package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.UUID;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdA1BasicTerminatorScenario {
    private AtdA1BasicTerminatorScenario() {
    }

    public static Result run(ZLinkStreamConnector connector, String actorId) {
        String requestId = "ATD-A1-" + UUID.randomUUID();
        ClientStreamSupport.send(
            connector.send(new Contracts.HoldMsg(requestId, 350))
                .metadata(Contracts.SPOT_RID_METADATA, "room-a"));
        ClientStreamSupport.sleep(120);
        ClientStreamSupport.send(
            connector.send(new Contracts.ProbeMsg(requestId, "hold-probe"))
                .metadata(Contracts.SPOT_RID_METADATA, "room-a"));
        Contracts.EvidenceRes evidence = ClientStreamSupport.waitForEvidence(
            connector,
            requestId,
            "hold-completed");
        ScenarioAssert.containsMarkersInOrder(
            evidence.markers(),
            "hold-started",
            "probe-started",
            "probe-completed",
            "hold-resumed",
            "hold-completed");
        System.out.println("scenario ATD-A1 passed");
        return new Result(actorId);
    }

    public record Result(String actorId) {
    }
}
