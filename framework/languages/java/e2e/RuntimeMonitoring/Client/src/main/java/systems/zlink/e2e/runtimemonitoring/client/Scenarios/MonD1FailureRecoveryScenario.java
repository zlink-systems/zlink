package systems.zlink.e2e.runtimemonitoring.client.Scenarios;

import systems.zlink.e2e.runtimemonitoring.client.Support.MonitoringScenarioContext;

public final class MonD1FailureRecoveryScenario {
    private MonD1FailureRecoveryScenario() {
    }

    public static void run(MonitoringScenarioContext context) {
        runRepeated(context, "MON-D1");
    }

    public static void runUnknownMesh(MonitoringScenarioContext context, String scenario) {
        context.postExpectFailure(
            context.serviceEndpoint(),
            "/runtime/unknown-mesh",
            scenario + " unknown MeshName was not rejected");
        System.out.println("scenario " + scenario + " passed");
    }

    public static void runRepeated(MonitoringScenarioContext context, String scenario) {
        String serviceA = context.serviceEndpoint();
        String serviceB = context.serviceBEndpoint();
        MonitoringScenarioContext.ensure(!serviceB.isBlank(), scenario + " requires service-b HTTP endpoint");
        for (int cycle = 1; cycle <= 3; cycle++) {
            context.shutdownServiceB(scenario + " cycle " + cycle + " did not stop");
            context.awaitRuntimeSnapshot(
                serviceA,
                snapshot -> snapshot.peers().stream()
                    .noneMatch(peer -> "READY".equals(peer.state())),
                scenario + " cycle " + cycle + " retained a ready peer");
            context.restartServiceB();
            context.waitForPort(serviceB, true, scenario + " cycle " + cycle + " did not restart");
            context.awaitRuntimeSnapshot(
                serviceA,
                MonitoringScenarioContext::routeMeshTargetReady,
                scenario + " cycle " + cycle + " did not become ready");
            context.post(serviceA, "/admin/drain");
            var reply = context.runtimeRequest(
                serviceA,
                scenario.toLowerCase() + "-request-" + cycle);
            MonitoringScenarioContext.ensure(
                "svc-b".equals(reply.providerRid()),
                scenario + " cycle " + cycle + " did not reach the restarted provider");
        }
        context.postBestEffort(serviceA, "/admin/restore");
        context.stopRestartedServiceB();
        System.out.println("scenario " + scenario + " passed");
    }
}
