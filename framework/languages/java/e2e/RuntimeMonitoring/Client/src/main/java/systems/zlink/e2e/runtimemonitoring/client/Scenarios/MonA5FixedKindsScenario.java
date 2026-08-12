package systems.zlink.e2e.runtimemonitoring.client.Scenarios;

import systems.zlink.e2e.runtimemonitoring.client.Support.MonitoringScenarioContext;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;

public final class MonA5FixedKindsScenario {
    private MonA5FixedKindsScenario() {
    }

    public static void run(MonitoringScenarioContext context) {
        Contracts.RuntimeSnapshot baseline = context.awaitRuntimeSnapshot(
            context.serviceEndpoint(),
            snapshot -> "READY".equals(snapshot.state()),
            "MON-A5 location runtime was not ready");
        int evidenceStart = context.evidenceEntryCount(context.serviceEndpoint());

        context.setRedisPaused(true);
        try {
            Contracts.RuntimeSnapshot degraded = context.awaitRuntimeSnapshot(
                context.serviceEndpoint(),
                snapshot -> "DEGRADED".equals(snapshot.state())
                    && "LOCATION_UNAVAILABLE".equals(
                        snapshot.placementUnavailableReason()),
                "MON-A5 location runtime did not become degraded");
            MonitoringScenarioContext.ensure(
                degraded.peers().stream()
                    .anyMatch(peer -> "READY".equals(peer.state()))
                    && degraded.channels().stream().anyMatch(channel ->
                        Contracts.SPOT_CHANNEL.equals(channel.channelName())
                            && channel.ready()),
                "MON-A5 store outage removed the admitted messaging path: " + degraded);
            Contracts.WorkRes reply =
                context.runtimeRequest(context.serviceEndpoint(), "mon-a5-during-outage");
            MonitoringScenarioContext.ensure(
                "work:mon-a5-during-outage".equals(reply.value()),
                "MON-A5 admitted messaging failed during the store outage");
            context.waitForEvidenceAfter(
                context.serviceEndpoint(), evidenceStart,
                "route-mesh-runtime", "status-changed");
        } finally {
            context.setRedisPaused(false);
        }

        Contracts.RuntimeSnapshot recovered = context.awaitRuntimeSnapshot(
            context.serviceEndpoint(),
            snapshot -> "READY".equals(snapshot.state())
                && snapshot.peers().stream()
                    .anyMatch(peer -> "READY".equals(peer.state())),
            "MON-A5 recovered snapshot did not revalidate the current topology");
        MonitoringScenarioContext.ensure(
            "READY".equals(recovered.state()),
            "MON-A5 recovered location state mismatch");
        System.out.println("scenario MON-A5 passed");
    }
}
