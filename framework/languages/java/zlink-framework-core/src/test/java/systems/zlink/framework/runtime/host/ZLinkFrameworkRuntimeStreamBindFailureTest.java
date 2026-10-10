package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.URI;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkFrameworkRuntimeStreamBindFailureTest {
    /**
     * A STREAM node whose bind fails ends startup with that failure, and the startup rollback
     * releases everything the start had already opened: the listener of the STREAM node bound
     * before the failure, the MeshNode listener and the backend context.
     */
    @Test
    void failedStreamBindReleasesEverythingTheStartAlreadyOpened() throws Exception {
        RecordingProvider provider = new RecordingProvider(new ZLinkJavaBackendAdapterFactory());
        try (ServerSocket occupied = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
            options.addRouteMesh("rollback").listen("tcp://127.0.0.1:0");
            options.addStreamNode("first")
                    .bind("tcp://127.0.0.1:0")
                    .registerSession(FirstSession.class);
            options.addStreamNode("second")
                    .bind("tcp://127.0.0.1:" + occupied.getLocalPort())
                    .registerSession(SecondSession.class);

            assertThrows(
                    RuntimeException.class,
                    () -> ZLinkFrameworkRuntimeTestAccess.start(options, provider));
        }

        assertListenersReleased(provider, 1);
        assertEquals(1, provider.contexts.size());
        IllegalStateException closed =
                assertThrows(
                        IllegalStateException.class,
                        () -> provider.contexts.getFirst().coreHwmBudgetSnapshot());
        assertEquals("context is closed", closed.getMessage());
    }

    /**
     * A MeshNode whose bind fails ends startup, and the same rollback releases the MeshNode opened
     * before it and the backend context.
     */
    @Test
    void failedMeshBindReleasesEverythingTheStartAlreadyOpened() throws Exception {
        RecordingProvider provider = new RecordingProvider(new ZLinkJavaBackendAdapterFactory());
        try (ServerSocket occupied = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
            options.addRouteMesh("first").listen("tcp://127.0.0.1:0");
            options.addRouteMesh("second").listen("tcp://127.0.0.1:" + occupied.getLocalPort());

            assertThrows(
                    RuntimeException.class,
                    () -> ZLinkFrameworkRuntimeTestAccess.start(options, provider));
        }

        assertListenersReleased(provider, 0);
        assertEquals(1, provider.contexts.size());
        IllegalStateException closed =
                assertThrows(
                        IllegalStateException.class,
                        () -> provider.contexts.getFirst().coreHwmBudgetSnapshot());
        assertEquals("context is closed", closed.getMessage());
    }

    /**
     * A start that fails with an {@link Error} rolls back like one that fails with a runtime
     * exception: the Error reaches the caller, and the MeshNode listener and the backend context
     * that the start opened are released.
     */
    @Test
    void startFailingWithAnErrorIsRolledBack() throws Exception {
        RecordingProvider provider = new RecordingProvider(new ZLinkJavaBackendAdapterFactory());
        provider.streamAdapterFailure = new AssertionError("stream adapter failed");
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addRouteMesh("rollback").listen("tcp://127.0.0.1:0");
        options.addStreamNode("stream")
                .bind("tcp://127.0.0.1:0")
                .registerSession(FirstSession.class);

        AssertionError failure =
                assertThrows(
                        AssertionError.class,
                        () -> ZLinkFrameworkRuntimeTestAccess.start(options, provider));

        assertSame(provider.streamAdapterFailure, failure);
        assertListenersReleased(provider, 0);
        assertEquals(1, provider.contexts.size());
        IllegalStateException closed =
                assertThrows(
                        IllegalStateException.class,
                        () -> provider.contexts.getFirst().coreHwmBudgetSnapshot());
        assertEquals("context is closed", closed.getMessage());
    }

    private static void assertListenersReleased(RecordingProvider provider, int streamCount)
            throws Exception {
        assertEquals(streamCount, provider.streamEndpoints.size());
        for (String endpoint : provider.streamEndpoints) {
            assertPortReleased(URI.create(endpoint).getPort());
        }
        assertPortReleased(
                URI.create(provider.meshNodes.getFirst().advertisedEndpoint()).getPort());
    }

    private static void assertPortReleased(int port) throws Exception {
        try (ServerSocket rebound = new ServerSocket()) {
            rebound.bind(new InetSocketAddress(InetAddress.getLoopbackAddress(), port), 1);
        }
    }

    /** Delegates to the real backend and records the contexts the runtime creates. */
    private static final class RecordingProvider implements ZLinkBackendAdapterProvider {
        private final ZLinkBackendAdapterProvider delegate;
        private final List<ZLinkBackendContext> contexts = new ArrayList<>();
        private final List<String> streamEndpoints = new ArrayList<>();
        private final List<ZLinkInternalMeshNode> meshNodes = new ArrayList<>();
        private Error streamAdapterFailure;

        RecordingProvider(ZLinkBackendAdapterProvider delegate) {
            this.delegate = delegate;
        }

        @Override
        public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
            ZLinkChannelBackendAdapter adapter = delegate.createChannelAdapter(options);
            return (ZLinkChannelBackendAdapter)
                    Proxy.newProxyInstance(
                            ZLinkChannelBackendAdapter.class.getClassLoader(),
                            new Class<?>[] {ZLinkChannelBackendAdapter.class},
                            (proxy, method, arguments) -> {
                                Object result = invoke(adapter, method, arguments);
                                if (method.getName().equals("createContext")) {
                                    contexts.add((ZLinkBackendContext) result);
                                }
                                return result;
                            });
        }

        @Override
        public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createSpotAdapter(options);
        }

        @Override
        public ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
            var adapter = delegate.createMeshAdapter(options);
            return (context, name) -> {
                var node = adapter.createMeshNode(context, name);
                meshNodes.add(node);
                return node;
            };
        }

        @Override
        public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
            if (streamAdapterFailure != null) {
                throw streamAdapterFailure;
            }
            var adapter = delegate.createStreamAdapter(options);
            return (context, mesh) -> {
                var socket = adapter.createStreamSocket(context, mesh);
                return (ZLinkBackendStreamSocket)
                        Proxy.newProxyInstance(
                                ZLinkBackendStreamSocket.class.getClassLoader(),
                                new Class<?>[] {ZLinkBackendStreamSocket.class},
                                (proxy, method, arguments) -> {
                                    Object result = invoke(socket, method, arguments);
                                    if (method.getName().equals("bind")) {
                                        streamEndpoints.add(socket.lastEndpoint());
                                    }
                                    return result;
                                });
            };
        }

        private static Object invoke(Object target, Method method, Object[] arguments)
                throws Throwable {
            try {
                return method.invoke(target, arguments);
            } catch (InvocationTargetException failure) {
                throw failure.getCause();
            }
        }

        @Override
        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
                ZLinkBackendAdapterOptions options) {
            return delegate.createMonitoringAdapter(options);
        }
    }

    public static final class FirstSession extends NoopSession {
        public FirstSession(ZLinkSessionContext context) {
            super(context);
        }
    }

    public static final class SecondSession extends NoopSession {
        public SecondSession(ZLinkSessionContext context) {
            super(context);
        }
    }

    private abstract static class NoopSession implements ZLinkSession {
        private final ZLinkSessionContext context;

        NoopSession(ZLinkSessionContext context) {
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
    }
}
