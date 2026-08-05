package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;

final class FakeMeshDispatchIntegrationTest {
    @BeforeEach
    void resetHandlers() {
        NodeHandler.received = new CompletableFuture<>();
        ChannelHandler.received = new CompletableFuture<>();
    }

    @Test
    void runtimeWiresFormalMeshNodeAndChannelHandlers() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        var mesh = options.addRouteMesh("game")
            .listen("inproc://formal-mesh-dispatch")
            .addRouteSendHandler(NodeHandler.class, String.class);
        mesh.channelName("play")
            .server()
            .addSendHandler(ChannelHandler.class, String.class);
        FakeZLinkBackendAdapterFactory backend = new FakeZLinkBackendAdapterFactory();

        try (ZLinkFrameworkRuntime ignored =
                 ZLinkFrameworkRuntimeTestAccess.start(options, backend)) {
            backend.dispatchMeshNodeSend("String", "\"node-value\"");
            backend.dispatchMeshChannelSend("play", "String", "\"channel-value\"");

            assertEquals("node-value@fake-mesh-source",
                NodeHandler.received.get(2, TimeUnit.SECONDS));
            assertEquals("channel-value@play",
                ChannelHandler.received.get(2, TimeUnit.SECONDS));
        }
    }

    public static final class NodeHandler implements ZLinkRouteSendHandler<String> {
        private static CompletableFuture<String> received;

        public NodeHandler() {
        }

        @Override
        public CompletionStage<Void> handle(
            String message,
            ZLinkRouteMessageContext context) {
            received.complete(message + "@" + context.sourceNodeRid());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ChannelHandler implements ZLinkSendHandler<String> {
        private static CompletableFuture<String> received;

        public ChannelHandler() {
        }

        @Override
        public CompletionStage<Void> handle(
            String message,
            ZLinkMessageContext context) {
            received.complete(message + "@" + context.channelName().orElse(""));
            return CompletableFuture.completedFuture(null);
        }
    }
}
