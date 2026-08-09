package Scenarios;

import systems.zlink.e2e.runtimemonitoring.client.Scenarios;
import systems.zlink.e2e.runtimemonitoring.client.Support;
import systems.zlink.e2e.runtimemonitoring.client.Support.MonitoringScenarioContext;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;

public final class MonC1ObserverIsolationScenario {
    private MonC1ObserverIsolationScenario() {
    }

    public static void run(MonitoringScenarioContext context) {
        String serviceA = context.serviceEndpoint();
        Contracts.ObserverIsolationStatus started = context.observer(serviceA, "start");
        Contracts.ObserverIsolationStatus entered = context.awaitObserver(
            serviceA,
            Contracts.ObserverIsolationStatus::slowEntered,
            "MON-C1 slow observer did not enter its callback gate");
        MonitoringScenarioContext.ensure(!entered.slowReleased(),
            "MON-C1 slow observer was released before the parallel request");

        context.shutdownServiceB("MON-C1 service-b did not stop during observer isolation");
        context.awaitRuntimeSnapshot(
            serviceA,
            snapshot -> snapshot.peers().stream().noneMatch(peer -> "READY".equals(peer.state())),
            "MON-C1 RouteMesh did not publish service-b loss");
        context.restartServiceB();
        context.waitForPort(
            context.serviceBEndpoint(), true, "MON-C1 service-b did not restart");
        context.awaitRuntimeSnapshot(
            serviceA,
            MonitoringScenarioContext::routeMeshTargetReady,
            "MON-C1 RouteMesh did not recover service-b");

        Contracts.WorkRes parallel = context.runtimeRequest(serviceA, "mon-c1-parallel");
        MonitoringScenarioContext.ensure(
            "svc-b".equals(parallel.providerRid()),
            "MON-C1 parallel Channel request did not complete on service-b: " + parallel);

        Contracts.ObserverIsolationStatus progressed = context.awaitObserver(
            serviceA,
            status -> status.normalEventCount() > started.normalEventCount()
                && status.normalLatestSequence() > 0
                && !status.slowReleased(),
            "MON-C1 normal observer did not progress while slow observer was held");
        context.observer(serviceA, "release");
        Contracts.ObserverIsolationStatus failed = context.awaitObserver(
            serviceA,
            status -> status.slowFailed() && status.slowLatestSequence() > 0,
            "MON-C1 slow observer failure was not isolated");
        MonitoringScenarioContext.ensure(
            failed.normalLatestSequence() >= progressed.normalLatestSequence(),
            "MON-C1 normal observer regressed after slow observer failure");
        Contracts.RuntimeSnapshot resynced = context.runtimeSnapshot(serviceA);
        MonitoringScenarioContext.ensure(
            resynced.sequence() >= failed.slowLatestSequence()
                && resynced.channels().stream().anyMatch(
                    channel -> Contracts.SPOT_CHANNEL.equals(channel.channelName())),
            "MON-C1 public snapshot did not retain the latest channel state");

        Contracts.WorkRes followUp = context.runtimeRequest(serviceA, "mon-c1-recovery");
        MonitoringScenarioContext.ensure(
            "svc-b".equals(followUp.providerRid()),
            "MON-C1 messaging did not recover after observer failure: " + followUp);
        context.stopRestartedServiceB();
        System.out.println("scenario MON-C1 passed; normal-events="
            + failed.normalEventCount() + ", sequence=" + resynced.sequence());
    }
}
