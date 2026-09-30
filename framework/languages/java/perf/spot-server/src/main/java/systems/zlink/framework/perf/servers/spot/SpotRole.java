package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.monitoring.ZLinkPlacementSnapshot;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotManager;

import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;

// What every Spot role of §10.3-§10.8 shares: an automatic RouteMesh Object Server hosting this cell's User Spots as
// Actor-less SpotWide Spots (§10), and their creation through the public manager as setup. No call that a scenario measures
// lives here; each scenario file shows its own requests, sends and Yield/worker calls.
public final class SpotRole {
    private SpotRole() {}

    public static final String SPOT_TYPE = "perf-spot";

    /** The public create results of this cell (§16.1 objectsReady): the manager result and the mesh placement count. */
    public record SpotObjects(boolean ready, List<Object> evidence) {}

    /** callsChannel: the Spot handler calls this cell's ChannelName (§10.5, §10.6), so the node registers a Channel client. */
    public static <T extends PerfSpot> ServerApplication application(RoleConfig config, Class<T> spot, boolean callsChannel,
            Consumer<ZLinkFrameworkOptions> more) {
        ServerApplication app = ServerApplication.create(config).configure(options -> {
            if (more != null) {
                more.accept(options);
            }
            ZLinkMeshNodeBuilder mesh = ServerApplication.routeMesh(options, config, "perf-spot");
            if (callsChannel) {
                mesh.channelName(config.channelName()).client();
            }
            mesh.objects().server().addSpotFactory(SPOT_TYPE, spot, factory -> factory
                    .executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE)
                    .stableTypeLimit(config.spotIds().size())
                    .disableRelocation());
        });
        return app.bean(ObjectsReadiness.class, () -> new ObjectsReadiness(false, "This cell has not created its User Spots yet."));
    }

    /**
     * Setup: every SpotId of the cell is created through the public manager. Not part of any measured latency. A role that
     * probes reports objectsReady only after its probes, so warmup never overlaps a probe. Completes with null when a create
     * failed (the failure is recorded).
     */
    public static CompletionStage<SpotObjects> createSpots(RoleConfig config, ZLinkSpotManager manager,
            ZLinkRouteMeshRuntime mesh, Measurement measurement) {
        List<Object> created = new ArrayList<>();
        CompletableFuture<Void> chain = CompletableFuture.completedFuture(null);
        for (String spotId : config.spotIds()) {
            chain = chain.thenCompose(ignored -> manager.getOrCreate(spotId, SPOT_TYPE)
                    .timeout(Duration.ofMillis(config.workload().setupTimeoutMs()))
                    .submit()
                    .thenAccept(result -> {
                        if (result.state() == ZLinkSpotCreateState.REJECTED) {
                            throw new IllegalStateException("Spot " + spotId + " was rejected.");
                        }
                        Map<String, Object> observed = new LinkedHashMap<>();
                        observed.put("spotId", spotId);
                        observed.put("state", result.state().toString());
                        observed.put("meshName", result.spot().meshName());
                        created.add(observed);
                    }));
        }
        return chain.thenApply(ignored -> {
            ZLinkPlacementSnapshot placement = mesh.snapshot(config.meshName()).placement();
            Map<String, Object> observed = new LinkedHashMap<>();
            observed.put("isAvailable", placement.isAvailable());
            observed.put("activeSpotCount", placement.activeSpotCount());
            observed.put("expectedSpots", config.spotIds().size());
            return new SpotObjects(placement.isAvailable() && placement.activeSpotCount() >= config.spotIds().size(), List.of(
                    Evidence.of("spotCreate", "ZLinkSpotManager.getOrCreate.submit", created),
                    Evidence.of("spotPlacement", "ZLinkRouteMeshRuntime.snapshot.placement", observed)));
        }).exceptionally(error -> {
            measurement.recordDiagnostic(error);
            return null;
        });
    }

    public static void publish(ObjectsReadiness readiness, SpotObjects objects) {
        readiness.set(objects.ready(), "The Object Server has not activated every User Spot of this cell.", objects.evidence());
    }
}
