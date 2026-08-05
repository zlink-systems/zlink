package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
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
