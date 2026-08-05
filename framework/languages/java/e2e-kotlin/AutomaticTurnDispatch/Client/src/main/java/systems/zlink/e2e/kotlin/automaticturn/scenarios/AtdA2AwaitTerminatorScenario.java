package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import java.util.UUID;

public final class AtdA2AwaitTerminatorScenario {
    private AtdA2AwaitTerminatorScenario() {
    }

    public static void run(
        ZLinkStreamConnector connector,
        String actorId,
        AtdA1BasicTerminatorScenario.Result previous) {
        ScenarioAssert.that(actorId.equals(previous.actorId()), "ATD-A2 actor context mismatch");
        String requestId = "ATD-A2-" + UUID.randomUUID();
        ClientStreamSupport.send(
            connector.send(new Contracts.AwaitMsg(requestId, 1200, "corr-a2"))
                .metadata(Contracts.SPOT_RID_METADATA, "room-a"));
        ClientStreamSupport.waitForEvidence(connector, requestId, "await-released");
        ClientStreamSupport.send(
            connector.send(new Contracts.ProbeMsg(requestId, "await-probe"))
                .metadata(Contracts.SPOT_RID_METADATA, "room-a"));
        Contracts.EvidenceRes evidence = ClientStreamSupport.waitForEvidence(
            connector,
            requestId,
            "await-completed");
        ScenarioAssert.containsMarkersInOrder(
            evidence.markers(),
            "await-started",
            "await-released",
            "probe-started",
            "probe-completed",
            "await-resumed",
            "await-completed");
        System.out.println("scenario ATD-A2 passed");
    }
}
