package Scenarios;

import systems.zlink.e2e.runtimemonitoring.client.Scenarios;
import systems.zlink.e2e.runtimemonitoring.client.Support;
import systems.zlink.e2e.runtimemonitoring.client.Support.MonitoringScenarioContext;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;

public final class MonA4AvailabilityTransitionScenario {
    private MonA4AvailabilityTransitionScenario() {
    }

    public static void run(MonitoringScenarioContext context) {
        runReplacement(context, "MON-A4");
    }

    public static void runReplacement(MonitoringScenarioContext context, String scenario) {
        String serviceA = context.serviceEndpoint();
        String serviceB = context.serviceBEndpoint();
        MonitoringScenarioContext.ensure(!serviceB.isBlank(), scenario + " requires service-b");
        context.shutdownServiceB(scenario + " service-b did not stop");
        context.awaitRuntimeSnapshot(
            serviceA,
            snapshot -> snapshot.peers().stream()
                .noneMatch(peer -> "READY".equals(peer.state())),
            scenario + " stopped peer remained ready");
        context.restartServiceB();
        context.waitForPort(serviceB, true, scenario + " replacement did not start");
        context.awaitRuntimeSnapshot(
            serviceA,
            MonitoringScenarioContext::routeMeshTargetReady,
            scenario + " replacement did not become ready");
        var reply = context.runtimeRequest(
            serviceA,
            scenario.toLowerCase() + "-request");
        MonitoringScenarioContext.ensure(
            "svc-b".equals(reply.providerRid()),
            scenario + " request was not handled by replacement");
        System.out.println("scenario " + scenario + " passed");
    }

    public static void runCrashReplacement(MonitoringScenarioContext context, String scenario) {
        String serviceA = context.serviceEndpoint();
        String serviceB = context.serviceBEndpoint();
        MonitoringScenarioContext.ensure(!serviceB.isBlank(), scenario + " requires service-b");
        String crashedRid = context.awaitRuntimeSnapshot(
                serviceA,
                snapshot -> snapshot.peers().stream()
                    .anyMatch(peer -> "READY".equals(peer.state())),
                scenario + " did not have a ready peer before crash")
            .peers().stream()
            .filter(peer -> "READY".equals(peer.state()))
            .map(Contracts.RuntimePeer::nodeRid)
            .findFirst()
            .orElseThrow(() -> new IllegalStateException(
                scenario + " did not expose the crashed peer RID"));
        context.crashServiceB(scenario + " crashed peer did not stop");
        context.awaitRuntimeSnapshot(
            serviceA,
            snapshot -> snapshot.peers().stream()
                .noneMatch(peer -> crashedRid.equals(peer.nodeRid())),
            scenario + " stale peer was not removed after lease expiry");
        context.restartServiceB();
        context.waitForPort(serviceB, true, scenario + " replacement did not start");
        context.awaitRuntimeSnapshot(
            serviceA,
            snapshot -> MonitoringScenarioContext.routeMeshTargetReady(snapshot)
                && snapshot.peers().stream()
                    .anyMatch(peer -> "READY".equals(peer.state())
                        && !crashedRid.equals(peer.nodeRid())),
            scenario + " replacement did not become ready");
        var reply = context.runtimeRequest(
            serviceA,
            scenario.toLowerCase() + "-request");
        MonitoringScenarioContext.ensure(
            "svc-b".equals(reply.providerRid()),
            scenario + " request was not handled by replacement");
        System.out.println("scenario " + scenario + " passed");
    }
}
