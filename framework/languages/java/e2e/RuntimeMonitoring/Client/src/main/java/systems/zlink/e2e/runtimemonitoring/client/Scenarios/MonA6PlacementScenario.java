package Scenarios;

import systems.zlink.e2e.runtimemonitoring.client.Scenarios;
import systems.zlink.e2e.runtimemonitoring.client.Support;
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
            snapshot -> MonitoringScenarioContext.routeMeshTargetReady(snapshot)
                && snapshot.placementAvailable(),
            "MON-A6 RouteMesh placement did not become ready");
        int baselineSpots = baseline.activeSpotCount();
        MonitoringScenarioContext.ensure(
            baselineSpots == 0,
            "MON-A6 fixture bootstrap Spot was not disabled: baseline=" + baselineSpots);

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
            secondSpot.status() == 200 && secondSpot.body().contains("\"accepted\":true"),
            "MON-A6 second Spot was rejected before reaching the configured limit: "
                + secondSpot.status() + ": " + secondSpot.body());
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeSpotCount() == baselineSpots + 2,
            "MON-A6 placement snapshot did not count both Spots at the limit");

        RawHttpResponse spotOverflow = raw(
            baseUrl,
            "/runtime/placement/spot/create?id=mon-a6-placement-overflow-2");
        MonitoringScenarioContext.ensure(
            spotOverflow.status() == 409 && spotOverflow.body().contains("CAPACITY_EXCEEDED"),
            "MON-A6 Spot over limit did not expose CAPACITY_EXCEEDED: "
                + spotOverflow.status() + ": " + spotOverflow.body());

        // Config-7 defines placement as unavailable only after both the Actor
        // and Spot capacity limits are full.
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

        Contracts.RuntimeSnapshot unavailable = context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> !snapshot.placementAvailable()
                && "CAPACITY_EXCEEDED".equals(snapshot.placementUnavailableReason()),
            "MON-A6 placement did not expose the capacity reason");

        RawHttpResponse closedAtLimit = raw(
            baseUrl,
            "/runtime/placement/spot/close?id=mon-a6-placement-spot");
        MonitoringScenarioContext.ensure(
            closedAtLimit.status() == 200 && closedAtLimit.body().contains("\"accepted\":true"),
            "MON-A6 Spot close did not return public success: "
                + closedAtLimit.status() + ": " + closedAtLimit.body());
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeSpotCount() == baselineSpots + 1
                && !snapshot.placementAvailable()
                && "CAPACITY_EXCEEDED".equals(
                    snapshot.placementUnavailableReason()),
            "MON-A6 placement availability did not retain the full Actor limit");

        RawHttpResponse destroyed = raw(
            baseUrl,
            "/runtime/placement/actor/destroy?id=mon-a6-placement-actor");
        MonitoringScenarioContext.ensure(
            destroyed.status() == 200 && destroyed.body().contains("\"destroyed\":true"),
            "MON-A6 actor destroy did not return public success: "
                + destroyed.status() + ": " + destroyed.body());
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeActorCount() == 0
                && snapshot.placementAvailable(),
            "MON-A6 placement did not recover after freeing both capacities");

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

        RawHttpResponse actorReplacement = raw(
            baseUrl,
            "/runtime/placement/actor/create?id=mon-a6-placement-actor-replacement");
        MonitoringScenarioContext.ensure(
            actorReplacement.status() == 200 && actorReplacement.body().contains("\"accepted\":true"),
            "MON-A6 replacement Actor was rejected after capacity recovery: "
                + actorReplacement.status() + ": " + actorReplacement.body());
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeActorCount() == 1,
            "MON-A6 replacement Actor was not counted after capacity recovery");
        RawHttpResponse actorReplacementDestroyed = raw(
            baseUrl,
            "/runtime/placement/actor/destroy?id=mon-a6-placement-actor-replacement");
        MonitoringScenarioContext.ensure(
            actorReplacementDestroyed.status() == 200
                && actorReplacementDestroyed.body().contains("\"destroyed\":true"),
            "MON-A6 replacement Actor cleanup did not return public success: "
                + actorReplacementDestroyed.status() + ": "
                + actorReplacementDestroyed.body());
        RawHttpResponse closed = raw(
            baseUrl,
            "/runtime/placement/spot/close?id=mon-a6-placement-replacement");
        MonitoringScenarioContext.ensure(
            closed.status() == 200 && closed.body().contains("\"accepted\":true"),
            "MON-A6 replacement Spot close did not return public success: "
                + closed.status() + ": " + closed.body());

        // Keep the fixture baseline for later scenarios in an all-suite run.
        raw(baseUrl, "/runtime/placement/spot/close?id=mon-a6-placement-overflow");
        context.awaitRuntimeSnapshot(
            baseUrl,
            snapshot -> snapshot.activeSpotCount() == baselineSpots
                && snapshot.placementAvailable(),
            "MON-A6 placement did not recover after closing the created Spots");
        System.out.println("scenario MON-A6 passed; baseline=" + baselineSpots
            + ", overflow-count=" + unavailable.activeSpotCount());
    }

    private static RawHttpResponse raw(String baseUrl, String path) {
        try {
            return ZLinkHttpClient.create(baseUrl)
                // The Framework operation has its own deadline; leave HTTP response
                // time for the service to translate that terminal result.
                .timeout(Duration.ofSeconds(20))
                .post(path)
                .submitRaw()
                .toCompletableFuture()
                .join();
        } catch (RuntimeException error) {
            throw new IllegalStateException(
                "MON-A6 HTTP request failed: " + path,
                error);
        }
    }
}
