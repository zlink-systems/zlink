package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

final class ZLinkSpotDirectPublishTest {
    @Test
    void publicSpotPublishCompletesWithoutTargetResult() {
        AtomicInteger publishCalls = new AtomicInteger();
        ZLinkBackendSpot spot = (ZLinkBackendSpot) Proxy.newProxyInstance(
            ZLinkBackendSpot.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendSpot.class},
            (ignored, method, arguments) -> switch (method.getName()) {
                case "name", "routingId" -> "source";
                case "admissionSource" -> ignored;
                case "admissionTimeout" -> Duration.ofSeconds(1);
                case "admissionPendingCapacity" -> 8;
                case "publish" -> {
                    publishCalls.incrementAndGet();
                    yield true;
                }
                case "close", "setAdmissionReadyHandler",
                    "setAdmissionShutdownHandler" -> null;
                default -> defaultValue(method.getReturnType());
            });
        var serializer = new ZLinkStringMessageSerializer();
        var options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(
            systems.zlink.framework.configuration
                .ZLinkMessageFlowLogMode.OFF);
        var outbound = new ZLinkSpotDirectOutbound(
            new ZLinkSpotRouteMessages(serializer),
            Runnable::run,
            new ZLinkMessageFlowTracer(
                options,
                ZLinkHandlerActivator.reflection(),
                Runnable::run));

        try (Message payload = Message.from(
                "payload".getBytes(StandardCharsets.UTF_8))) {
            var result = outbound.publish(
                    spot,
                    "events",
                    "orders",
                    payload,
                    Optional.empty())
                .submit()
                .toCompletableFuture()
                .join();

            assertNull(result);
            assertEquals(1, publishCalls.get());
        }
    }

    @Test
    void publicSpotSendCannotCompletePendingAdmission() {
        CompletableFuture<Void> admission = new CompletableFuture<>();
        ZLinkOneWayCalls oneWayCalls = new ZLinkOneWayCalls(
            (backend, key) -> (submission, cleanup) -> admission);
        ZLinkBackendSpot spot = (ZLinkBackendSpot) Proxy.newProxyInstance(
            ZLinkBackendSpot.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendSpot.class},
            (ignored, method, arguments) -> defaultValue(method.getReturnType()));
        var options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(
            systems.zlink.framework.configuration
                .ZLinkMessageFlowLogMode.OFF);
        var outbound = new ZLinkSpotDirectOutbound(
            new ZLinkSpotRouteMessages(new ZLinkStringMessageSerializer()),
            Runnable::run,
            new ZLinkMessageFlowTracer(
                options,
                ZLinkHandlerActivator.reflection(),
                Runnable::run),
            oneWayCalls);

        try (Message payload = Message.from(
                "payload".getBytes(StandardCharsets.UTF_8))) {
            var publicStage = outbound.send(
                    spot,
                    systems.zlink.contracts.core.RoutingId.from("node"),
                    "spot",
                    1L,
                    payload,
                    Optional.empty())
                .submit()
                .toCompletableFuture();

            assertNotSame(admission, publicStage);
            assertTrue(publicStage.complete(null));
            assertFalse(admission.isDone());

            admission.complete(null);
            assertTrue(admission.isDone());
        }
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) {
            return false;
        }
        if (type == char.class) {
            return '\0';
        }
        return 0;
    }
}
