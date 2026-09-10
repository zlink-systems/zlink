package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

final class ZLinkChannelCallRuntimeTest {
    @Test
    void closedRuntimeRejectsNodeChannelAndSpotBeforeBackendSelection() {
        try (var scheduler = Executors.newSingleThreadScheduledExecutor()) {
            var calls = new ZLinkChannelCallRuntime(null, scheduler, null, null,
                (channel, target, spot, generation, authority, lease, parts, timeout,
                 owner, identity) -> {
                    fail("closed source must not enter Spot selection");
                    return null;
                });
            ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
                getClass().getClassLoader(), new Class<?>[] {ZLinkInternalSpotNode.class},
                (proxy, method, args) -> {
                    fail("closed source must not enter backend selection");
                    return null;
                });
            calls.beginClose();
            var target = RoutingId.from("closed-target");
            var identity = ZLinkServiceOperationIds.next();
            var timeout = Duration.ofSeconds(1);
            var requests = List.of(
                calls.requestNode(identity, node, target, null, List.of(), timeout),
                calls.requestChannel(identity, node, "missing", null, List.of(), timeout),
                calls.requestToSpot("missing", target, "spot", 1, 1, 1,
                    List.of(), timeout, identity));
            for (var request : requests) {
                CompletionException failure = assertThrows(CompletionException.class,
                    () -> request.toCompletableFuture().join());
                assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            }
        }
    }

    @Test
    void disabledFlowCaptureReturnsTheBindingStageWithoutADependent() {
        var source = new CompletableFuture<String>();
        var ambient = ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
        try (var ignored = ZLinkFlowContext.enter(ambient)) {
            try (var suppressed = ZLinkFlowContext.enterCurrentOrCreate(
                    ZLinkFlowOrigin.APPLICATION, false)) {
                assertNull(ZLinkFlowContext.current());
                assertSame(source, ZLinkChannelCallRuntime.preserveCurrentFlow(source));
                assertEquals(0, source.getNumberOfDependents());
            }
            assertSame(ambient, ZLinkFlowContext.current());
        }
    }

    @Test
    void enabledFlowCaptureRestoresTheFlowForCompletionAndThenRestoresTheThread() {
        var source = new CompletableFuture<String>();
        var captured = ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
        CompletableFuture<ZLinkFlowContext.State> observed;
        try (var ignored = ZLinkFlowContext.enter(captured)) {
            observed = ZLinkChannelCallRuntime.preserveCurrentFlow(source)
                .thenApply(value -> ZLinkFlowContext.current()).toCompletableFuture();
        }
        assertNull(ZLinkFlowContext.current());
        source.complete("reply");
        assertSame(captured, observed.join());
        assertNull(ZLinkFlowContext.current());
    }

    @Test
    void requestEnvelopeUsesTheOperationIdentityAndBorrowsTheEncodedPayload() {
        var identity = ZLinkServiceOperationIds.next();
        try (Message payload = Message.from("payload");
             var ignored = ZLinkFlowContext.suppress()) {
            var parts = ZLinkChannelCallRuntime.envelopeParts(
                ZLinkChannelEnvelope.KIND_REQUEST, "orders", Optional.of("request"),
                payload, "application/json", Map.of(), identity);
            try {
                assertSame(payload, parts.getLast(), "submission must not copy native payload");
                assertEquals(ZLinkServiceOperationIds.correlationId(identity),
                    ZLinkChannelEnvelope.decodeHeader(parts.getFirst(), false).correlationId());
            } finally {
                parts.getFirst().close();
            }
            assertFalse(payload.empty());
        }
    }

    @Test
    void closeSettlesTheSoleOperationAndClosesALateReplyWithoutResubmission()
        throws Exception {
        try (var scheduler = Executors.newSingleThreadScheduledExecutor()) {
            var calls = new ZLinkChannelCallRuntime(null, scheduler, null, null, null);
            var binding = new CompletableFuture<ZLinkBackendReceived>();
            AtomicInteger attempts = new AtomicInteger();
            Duration timeout = Duration.ofSeconds(2);
            ZLinkBackendDealerSocket dealer = (ZLinkBackendDealerSocket) Proxy.newProxyInstance(
                getClass().getClassLoader(), new Class<?>[] {ZLinkBackendDealerSocket.class},
                (proxy, method, args) -> {
                    assertEquals("request", method.getName());
                    assertSame(timeout, args[1]);
                    attempts.incrementAndGet();
                    return binding;
                });
            try {
                var operation = calls.requestClient(dealer, List.of(), timeout)
                    .toCompletableFuture();
                assertFalse(operation.isDone());
                calls.beginClose();
                CompletionException completion = assertThrows(CompletionException.class,
                    () -> operation.orTimeout(1, TimeUnit.SECONDS).join());
                assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(ZLinkFrameworkException.class, completion.getCause()).kind());
                Message late = Message.from("late");
                binding.complete(new ZLinkBackendReceived(ZLinkBackendRequestResult.OK,
                    Optional.empty(), Optional.empty(), Optional.empty(), List.of(late)));
                assertTrue(late.empty(), "a losing late reply must release native ownership");
                assertEquals(1, attempts.get());

                var rejected = calls.requestClient(dealer, List.of(), timeout).toCompletableFuture();
                CompletionException closed = assertThrows(CompletionException.class, rejected::join);
                assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(ZLinkFrameworkException.class, closed.getCause()).kind());
                assertEquals(1, attempts.get(), "close must reject before starting another binding operation");
            } finally {
                calls.beginClose();
            }
        }
    }
}
