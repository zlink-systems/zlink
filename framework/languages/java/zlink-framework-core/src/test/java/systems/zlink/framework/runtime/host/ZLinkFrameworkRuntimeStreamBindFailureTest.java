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
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
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
        int streamPort = freePort();
        int meshPort = freePort();
        RecordingProvider provider = new RecordingProvider(new ZLinkJavaBackendAdapterFactory());
        try (ServerSocket occupied = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
            options.addRouteMesh("rollback").listen("tcp://127.0.0.1:" + meshPort);
            options.addStreamNode("first")
                    .bind("tcp://127.0.0.1:" + streamPort)
                    .registerSession(FirstSession.class);
            options.addStreamNode("second")
                    .bind("tcp://127.0.0.1:" + occupied.getLocalPort())
                    .registerSession(SecondSession.class);

            assertThrows(
                    RuntimeException.class,
                    () -> ZLinkFrameworkRuntimeTestAccess.start(options, provider));
        }

        //  Binding a port again fails with "address in use" while a socket
        //  that the failed start opened has no owner.
        assertPortReleased(streamPort);
        assertPortReleased(meshPort);
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
        int firstMeshPort = freePort();
        RecordingProvider provider = new RecordingProvider(new ZLinkJavaBackendAdapterFactory());
        try (ServerSocket occupied = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
            options.addRouteMesh("first").listen("tcp://127.0.0.1:" + firstMeshPort);
            options.addRouteMesh("second").listen("tcp://127.0.0.1:" + occupied.getLocalPort());

            assertThrows(
                    RuntimeException.class,
                    () -> ZLinkFrameworkRuntimeTestAccess.start(options, provider));
        }

        assertPortReleased(firstMeshPort);
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
        int meshPort = freePort();
        RecordingProvider provider = new RecordingProvider(new ZLinkJavaBackendAdapterFactory());
        provider.streamAdapterFailure = new AssertionError("stream adapter failed");
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addRouteMesh("rollback").listen("tcp://127.0.0.1:" + meshPort);
        options.addStreamNode("stream")
                .bind("tcp://127.0.0.1:" + freePort())
                .registerSession(FirstSession.class);

        AssertionError failure =
                assertThrows(
                        AssertionError.class,
                        () -> ZLinkFrameworkRuntimeTestAccess.start(options, provider));

        assertSame(provider.streamAdapterFailure, failure);
        assertPortReleased(meshPort);
        assertEquals(1, provider.contexts.size());
        IllegalStateException closed =
                assertThrows(
                        IllegalStateException.class,
                        () -> provider.contexts.getFirst().coreHwmBudgetSnapshot());
        assertEquals("context is closed", closed.getMessage());
    }

    private static int freePort() throws Exception {
        try (ServerSocket probe = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            return probe.getLocalPort();
        }
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
                                Object result;
                                try {
                                    result = method.invoke(adapter, arguments);
                                } catch (InvocationTargetException failure) {
                                    throw failure.getCause();
                                }
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
            return delegate.createMeshAdapter(options);
        }

        @Override
        public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
            if (streamAdapterFailure != null) {
                throw streamAdapterFailure;
            }
            return delegate.createStreamAdapter(options);
        }

        @Override
        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
                ZLinkBackendAdapterOptions options) {
            return delegate.createMonitoringAdapter(options);
        }

        @Override
        public java.util.function.Function<
                        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
                        java.time.Duration>
                admissionTimeout() {
            return delegate.admissionTimeout();
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
