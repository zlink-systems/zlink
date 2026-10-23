package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

final class ZLinkChannelHandlerUnfilteredLifecycleTest {
    @Test
    void activationFailureClosesDependenciesCreatedBeforeTheFailure() {
        var probe = new Probe();
        var failure = new IllegalStateException("activation failed");
        probe.createFailure = failure;

        var completed = dispatch(probe, null);

        assertSame(failure, failureOf(completed));
        assertEquals(1, probe.dependenciesCreated);
        assertEquals(0, probe.handlerCalls);
        assertEquals(0, probe.destroyCalls);
        assertEquals(1, probe.closeCalls);
        assertEquals(1, probe.dependenciesClosed);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void pendingHandlerRetainsItsScopeUntilTerminalCompletion(boolean failed) {
        var probe = new Probe();
        var completed = dispatch(probe, null);

        assertEquals(1, probe.handlerCalls);
        assertFalse(completed.toCompletableFuture().isDone());
        assertEquals(0, probe.destroyCalls);
        assertEquals(0, probe.closeCalls);
        assertEquals(0, probe.dependenciesClosed);

        if (failed) {
            var failure = new IllegalStateException("handler completion failed");
            probe.completion.completeExceptionally(failure);
            assertSame(failure, failureOf(completed));
        } else {
            probe.completion.complete(null);
            completed.toCompletableFuture().join();
        }

        assertEquals(1, probe.destroyCalls);
        assertEquals(1, probe.closeCalls);
        assertEquals(1, probe.dependenciesClosed);
    }

    @ParameterizedTest
    @EnumSource(FailurePoint.class)
    void synchronousFailureStillDestroysTheHandlerAndClosesItsScope(FailurePoint point) {
        var probe = new Probe();
        var failure = new IllegalStateException("synchronous dispatch failed");
        Runnable beforeHandler = null;
        if (point == FailurePoint.HANDLER) {
            probe.handlerFailure = failure;
        } else {
            beforeHandler =
                    () -> {
                        throw failure;
                    };
        }

        var completed = dispatch(probe, beforeHandler);

        assertSame(failure, failureOf(completed));
        assertEquals(point == FailurePoint.HANDLER ? 1 : 0, probe.handlerCalls);
        assertEquals(1, probe.destroyCalls);
        assertEquals(1, probe.closeCalls);
        assertEquals(1, probe.dependenciesClosed);
    }

    @Test
    void destructionFailureRemainsPrimaryWhenScopeCleanupAlsoFails() {
        var probe = new Probe();
        var destroyFailure = new IllegalStateException("handler destruction failed");
        var closeFailure = new IllegalStateException("scope cleanup failed");
        probe.destroyFailure = destroyFailure;
        probe.closeFailure = closeFailure;
        probe.completion.complete(null);

        var completed = dispatch(probe, null);

        assertSame(destroyFailure, failureOf(completed));
        assertEquals(1, destroyFailure.getSuppressed().length);
        assertSame(closeFailure, destroyFailure.getSuppressed()[0]);
        assertEquals(1, probe.destroyCalls);
        assertEquals(1, probe.closeCalls);
        assertEquals(1, probe.dependenciesClosed);
    }

    private static CompletionStage<Void> dispatch(Probe probe, Runnable beforeHandler) {
        var invoker =
                new ZLinkChannelHandlerInvoker(
                        new ZLinkStringMessageSerializer(),
                        new ZLinkCodecRegistration(),
                        probe,
                        Runnable::run,
                        List.of());
        try (var payload = Message.from("payload")) {
            return invoker.invokeSendHandler(
                    "channel",
                    new ChannelSendHandlerRegistration(ProbeHandler.class, String.class, "packet"),
                    payload.dataBuffer(),
                    Map.of(),
                    null,
                    beforeHandler);
        }
    }

    private static Throwable failureOf(CompletionStage<?> completed) {
        return assertThrows(CompletionException.class, () -> completed.toCompletableFuture().join())
                .getCause();
    }

    private enum FailurePoint {
        HANDLER,
        BEFORE_HANDLER
    }

    private static final class Probe implements ZLinkHandlerActivator {
        private final CompletableFuture<Void> completion = new CompletableFuture<>();
        private final ProbeHandler handler = new ProbeHandler(this);
        private RuntimeException createFailure;
        private RuntimeException handlerFailure;
        private RuntimeException destroyFailure;
        private RuntimeException closeFailure;
        private int dependenciesCreated;
        private int dependenciesClosed;
        private int handlerCalls;
        private int destroyCalls;
        private int closeCalls;

        @Override
        public Object create(Class<?> handlerType) {
            throw new AssertionError("handler creation must use the dispatch activation");
        }

        @Override
        public Activation openActivation() {
            return new Activation() {
                private ScopeDependency dependency;

                @Override
                public Object create(Class<?> handlerType) {
                    assertSame(ProbeHandler.class, handlerType);
                    dependency = new ScopeDependency(Probe.this);
                    if (createFailure != null) throw createFailure;
                    return handler;
                }

                @Override
                public void destroy(Object instance) {
                    assertSame(handler, instance);
                    destroyCalls++;
                    assertEquals(0, dependenciesClosed);
                    if (destroyFailure != null) throw destroyFailure;
                }

                @Override
                public void close() {
                    closeCalls++;
                    dependency.close();
                    if (closeFailure != null) throw closeFailure;
                }
            };
        }
    }

    private record ScopeDependency(Probe probe) implements AutoCloseable {
        ScopeDependency {
            probe.dependenciesCreated++;
        }

        @Override
        public void close() {
            probe.dependenciesClosed++;
        }
    }

    private record ProbeHandler(Probe probe) implements ZLinkSendHandler<String> {
        @Override
        public CompletionStage<Void> handle(String message, ZLinkMessageContext context) {
            probe.handlerCalls++;
            if (probe.handlerFailure != null) throw probe.handlerFailure;
            return probe.completion;
        }
    }
}
