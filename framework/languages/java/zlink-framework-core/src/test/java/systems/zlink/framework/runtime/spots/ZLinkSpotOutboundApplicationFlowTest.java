package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

/**
 * R1 value-passing contract for spot publish/direct-outbound APPLICATION
 * flow: the flow state is captured as a value and consumed only at encode
 * time, while the stage a publish turn returns stays the bare admission
 * future (no scope wrapper, no extra completion hop).
 */
final class ZLinkSpotOutboundApplicationFlowTest {
    @Test
    void directSendStartsApplicationFlowOutsideCallbacks() {
        AtomicReference<List<Message>> sentParts = new AtomicReference<>();
        ZLinkBackendSpot spot = spotCapturing("sendToSpot", sentParts);
        var outbound = outbound(ZLinkMessageFlowLogMode.NORMAL);

        try (Message payload = message("payload")) {
            outbound.send(spot, RoutingId.from("target"), "spot-1", 1L,
                    payload, Optional.of("Move"))
                .submit().toCompletableFuture().join();
        }

        ZLinkFlowContext.State flow = ZLinkSpotFlowFrame.decode(sentParts.get());
        assertNotNull(flow);
        assertEquals(ZLinkFlowOrigin.APPLICATION, flow.origin());
        assertTrue(ZLinkFlowContext.isValidFlowId(flow.flowId()));
    }

    @Test
    void directSendPreservesAmbientCallbackFlowAsValue() {
        AtomicReference<List<Message>> sentParts = new AtomicReference<>();
        ZLinkBackendSpot spot = spotCapturing("sendToSpot", sentParts);
        var outbound = outbound(ZLinkMessageFlowLogMode.NORMAL);
        ZLinkFlowContext.State ambient =
            ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);

        try (Message payload = message("payload");
             ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(ambient)) {
            outbound.send(spot, RoutingId.from("target"), "spot-1", 1L,
                    payload, Optional.of("Move"))
                .submit().toCompletableFuture().join();
        }

        ZLinkFlowContext.State flow = ZLinkSpotFlowFrame.decode(sentParts.get());
        assertNotNull(flow);
        assertEquals(ambient.flowId(), flow.flowId());
        assertEquals(ZLinkFlowOrigin.INBOUND, flow.origin());
    }

    @Test
    void offCapturesNothingAndAttachesNoFlowFrame() {
        AtomicReference<List<Message>> sentParts = new AtomicReference<>();
        ZLinkBackendSpot spot = spotCapturing("sendToSpot", sentParts);
        var outbound = outbound(ZLinkMessageFlowLogMode.OFF);

        try (Message payload = message("payload")) {
            outbound.send(spot, RoutingId.from("target"), "spot-1", 1L,
                    payload, Optional.of("Move"))
                .submit().toCompletableFuture().join();
        }

        assertNull(ZLinkSpotFlowFrame.decode(sentParts.get()));
        assertEquals(2, sentParts.get().size());
    }

    @Test
    void publishTurnReturnsBareAdmissionEvenWithTracingOn() {
        CompletableFuture<Void> admission = new CompletableFuture<>();
        AtomicReference<List<Message>> publishedParts = new AtomicReference<>();
        ZLinkBackendSpot spot = (ZLinkBackendSpot) Proxy.newProxyInstance(
            ZLinkBackendSpot.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendSpot.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("publishAsync")) {
                    publishedParts.set(copyParts(arguments));
                    return admission;
                }
                return defaultValue(method.getReturnType());
            });
        var outbound = outbound(ZLinkMessageFlowLogMode.NORMAL);

        try (Message payload = message("payload")) {
            var publicStage = outbound.publish(
                    spot, "events", "orders", payload, Optional.of("Move"))
                .submit()
                .toCompletableFuture();

            //  The flow frame is attached at encode time by value passing…
            ZLinkFlowContext.State flow =
                ZLinkSpotFlowFrame.decode(publishedParts.get());
            assertNotNull(flow);
            assertEquals(ZLinkFlowOrigin.APPLICATION, flow.origin());

            //  …while the public stage stays decoupled from the bare
            //  admission future: no wrapper chain was added to it.
            assertNotSame(admission, publicStage);
            assertTrue(publicStage.complete(null));
            assertFalse(admission.isDone());
            admission.complete(null);
        }
    }

    private static ZLinkSpotDirectOutbound outbound(ZLinkMessageFlowLogMode mode) {
        var options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(mode);
        return new ZLinkSpotDirectOutbound(
            new ZLinkSpotRouteMessages(new ZLinkStringMessageSerializer()),
            Runnable::run,
            new ZLinkMessageFlowTracer(
                options,
                ZLinkHandlerActivator.reflection(),
                Runnable::run));
    }

    private static ZLinkBackendSpot spotCapturing(
        String methodName,
        AtomicReference<List<Message>> parts) {
        return (ZLinkBackendSpot) Proxy.newProxyInstance(
            ZLinkBackendSpot.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendSpot.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals(methodName)) {
                    parts.set(copyParts(arguments));
                    return CompletableFuture.completedFuture(null);
                }
                if (method.getName().equals("admissionTimeout")) {
                    return Duration.ofSeconds(1);
                }
                return defaultValue(method.getReturnType());
            });
    }

    private static List<Message> copyParts(Object[] arguments) {
        for (Object argument : arguments) {
            if (argument instanceof List<?> list
                && !list.isEmpty()
                && list.get(0) instanceof Message) {
                @SuppressWarnings("unchecked")
                List<Message> parts = (List<Message>) list;
                return parts.stream().map(part ->
                    Message.from(part.toByteArray())).toList();
            }
        }
        return List.of();
    }

    private static Message message(String value) {
        return Message.from(value.getBytes(StandardCharsets.UTF_8));
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
