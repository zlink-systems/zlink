package systems.zlink.e2e.runtimemonitoring.client.Scenarios;

import systems.zlink.e2e.runtimemonitoring.client.Support.MonitoringScenarioContext;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;

/** Verifies public peer availability through a restart without exposing generation. */
public final class MonA2LocationEventsScenario {
    private MonA2LocationEventsScenario() {
    }

    public static void run(MonitoringScenarioContext context) {
        Contracts.RuntimePeer first = context.awaitRuntimeSnapshot(
                context.serviceEndpoint(),
                MonitoringScenarioContext::routeMeshTargetReady,
                "MON-A2 initial peer was not ready")
            .peers().stream()
            .filter(peer -> "READY".equals(peer.state()))
            .findFirst()
            .orElseThrow();
        int evidenceBaseline = context.evidenceEntryCount(context.serviceEndpoint());

        context.shutdownServiceB("MON-A2 service-b did not stop");
        context.awaitRuntimeSnapshot(
            context.serviceEndpoint(),
            snapshot -> snapshot.peers().stream()
                .noneMatch(peer -> "READY".equals(peer.state())),
            "MON-A2 stopped peer remained ready");

        context.restartServiceB();
        context.waitForPort(
            context.serviceBEndpoint(),
            true,
            "MON-A2 service-b did not restart");
        Contracts.RuntimePeer restarted = context.awaitRuntimeSnapshot(
                context.serviceEndpoint(),
                snapshot -> MonitoringScenarioContext.routeMeshTargetReady(snapshot)
                    && snapshot.peers().stream().anyMatch(peer ->
                        "READY".equals(peer.state())
                            && !peer.nodeRid().equals(first.nodeRid())),
                "MON-A2 restarted peer did not return to Ready")
            .peers().stream()
            .filter(peer ->
                "READY".equals(peer.state())
                    && !peer.nodeRid().equals(first.nodeRid()))
            .findFirst()
            .orElseThrow();

        MonitoringScenarioContext.ensure(
            !restarted.nodeRid().equals(first.nodeRid()),
            "MON-A2 restarted peer RID did not change");
        MonitoringScenarioContext.ensure(
            context.runtimeSnapshot(context.serviceEndpoint()).peers().stream()
                .noneMatch(peer ->
                    "READY".equals(peer.state())
                        && peer.nodeRid().equals(first.nodeRid())),
            "MON-A2 previous peer remained ready");
        MonitoringScenarioContext.ensure(
            restarted.unavailableReason().isBlank(),
            "MON-A2 Ready peer has an unavailable reason");
        context.waitForEvidenceAfter(
            context.serviceEndpoint(),
            evidenceBaseline,
            "route-mesh-runtime",
            "status-changed");

        System.out.println("scenario MON-A2 passed");
    }
}
