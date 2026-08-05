package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class TdBasicTurnScenario {
    private static final String SPOT_RID = "room-a";

    private TdBasicTurnScenario() {
    }

    public static void runSurface() {
        System.out.println("scenario TD-A1 passed");
    }

    public static void runAsyncHoldsTurn(ZLinkStreamConnector connector) {
        runInterleave(connector, "TD-A2", "await-held", List.of(
            "await-held", "await-resumed", "completed", "probe-started", "probe-completed"));
    }

    public static void runAsyncCompletion(ZLinkStreamConnector connector) {
        String requestId = id("TD-A4");
        sendAwait(connector, requestId, "TD-A4");
        Contracts.EvidenceRes evidence = ClientStreamSupport.waitForEvidence(
            connector, requestId, SPOT_RID, "completed");
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "await-held", "await-resumed", "completed");
        System.out.println("scenario TD-A4 passed");
    }

    public static void runYieldInterleave(ZLinkStreamConnector connector) {
        runInterleave(connector, "TD-B1", "yield-released", List.of(
            "yield-released", "probe-started", "probe-completed", "yield-resumed", "completed"));
    }

    public static void runYieldQueuedOrder(ZLinkStreamConnector connector) {
        String requestId = id("TD-B2");
        sendAwait(connector, requestId, "TD-B2");
        ClientStreamSupport.waitForEvidence(connector, requestId, SPOT_RID, "yield-released");
        for (int index = 1; index <= 3; index++) {
            ClientStreamSupport.send(connector.send(new Contracts.ProbeMsg(requestId, "probe-" + index))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, SPOT_RID));
        }
        Contracts.EvidenceRes evidence = ClientStreamSupport.waitForEvidence(
            connector, requestId, SPOT_RID, "completed");
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "yield-released",
            "probe-started", "probe-completed",
            "probe-started", "probe-completed",
            "probe-started", "probe-completed",
            "yield-resumed", "completed");
        System.out.println("scenario TD-B2 passed");
    }

    private static void runInterleave(
        ZLinkStreamConnector connector,
        String scenarioId,
        String waitingMarker,
        List<String> expected) {
        String requestId = id(scenarioId);
        sendAwait(connector, requestId, scenarioId);
        ClientStreamSupport.waitForEvidence(connector, requestId, SPOT_RID, waitingMarker);
        ClientStreamSupport.send(connector.send(new Contracts.ProbeMsg(requestId, "turn-probe"))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
            .metadata(Contracts.SPOT_RID_METADATA, SPOT_RID));
        Contracts.EvidenceRes evidence = ClientStreamSupport.waitForEvidence(
            connector, requestId, SPOT_RID, "probe-completed");
        if (expected.contains("completed")) {
            evidence = ClientStreamSupport.waitForEvidence(
                connector, requestId, SPOT_RID, "completed");
        }
        ScenarioAssert.containsMarkersInOrder(evidence.markers(), expected.toArray(String[]::new));
        System.out.println("scenario " + scenarioId + " passed");
    }

    private static void sendAwait(
        ZLinkStreamConnector connector,
        String requestId,
        String scenarioId) {
        ClientStreamSupport.send(connector.send(new Contracts.AwaitMsg(requestId, 2_000, scenarioId))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
            .metadata(Contracts.SPOT_RID_METADATA, SPOT_RID));
    }

    private static String id(String scenarioId) {
        return scenarioId + "-" + UUID.randomUUID().toString().replace("-", "");
    }
}
