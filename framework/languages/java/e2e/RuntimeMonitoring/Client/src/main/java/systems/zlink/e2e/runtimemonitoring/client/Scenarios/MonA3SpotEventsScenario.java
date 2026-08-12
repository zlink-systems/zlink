package systems.zlink.e2e.runtimemonitoring.client.Scenarios;

import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.runtimemonitoring.client.Support.MonitoringScenarioContext;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;

/** Verifies channel weight, ready membership, and actual select-one requests. */
public final class MonA3SpotEventsScenario {
    private MonA3SpotEventsScenario() {
    }

    public static void run(MonitoringScenarioContext context) {
        context.awaitRuntimeSnapshot(
            context.serviceEndpoint(),
            snapshot -> channel(snapshot).readyTargetCount() >= 1,
            "MON-A3 initial ready member count did not reach one");
        int evidenceBaseline = context.evidenceEntryCount(context.serviceEndpoint());

        context.post(context.serviceBEndpoint(), "/runtime/weight/zero");
        context.awaitRuntimeSnapshot(
            context.serviceEndpoint(),
            snapshot -> channel(snapshot).readyTargetCount() == 0,
            "MON-A3 zero-weight peer remained selectable");
        context.runtimeRequestExpectTargetNotFound(
            context.serviceEndpoint(), "zero-weight");

        context.post(context.serviceBEndpoint(), "/runtime/weight/restore");
        context.awaitRuntimeSnapshot(
            context.serviceEndpoint(),
            snapshot -> channel(snapshot).readyTargetCount() >= 1,
            "MON-A3 restored peer did not become selectable");

        boolean reachedServiceB = false;
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        int request = 0;
        while (System.nanoTime() < deadline && !reachedServiceB) {
            Contracts.WorkRes reply = context.runtimeRequest(
                context.serviceEndpoint(), "restored-" + request++);
            reachedServiceB = "svc-b".equals(reply.providerRid());
        }
        MonitoringScenarioContext.ensure(
            reachedServiceB,
            "MON-A3 restored service-b was not selected");
        context.waitForEvidenceAfter(
            context.serviceEndpoint(),
            evidenceBaseline,
            "route-mesh-runtime",
            "status-changed");

        System.out.println("scenario MON-A3 passed");
    }

    private static Contracts.RuntimeChannel channel(Contracts.RuntimeSnapshot snapshot) {
        return snapshot.channels().stream()
            .filter(value -> Contracts.SPOT_CHANNEL.equals(value.channelName()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException(
                "RouteMesh channel snapshot is missing"));
    }
}
