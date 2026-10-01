package systems.zlink.framework.perf.servers.spot;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfDriveReply;
import systems.zlink.framework.perf.PerfDriveRequest;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.PerfTriggerRequest;
import systems.zlink.framework.perf.ResetRequest;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry;
import systems.zlink.framework.spots.ZLinkSpotOutbound;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

class S2sRequestWindowGateTest {
    private static final String CELL = "s2s-spot-to-channel-request-echo/1024/test";

    private static RoleConfig config() {
        return new RoleConfig("test", CELL, "f".repeat(64), "java", "spot", 0,
                "s2s-spot-to-channel-request-echo", "request", "ordinary", "routemesh", "ch", "mesh",
                Map.of(), null, "", "", true, "Spot", true, null, List.of("spot-0"), List.of(), 1, null,
                null, "SpotWide", new RoleConfig.Workload(1024, .04, .01, 1, null, 1, 1, null,
                        1000, 1000, 2000, 1000, 5000, 1000), null, Map.of());
    }

    @Test
    void handlerReturnsValidatedEchoEvenAfterMeasurementEnd() throws Exception {
        RoleConfig config = config();
        Measurement measurement = new Measurement(config, true);
        ScenarioMetrics metrics = new ScenarioMetrics(measurement).latency("driverLatencyMs", "driver.latency");
        S2sSpotToChannelRequestEchoScenario.S2sRemoteRequestDriveHandler handler =
                new S2sSpotToChannelRequestEchoScenario.S2sRemoteRequestDriveHandler(measurement, metrics, config);
        AtomicReference<PerfEchoRequest> submittedRequest = new AtomicReference<>();
        ZLinkRequestCall call = proxy(ZLinkRequestCall.class, (instance, method, args) -> switch (method.getName()) {
            case "timeout" -> instance;
            case "submit", "yield" -> {
                CompletableFuture<PerfEchoReply> reply = new CompletableFuture<>();
                CompletableFuture.delayedExecutor(120, TimeUnit.MILLISECONDS).execute(() ->
                        reply.complete(PayloadPattern.reply(submittedRequest.get(), systems.zlink.framework.perf.PerfClock.now())));
                yield reply;
            }
            default -> null;
        });
        ZLinkSpotOutbound outbound = proxy(ZLinkSpotOutbound.class, (instance, method, args) -> {
            if (method.getName().equals("requestToChannel")) {
                submittedRequest.set((PerfEchoRequest) args[1]);
                return call;
            }
            return null;
        });
        ZLinkSpotHandlerRegistry registry = proxy(ZLinkSpotHandlerRegistry.class, (instance, method, args) -> null);
        ZLinkSpotContext context = proxy(ZLinkSpotContext.class, (instance, method, args) -> switch (method.getName()) {
            case "handlers" -> registry;
            case "outbound" -> outbound;
            case "spotId" -> "spot-0";
            default -> null;
        });
        S2sSpotToChannelRequestEchoScenario.S2sRemoteRequestSpot spot =
                new S2sSpotToChannelRequestEchoScenario.S2sRemoteRequestSpot(context);
        AtomicReference<CompletableFuture<PerfDriveReply>> operation = new AtomicReference<>();

        assertTrue(measurement.start(trigger("warmup", "0"), null).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(new ResetRequest("test", CELL, "1"), null).ok());
        assertTrue(measurement.start(trigger("measured", "1"), () -> {
            PerfEchoRequest echo = measurement.request(0, 1, false);
            operation.set(handler.handle(spot, new PerfDriveRequest(echo)).toCompletableFuture());
            return operation.get().thenAccept(ignored -> {});
        }).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        PerfDriveReply driven = operation.get().get(5, TimeUnit.SECONDS);

        assertTrue(driven.started());
        assertNotNull(driven.echo());
        var snapshot = measurement.snapshot(null);
        assertEquals("1", snapshot.metrics.get("messages.inflightAtEnd"));
        assertEquals("0", ((Map<?, ?>) snapshot.histograms.get("driverLatencyMs")).get("count"));
    }

    private static PerfTriggerRequest trigger(String phase, String resetSeq) {
        return new PerfTriggerRequest("test", CELL, resetSeq, phase);
    }

    private static <T> T proxy(Class<T> type, InvocationHandler handler) {
        return type.cast(Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type}, handler));
    }
}
