package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdD2RemoteSpotAwaitScenario {
    private AtdD2RemoteSpotAwaitScenario() {
    }

    public static void run(ZLinkStreamConnector connector) {
        String requestId = "ATD-D2-" + UUID.randomUUID();
        String ownerSpot = requestId + "-owner";
        String targetSpot = requestId + "-target";

        Contracts.EnsureSpotRes owner = ClientStreamSupport.await(
            connector.request(new Contracts.EnsureSpotReq(ownerSpot))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.EnsureSpotRes.class);
        ScenarioAssert.that(ownerSpot.equals(owner.spotRid()), "ATD-D2 owner spot mismatch");
        ScenarioAssert.that("play-a".equals(owner.nodeRid()), "ATD-D2 owner node mismatch");

        Contracts.EnsureSpotRes target = ClientStreamSupport.await(
            connector.request(new Contracts.EnsureSpotReq(targetSpot))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-b")
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.EnsureSpotRes.class);
        ScenarioAssert.that(targetSpot.equals(target.spotRid()), "ATD-D2 target spot mismatch");
        ScenarioAssert.that("play-b".equals(target.nodeRid()), "ATD-D2 target node mismatch");

        Contracts.ScenarioRes reply = ClientStreamSupport.await(
            connector.request(new Contracts.RemoteSpotAwaitReq(requestId, targetSpot, 350))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, ownerSpot)
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.ScenarioRes.class);
        ScenarioAssert.that("ATD-D2".equals(reply.scenarioId()), "ATD-D2 reply scenario mismatch");
        ScenarioAssert.that(requestId.equals(reply.requestId()), "ATD-D2 reply request mismatch");
        ScenarioAssert.that("play-a".equals(reply.result()), "ATD-D2 continuation node mismatch");

        Contracts.EvidenceRes ownerEvidence = evidence(connector, requestId, "play-a");
        assertMarkers(ownerEvidence.markers(), List.of(
            "remote-await-started",
            "remote-await-released",
            "remote-await-resumed",
            "remote-await-completed"));
        ScenarioAssert.that(
            ownerEvidence.markers().stream()
                .filter(marker -> marker.startsWith("remote-await-resumed|"))
                .anyMatch(marker -> marker.contains("targetNode=play-b")),
            "ATD-D2 owner continuation did not observe play-b target");

        Contracts.EvidenceRes targetEvidence = evidence(connector, requestId, "play-b");
        assertMarkers(targetEvidence.markers(), List.of(
            "await-started",
            "await-released",
            "await-resumed",
            "await-completed"));
        ScenarioAssert.that(
            targetEvidence.markers().stream().noneMatch(marker -> marker.startsWith("remote-await-resumed|")),
            "ATD-D2 target node owned caller continuation");
        System.out.println("scenario ATD-D2 passed");
    }

    private static Contracts.EvidenceRes evidence(
        ZLinkStreamConnector connector,
        String requestId,
        String targetNode) {
        return ClientStreamSupport.await(
            connector.request(new Contracts.EvidenceReq(requestId))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, targetNode)
                .metadata(Contracts.SPOT_RID_METADATA, "room-a")
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.EvidenceRes.class);
    }

    private static void assertMarkers(
        List<String> actual,
        List<String> expected) {
        ScenarioAssert.that(actual.size() >= expected.size(), "ATD-D2 evidence marker count mismatch: " + actual);
        for (int i = 0; i < expected.size(); i++) {
            ScenarioAssert.that(
                actual.get(i).startsWith(expected.get(i) + "|"),
                "ATD-D2 evidence order mismatch: expected=" + expected + " actual=" + actual);
        }
    }
}
