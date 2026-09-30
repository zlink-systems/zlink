package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
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

    private void verifyTerminal(boolean request, boolean instance, boolean stale, boolean missing) {
        var terminal =
                ZLinkFrameworkErrorOrigin.framework(
                        stale
                                ? ZLinkFrameworkErrorKind.NOT_FOUND
                                : ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "first operation failed");
        var probe = new SpotTerminalProbe(terminal, missing);
        var backend =
                (ZLinkBackendSpot)
                        Proxy.newProxyInstance(
                                ZLinkBackendSpot.class.getClassLoader(),
                                new Class<?>[] {ZLinkBackendSpot.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("sendToSpot")
                                            || method.getName().equals("requestToSpot")) {
                                        probe.submissions++;
                                        return CompletableFuture.failedFuture(terminal);
                                    }
                                    if (method.getName().equals("admissionTimeout")) {
                                        return Duration.ofSeconds(1);
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
                        Runnable::run,
                        new ZLinkMessageFlowTracer(
                                new ZLinkDispatchOptionsRegistration(), null, Runnable::run));
        var outbound =
                new DefaultSpotOutbound(
                        backend,
                        "mesh",
                        null,
                        serializer,
                        ignored -> ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                        null,
                        direct,
                        null,
                        null,
                        false,
                        Duration.ofSeconds(1),
                        () -> probe,
                        probe);
        java.util.concurrent.CompletionStage<?> result;
        if (request) {
            var call = outbound.requestToSpot("spot", new Packet("request"));
            result = (instance ? call.instanceSpot() : call).submit(String.class);
        } else {
            var call = outbound.sendToSpot("spot", new Packet("send"));
            result = (instance ? call.instanceSpot() : call).submit();
        }
        var failure =
                assertThrows(CompletionException.class, () -> result.toCompletableFuture().join());
        assertSame(terminal, failure.getCause());
        assertEquals(missing ? 0 : 1, probe.submissions);
        assertEquals(1, probe.resolves);
        assertEquals(stale && !missing ? 1 : 0, probe.invalidations);
        assertEquals(
                missing ? 1 : 0,
                probe.activations,
                "failure must not create an activation operation");
    }

    private record Packet(String value) {}
}
