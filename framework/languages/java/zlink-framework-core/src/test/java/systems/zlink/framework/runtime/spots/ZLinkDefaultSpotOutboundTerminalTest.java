package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.spots.SpotTerminalProbe;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;

final class ZLinkDefaultSpotOutboundTerminalTest {
    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void sendPreservesFirstTerminal(boolean instance, boolean stale) {
        verifyTerminal(false, instance, stale, false);
    }

    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void requestPreservesFirstTerminal(boolean instance, boolean stale) {
        verifyTerminal(true, instance, stale, false);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void missingOwnerActivatesBeforeFirstSubmission(boolean request) {
        verifyTerminal(request, true, true, true);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void routedLocalSubmissionUsesOneCallGate(boolean request) {
        verifyTerminal(request, false, false, false, true);
    }

    private void verifyTerminal(boolean request, boolean instance, boolean stale, boolean missing) {
        verifyTerminal(request, instance, stale, missing, false);
    }

    private void verifyTerminal(
            boolean request, boolean instance, boolean stale, boolean missing, boolean routedPath) {
        var terminal =
                stale
                        ? ZLinkFrameworkErrorOrigin.framework(
                                ZLinkFrameworkErrorKind.NOT_FOUND, "first operation failed")
                        : new systems.zlink.framework.errors.ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE, "application failure");
        var probe = new SpotTerminalProbe(terminal, missing);
        try (var operations =
                new systems.zlink.framework.runtime.internal.service
                        .ZLinkServiceOperationRegistry()) {
            var backend =
                    (ZLinkBackendSpot)
                            Proxy.newProxyInstance(
                                    ZLinkBackendSpot.class.getClassLoader(),
                                    new Class<?>[] {ZLinkBackendSpot.class},
                                    (proxy, method, arguments) -> {
                                        if (method.getName().equals("sendToSpot")) {
                                            probe.submissions++;
                                            return CompletableFuture.failedFuture(terminal);
                                        }
                                        if (method.getName().equals("requestToSpot")) {
                                            probe.submissions++;
                                            return operations.submit(
                                                    systems.zlink.framework.runtime.internal.service
                                                            .ZLinkServiceOperationIds.next(),
                                                    java.util.Arrays.stream(arguments)
                                                            .filter(Duration.class::isInstance)
                                                            .map(Duration.class::cast)
                                                            .findFirst()
                                                            .orElseThrow(),
                                                    () -> CompletableFuture.failedFuture(terminal),
                                                    systems.zlink.framework.runtime.internal.backend
                                                                    .ZLinkBackendReceived
                                                            ::close);
                                        }
                                        if (method.getReturnType() == void.class) {
                                            return null;
                                        }
                                        throw new AssertionError(
                                                "unexpected backend call: " + method.getName());
                                    });
            var serializer = new ZLinkJsonMessageSerializer();
            var direct =
                    new ZLinkSpotDirectOutbound(
                            new ZLinkSpotRouteMessages(serializer),
                            new ZLinkMessageFlowTracer(
                                    new ZLinkDispatchOptionsRegistration(),
                                    systems.zlink.framework.runtime.internal.handlers
                                            .ZLinkHandlerActivator.reflection(),
                                    Runnable::run));
            var admissions = new java.util.concurrent.atomic.AtomicInteger();
            direct.setCallAdmission(admissions::incrementAndGet);
            var routerNode =
                    (systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    systems.zlink.framework.runtime.internal.backend
                                            .ZLinkInternalSpotNode.class
                                            .getClassLoader(),
                                    new Class<?>[] {
                                        systems.zlink.framework.runtime.internal.backend
                                                .ZLinkInternalSpotNode.class
                                    },
                                    (proxy, method, arguments) -> {
                                        if (method.getName().equals("entrySpot")) return backend;
                                        throw new AssertionError(
                                                "unexpected node call: " + method.getName());
                                    });
            var routed =
                    new ZLinkSpotRoutedOutbound(
                            null,
                            new ZLinkSpotRouteMessages(serializer),
                            direct,
                            new ZLinkMessageFlowTracer(
                                    new ZLinkDispatchOptionsRegistration(),
                                    systems.zlink.framework.runtime.internal.handlers
                                            .ZLinkHandlerActivator.reflection(),
                                    Runnable::run),
                            ignored -> routerNode);
            var outbound =
                    new DefaultSpotOutbound(
                            backend,
                            "mesh",
                            null,
                            serializer,
                            ignored -> ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                            routed,
                            direct,
                            null,
                            null,
                            routedPath,
                            Duration.ofSeconds(1),
                            () -> probe,
                            probe);
            probe.readySend =
                    payload ->
                            backend.sendToSpot(
                                            probe.address.targetNodeRid(),
                                            probe.address.spotId(),
                                            probe.address.spotGeneration(),
                                            payload)
                                    .whenComplete(
                                            (ignored, failure) ->
                                                    ZLinkFrameworkMultipartView.closeAll(payload));
            probe.readyRequest =
                    (payload, timeout) ->
                            backend.requestToSpot(
                                            probe.address.targetNodeRid(),
                                            probe.address.spotId(),
                                            probe.address.spotGeneration(),
                                            null,
                                            java.util.List.of(payload),
                                            timeout,
                                            null)
                                    .thenApply(received -> received.parts())
                                    .whenComplete((ignored, failure) -> payload.close());
            java.util.concurrent.CompletionStage<?> result;
            java.util.function.Supplier<java.util.concurrent.CompletionStage<?>> repeat;
            if (request) {
                var call = outbound.requestToSpot("spot", new Packet("request"));
                var selected = instance ? call.instanceSpot() : call;
                repeat = () -> selected.submit(String.class);
                result = repeat.get();
            } else {
                var call = outbound.sendToSpot("spot", new Packet("send"));
                var selected = instance ? call.instanceSpot() : call;
                repeat = selected::submit;
                result = repeat.get();
            }
            var failure =
                    assertThrows(
                            CompletionException.class, () -> result.toCompletableFuture().join());
            assertSame(terminal, failure.getCause());
            var duplicate =
                    assertThrows(
                            CompletionException.class,
                            () -> repeat.get().toCompletableFuture().join());
            assertEquals(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    assertInstanceOf(
                                    systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                    duplicate.getCause())
                            .kind());
            assertEquals(1, admissions.get());
            assertEquals(missing ? 0 : 1, probe.submissions);
            assertEquals(1, probe.resolves);
            if (instance && !missing) {
                assertEquals(
                        0,
                        probe.coordinatorCalls,
                        "Instance Ready route must bypass the coordinator Store read");
            }
            assertEquals(stale && (request || !missing) ? 1 : 0, probe.invalidations);
            assertEquals(
                    missing ? 1 : 0,
                    probe.activations,
                    "failure must not create an activation operation");
        }
    }

    private record Packet(String value) {}
}
