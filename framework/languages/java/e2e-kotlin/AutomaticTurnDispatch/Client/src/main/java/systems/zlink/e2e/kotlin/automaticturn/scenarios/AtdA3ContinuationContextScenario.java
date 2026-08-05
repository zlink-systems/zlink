package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.UUID;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdA3ContinuationContextScenario {
    private AtdA3ContinuationContextScenario() {
    }

    public static void run(
        ZLinkStreamConnector roomA,
        String actorA,
        ZLinkStreamConnector roomB,
        String actorB) {
        String requestId = "ATD-A3-" + UUID.randomUUID();
        String spotRid = "room-a";
        String correlationId = "corr-a3";
        ClientStreamSupport.send(
            roomA.send(new Contracts.AwaitMsg(requestId, 80, correlationId))
                .metadata(Contracts.SPOT_RID_METADATA, spotRid));

        Contracts.EvidenceRes evidence = ClientStreamSupport.waitForEvidence(
            roomA,
            requestId,
            spotRid,
            "await-completed");
        ScenarioAssert.containsMarkersInOrder(
            evidence.markers(),
            "await-started",
            "await-released",
            "await-resumed",
            "await-completed");

        for (String marker : evidence.markers()) {
            if (!marker.startsWith("await-")) {
                continue;
            }
            ScenarioAssert.that(
                marker.contains("|spot=" + spotRid + ";"),
                "ATD-A3 continuation marker lost target spot: " + marker);
            ScenarioAssert.that(
                marker.contains(";correlation=" + correlationId + ";"),
                "ATD-A3 continuation marker lost correlation id: " + marker);
        }
        System.out.println("scenario ATD-A3 passed");
    }
}
