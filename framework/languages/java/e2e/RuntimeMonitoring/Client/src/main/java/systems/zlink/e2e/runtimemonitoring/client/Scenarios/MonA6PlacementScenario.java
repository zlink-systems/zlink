package systems.zlink.e2e.runtimemonitoring.client.Scenarios;

import java.time.Duration;
import systems.zlink.e2e.runtimemonitoring.client.Support.MonitoringScenarioContext;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class MonA6PlacementScenario {
    private MonA6PlacementScenario() {
    }

    public static void run(MonitoringScenarioContext context) {
        String baseUrl = context.serviceEndpoint();
        Contracts.RuntimeSnapshot baseline = context.awaitRuntimeSnapshot(
            baseUrl,
            MonitoringScenarioContext::routeMeshTargetReady,
            "MON-A6 RouteMesh did not become ready");
        int baselineSpots = baseline.activeSpotCount();

        RawHttpResponse firstSpot = raw(
            baseUrl,
            "/runtime/placement/spot/create?id=mon-a6-placement-spot");
        MonitoringScenarioContext.ensure(
            firstSpot.status() == 200 && firstSpot.body().contains("\"accepted\":true"),
            "MON-A6 first Spot creation was rejected: "
                + firstSpot.status() + ": " + firstSpot.body());
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeSpotCount() == baselineSpots + 1,
            "MON-A6 placement snapshot did not count the created Spot");

        RawHttpResponse secondSpot = raw(
            baseUrl,
            "/runtime/placement/spot/create?id=mon-a6-placement-overflow");
        MonitoringScenarioContext.ensure(
            secondSpot.status() == 409 && secondSpot.body().contains("CAPACITY_EXCEEDED"),
            "MON-A6 second Spot did not expose CAPACITY_EXCEEDED: "
                + secondSpot.status() + ": " + secondSpot.body());
        Contracts.RuntimeSnapshot unavailable = context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> !snapshot.placementAvailable()
                && "CAPACITY_EXCEEDED".equals(snapshot.placementUnavailableReason()),
            "MON-A6 placement did not expose the capacity reason");

        RawHttpResponse actor = raw(
            baseUrl,
            "/runtime/placement/actor/create?id=mon-a6-placement-actor");
        MonitoringScenarioContext.ensure(
            actor.status() == 200 && actor.body().contains("\"accepted\":true"),
            "MON-A6 actor creation was rejected: "
                + actor.status() + ": " + actor.body());
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeActorCount() == 1,
            "MON-A6 placement snapshot did not count the created actor");
        RawHttpResponse actorOverflow = raw(
            baseUrl,
            "/runtime/placement/actor/create?id=mon-a6-placement-actor-overflow");
        MonitoringScenarioContext.ensure(
            actorOverflow.status() == 409 && actorOverflow.body().contains("CAPACITY_EXCEEDED"),
            "MON-A6 second actor did not expose CAPACITY_EXCEEDED: "
                + actorOverflow.status() + ": " + actorOverflow.body());

        RawHttpResponse destroyed = raw(
            baseUrl,
            "/runtime/placement/actor/destroy?id=mon-a6-placement-actor");
        MonitoringScenarioContext.ensure(
            destroyed.status() == 200 && destroyed.body().contains("\"destroyed\":true"),
            "MON-A6 actor destroy did not return public success: "
                + destroyed.status() + ": " + destroyed.body());
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeActorCount() == 0,
            "MON-A6 actor capacity was not released after destroy");

        RawHttpResponse replacement = raw(
            baseUrl,
            "/runtime/placement/spot/create?id=mon-a6-placement-replacement");
        MonitoringScenarioContext.ensure(
            replacement.status() == 200 && replacement.body().contains("\"accepted\":true"),
            "MON-A6 replacement Spot was rejected after capacity recovery: "
                + replacement.status() + ": " + replacement.body());
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeSpotCount() == baselineSpots + 2,
            "MON-A6 replacement Spot was not counted after capacity recovery");
        RawHttpResponse closed = raw(
            baseUrl,
            "/runtime/placement/spot/close?id=mon-a6-placement-spot");
        MonitoringScenarioContext.ensure(
            closed.status() == 200 && closed.body().contains("\"accepted\":true"),
            "MON-A6 Spot close did not return public success: "
                + closed.status() + ": " + closed.body());

        // Keep the fixture baseline for later scenarios in an all-suite run.
        raw(baseUrl, "/runtime/placement/spot/close?id=mon-a6-placement-replacement");
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeSpotCount() == baselineSpots
                && snapshot.placementAvailable(),
            "MON-A6 placement did not recover after closing the created Spots");
        System.out.println("scenario MON-A6 passed; baseline=" + baselineSpots
            + ", overflow-count=" + unavailable.activeSpotCount());
    }

    private static RawHttpResponse raw(String baseUrl, String path) {
        return ZLinkHttpClient.create(baseUrl)
            .timeout(Duration.ofSeconds(10))
            .post(path)
            .submitRaw()
            .toCompletableFuture()
            .join();
    }
}
