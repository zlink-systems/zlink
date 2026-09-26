package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkListenerStatus;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.time.Duration;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkFrameworkRuntimeListenerStatusTest {
    @Test
    void listenerStatusAnswersOnlyWhileTheRuntimeOwnsBoundListeners() {
        ZLinkFrameworkRuntime first =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options(), new ZLinkJavaBackendAdapterFactory());
        try {
            assertBound(first, ZLinkListenerKind.ROUTE_MESH, "listener-mesh");
            assertBound(first, ZLinkListenerKind.CLIENT_SERVER, "listener-cs");
        } finally {
            first.close();
        }

        assertThrows(
                ZLinkConfigurationException.class,
                () -> first.listenerStatus(ZLinkListenerKind.ROUTE_MESH, "listener-mesh"));
        assertThrows(
                ZLinkConfigurationException.class,
                () -> first.listenerStatus(ZLinkListenerKind.CLIENT_SERVER, "listener-cs"));

        // A closed runtime is not restartable; a new runtime owns new listeners.
        try (ZLinkFrameworkRuntime second =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options(), new ZLinkJavaBackendAdapterFactory())) {
            assertBound(second, ZLinkListenerKind.ROUTE_MESH, "listener-mesh");
            assertBound(second, ZLinkListenerKind.CLIENT_SERVER, "listener-cs");
        }
    }

    @Test
    void listenerStatusDuringCloseReturnsTheRecordOrConfigurationError() throws Exception {
        String[][] listeners = {
            {"ROUTE_MESH", "listener-mesh"},
            {"CLIENT_SERVER", "listener-cs"},
            {"FANOUT", "listener-fanout"},
            {"STREAM", "listener-stream"}
        };
        for (int round = 0; round < 10; round++) {
            ZLinkFrameworkRuntime runtime =
                    ZLinkFrameworkRuntimeTestAccess.start(
                            options(), new ZLinkJavaBackendAdapterFactory());
            for (String[] listener : listeners) {
                assertBound(runtime, ZLinkListenerKind.valueOf(listener[0]), listener[1]);
            }
            AtomicBoolean querying = new AtomicBoolean(true);
            Queue<Throwable> unexpected = new ConcurrentLinkedQueue<>();
            Thread querier =
                    new Thread(
                            () -> {
                                while (querying.get()) {
                                    for (String[] listener : listeners) {
                                        try {
                                            runtime.listenerStatus(
                                                    ZLinkListenerKind.valueOf(listener[0]),
                                                    listener[1]);
                                        } catch (ZLinkConfigurationException expected) {
                                            // The listener may finish closing during the query.
                                        } catch (Throwable error) {
                                            unexpected.add(error);
                                        }
                                    }
                                }
                            },
                            "listener-status-querier");
            querier.start();
            try {
                runtime.close();
            } finally {
                querying.set(false);
                querier.join();
            }
            assertTrue(unexpected.isEmpty(), () -> "unexpected failures: " + unexpected);
            for (String[] listener : listeners) {
                assertThrows(
                        ZLinkConfigurationException.class,
                        () ->
                                runtime.listenerStatus(
                                        ZLinkListenerKind.valueOf(listener[0]), listener[1]));
            }
        }
    }

    public static final class NoOpSession implements ZLinkSession {
        private final ZLinkSessionContext context;

        public NoOpSession(ZLinkSessionContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static void assertBound(
            ZLinkFrameworkRuntime runtime, ZLinkListenerKind kind, String name) {
        ZLinkListenerStatus status = runtime.listenerStatus(kind, name);
        assertEquals(kind, status.kind());
        assertEquals(name, status.name());
        assertTrue(status.endpoint().startsWith("tcp://127.0.0.1:"), status.endpoint());
        assertFalse(status.endpoint().endsWith(":0"), status.endpoint());
    }

    private static DefaultZLinkFrameworkOptions options() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addRouteMesh("listener-mesh")
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from("listener-status-mesh"));
        options.addClientServerChannel("listener-cs")
                .server()
                .listen(0)
                .addRequestHandler(EchoHandler.class, String.class, String.class);
        options.addFanoutChannel("listener-fanout").enablePublisher("tcp://127.0.0.1:0");
        var stream = options.addStreamNode("listener-stream");
        stream.bind("tcp://127.0.0.1:0");
        stream.registerSession(NoOpSession.class);
        return options;
    }

    public static final class EchoHandler implements ZLinkRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(String request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture(request);
        }
    }
}
