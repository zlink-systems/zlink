package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkListenerStatus;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

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
        return options;
    }

    public static final class EchoHandler implements ZLinkRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(String request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture(request);
        }
    }
}
