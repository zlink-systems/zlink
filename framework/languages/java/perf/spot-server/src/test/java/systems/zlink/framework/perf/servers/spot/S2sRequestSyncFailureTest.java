package systems.zlink.framework.perf.servers.spot;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PerfDriveRequest;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.PerfTriggerRequest;
import systems.zlink.framework.perf.ResetRequest;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry;
import systems.zlink.framework.spots.ZLinkSpotOutbound;

import java.lang.reflect.Proxy;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

class S2sRequestSyncFailureTest {
    private static final String CELL = "s2s-spot-to-channel-request-echo/1024/test";

    private static RoleConfig config() {
        return new RoleConfig(
                "test",
                CELL,
                "d".repeat(64),
                "java",
                "spot",
                0,
                "s2s-spot-to-channel-request-echo",
                "request",
                "ordinary",
                "routemesh",
                "ch",
                "mesh",
                Map.of(),
                null,
                "",
                "",
                true,
                "Spot",
                true,
                null,
                List.of("spot-0"),
                List.of(),
                1,
                null,
                null,
                "SpotWide",
                new RoleConfig.Workload(1024, .15, .01, null, 1, 1, null, 2000, 1000, 5000, 1000),
                null,
                Map.of());
    }

    @Test
    void synchronousOutboundCreationFailureClosesTheStartedOperation() throws Exception {
        Measurement measurement = new Measurement(config(), true);
        ScenarioMetrics metrics =
                new ScenarioMetrics(measurement)
                        .counters("spot.applicationHandlerEntries", "spot.applicationYieldCalls");
        S2sSpotToChannelRequestEchoScenario.S2sRemoteRequestDriveHandler handler =
                new S2sSpotToChannelRequestEchoScenario.S2sRemoteRequestDriveHandler(
                        measurement, metrics, config());
        ZLinkSpotHandlerRegistry registry =
                proxy(ZLinkSpotHandlerRegistry.class, (proxy, method, args) -> null);
        ZLinkSpotOutbound outbound =
                proxy(
                        ZLinkSpotOutbound.class,
                        (proxy, method, args) -> {
                            if (method.getName().equals("requestToChannel")) {
                                throw new IllegalStateException("synchronous outbound failure");
                            }
                            return null;
                        });
        ZLinkSpotContext context =
                proxy(
                        ZLinkSpotContext.class,
                        (proxy, method, args) ->
                                switch (method.getName()) {
                                    case "handlers" -> registry;
                                    case "outbound" -> outbound;
                                    case "spotId" -> "spot-0";
                                    default -> null;
                                });
        S2sSpotToChannelRequestEchoScenario.S2sRemoteRequestSpot spot =
                new S2sSpotToChannelRequestEchoScenario.S2sRemoteRequestSpot(context);

        assertTrue(measurement.start(trigger("warmup", "0"), null).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(new ResetRequest("test", CELL, "1"), null).ok());
        assertTrue(
                measurement
                        .start(
                                trigger("measured", "1"),
                                () -> {
                                    PerfEchoRequest echo = measurement.request(0, 1, false);
                                    try {
                                        return handler.handle(spot, new PerfDriveRequest(echo))
                                                .thenApply(ignored -> null);
                                    } catch (IllegalStateException expected) {
                                        return CompletableFuture.completedFuture(null);
                                    }
                                })
                        .accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);

        var snapshot = measurement.snapshot(null);
        assertEquals("1", snapshot.metrics.get("messages.sent"));
        assertEquals("1", snapshot.metrics.get("messages.failed"));
        assertEquals("0", snapshot.metrics.get("messages.inflightAtEnd"));
    }

    private static PerfTriggerRequest trigger(String phase, String resetSeq) {
        return new PerfTriggerRequest("test", CELL, resetSeq, phase);
    }

    private static <T> T proxy(Class<T> type, java.lang.reflect.InvocationHandler handler) {
        return type.cast(
                Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type}, handler));
    }
}
