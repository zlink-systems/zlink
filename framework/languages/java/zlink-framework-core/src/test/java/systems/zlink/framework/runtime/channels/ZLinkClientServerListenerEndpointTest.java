package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.net.URI;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkClientServerListenerEndpointTest {
    @ParameterizedTest
    @ValueSource(strings = {"*", "0"})
    void registryListenerUsesTheBindingEndpoint(String port) {
        var backend =
                new ZLinkJavaBackendAdapterFactory()
                        .createChannelAdapter(
                                new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)));
        try (ZLinkChannelRuntime runtime =
                new ZLinkChannelRuntime(
                        backend, options(port).registration(), new ZLinkJsonMessageSerializer())) {
            String boundEndpoint = runtime.serverSocket("listener-cs").lastEndpoint();
            assertTrue(URI.create(boundEndpoint).getPort() > 0, boundEndpoint);
            assertEquals(
                    boundEndpoint,
                    runtime.listenerEndpoint(ZLinkListenerKind.CLIENT_SERVER, "listener-cs"));
        }
    }

    @ParameterizedTest
    @ValueSource(strings = {"*", "0"})
    void publicListenerStatusPublishesTheBoundPort(String port) {
        try (ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options(port))) {
            String endpoint =
                    runtime.listenerStatus(ZLinkListenerKind.CLIENT_SERVER, "listener-cs")
                            .endpoint();
            assertEquals("127.0.0.1", URI.create(endpoint).getHost());
            int boundPort = URI.create(endpoint).getPort();
            assertTrue(boundPort > 0 && boundPort <= 65_535, endpoint);
        }
    }

    private static DefaultZLinkFrameworkOptions options(String port) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("listener-cs")
                .server()
                .listen(0)
                .addRequestHandler(EchoHandler.class, String.class, String.class);
        // The public ClientServer builder accepts integer ports. Supply the alternate
        // binding notation directly to the registration to exercise the shared registry.
        options.registration()
                .channels()
                .getFirst()
                .replaceClientServerBind("tcp://127.0.0.1:" + port);
        return options;
    }

    public static final class EchoHandler implements ZLinkRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(String request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture(request);
        }
    }
}
