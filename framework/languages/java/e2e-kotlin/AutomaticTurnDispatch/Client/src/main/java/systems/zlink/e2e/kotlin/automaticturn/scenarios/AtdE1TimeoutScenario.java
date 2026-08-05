package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.UUID;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdE1TimeoutScenario {
    private AtdE1TimeoutScenario() {
    }

    public static void run(ZLinkStreamConnector connector) {
        String spotRid = "await-timeout-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.EnsureSpotRes spot = ClientStreamSupport.await(
            connector.request(new Contracts.EnsureSpotReq(spotRid))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.EnsureSpotRes.class);
        ScenarioAssert.that(spotRid.equals(spot.spotRid()), "ATD-E1 timeout spot creation mismatch");
        String requestId = "ATD-E1-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.AwaitTimeoutRes timeout = ClientStreamSupport.await(
            connector.request(new Contracts.AwaitTimeoutReq(requestId, 700, 100))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, spotRid)
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.AwaitTimeoutRes.class);
        ScenarioAssert.that(timeout.timedOut(), "ATD-E1 expected public timeout result");
        ScenarioAssert.that(requestId.equals(timeout.requestId()), "ATD-E1 timeout reply request mismatch");
        ScenarioAssert.that(spotRid.equals(timeout.spotRid()), "ATD-E1 timeout reply spot mismatch");
        Contracts.EvidenceRes timeoutEvidence = ClientStreamSupport.waitForEvidence(
            connector,
            requestId,
            spotRid,
            "timeout-await-completed");
        ScenarioAssert.that(timeoutEvidence.markers().stream().anyMatch(entry ->
                entry.startsWith("timeout-await-completed|") && entry.contains("error=")),
            "ATD-E1 timeout marker missing public error evidence");
        Contracts.CleanupProbeRes probe = ClientStreamSupport.await(
            connector.request(new Contracts.CleanupProbeReq(requestId, "timeout-probe"))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, spotRid)
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.CleanupProbeRes.class);
        ScenarioAssert.that(requestId.equals(probe.requestId()), "ATD-E1 probe reply mismatch");
        Contracts.EvidenceRes evidence = ClientStreamSupport.waitForEvidence(
            connector,
            requestId,
            spotRid,
            "probe-completed");
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "timeout-await-started",
            "timeout-await-released",
            "timeout-await-completed",
            "probe-started",
            "probe-completed");
        ScenarioAssert.that(evidence.markers().stream().anyMatch(entry ->
                entry.startsWith("probe-completed|") && entry.contains("marker=timeout-probe")),
            "ATD-E1 post-timeout probe marker missing");
        System.out.println("scenario ATD-E1 passed");
    }
}
