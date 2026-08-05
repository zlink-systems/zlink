package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdE2CancellationScenario {
    private AtdE2CancellationScenario() {
    }

    public static void run(ZLinkStreamConnector connector) {
        String requestId = "ATD-E2-" + UUID.randomUUID();
        ClientStreamSupport.send(
            connector.send(new Contracts.AwaitCancelMsg(requestId, 800, 100))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, "room-a"));
        waitForMarkers(connector, requestId, List.of(
            "cancel-await-started",
            "cancel-await-released",
            "cancel-await-completed"));
        Contracts.CleanupProbeRes probe = ClientStreamSupport.await(
            connector.request(new Contracts.CleanupProbeReq(requestId, "cancel-probe"))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, "room-a")
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.CleanupProbeRes.class);
        ScenarioAssert.that(requestId.equals(probe.requestId()), "ATD-E2 probe reply mismatch");
        waitForMarkers(connector, requestId, List.of(
            "cancel-await-started",
            "cancel-await-released",
            "cancel-await-completed",
            "probe-started",
            "probe-completed"));
        System.out.println("scenario ATD-E2 passed");
    }

    private static void waitForMarkers(
        ZLinkStreamConnector connector,
        String requestId,
        List<String> expected) {
        List<String> latest = List.of();
        for (int attempt = 0; attempt < 80; attempt++) {
            Contracts.EvidenceRes evidence = ClientStreamSupport.evidence(connector, requestId);
            latest = evidence.markers();
            if (startsWithMarkers(latest, expected)) {
                return;
            }
            ClientStreamSupport.sleep(100);
        }
        throw new IllegalStateException(
            "ATD-E2 markers not observed in order: expected=" + expected + " actual=" + latest);
    }

    private static boolean startsWithMarkers(
        List<String> actual,
        List<String> expected) {
        if (actual.size() < expected.size()) {
            return false;
        }
        for (int i = 0; i < expected.size(); i++) {
            if (!actual.get(i).startsWith(expected.get(i) + "|")) {
                return false;
            }
        }
        return true;
    }
}
