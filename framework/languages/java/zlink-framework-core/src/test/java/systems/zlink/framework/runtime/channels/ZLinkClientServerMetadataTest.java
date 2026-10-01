package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.lang.reflect.Proxy;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;

final class ZLinkClientServerMetadataTest {
    static Map<String, String> sent;
    static Map<String, String> requested;
    static CompletableFuture<Void> sendHandled;

    @Test
    void publicSendAndRequestExposeMetadataSnapshotAndReplyDoesNotCopyIt() throws Exception {
        sent = null;
        requested = null;
        sendHandled = new CompletableFuture<>();
        var options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("work").client();
        var reporter =
                new ZLinkDispatchErrorReporter(
                        options.registration().dispatchOptions(),
                        ZLinkHandlerActivator.reflection(),
                        Runnable::run);
        var registry = new ZLinkChannelDispatchRegistry(Runnable::run);
        registry.registerClientServer(
                "work",
                Map.of(
                        "Send",
                        new ChannelSendHandlerRegistration(SendHandler.class, Send.class, "Send")),
                Map.of(
                        "Request",
                        new ChannelRequestHandlerRegistration(
                                RequestHandler.class, Request.class, String.class, "Request")));
        var invoker =
                new ZLinkChannelHandlerInvoker(
                        new ZLinkJsonMessageSerializer(),
                        new ZLinkCodecRegistration(),
                        ZLinkHandlerActivator.reflection(),
                        Runnable::run,
                        List.of(),
                        List.of());
        var dispatcher =
                new ZLinkChannelMessageDispatcher(
                        registry,
                        invoker,
                        new ZLinkChannelDispatchReporter(reporter),
                        reporter.flow());
        {
            var backend =
                    (ZLinkChannelBackendAdapter)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkChannelBackendAdapter.class},
                                    (proxy, method, args) ->
                                            method.getName().equals("createContext")
                                                    ? Proxy.newProxyInstance(
                                                            getClass().getClassLoader(),
                                                            new Class<?>[] {
                                                                ZLinkBackendContext.class
                                                            },
                                                            (p, m, a) -> null)
                                                    : null);
            var runtime =
                    new ZLinkChannelRuntime(
                            backend,
                            new DefaultZLinkFrameworkOptions().registration(),
                            new ZLinkJsonMessageSerializer());
            var sockets = runtime.channelSocketRegistry();
            systems.zlink.framework.channels.ZLinkRouteClient client = runtime;
            sockets.registerChannel(options.registration().channels().getFirst());
            var dealer =
                    (ZLinkBackendDealerSocket)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkBackendDealerSocket.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("send")
                                                || method.getName().equals("request")) {
                                            @SuppressWarnings("unchecked")
                                            var original = (List<Message>) args[0];
                                            var parts = ZLinkChannelRuntime.copyMessages(original);
                                            assertEquals(2, parts.size());
                                            var reply =
                                                    new CompletableFuture<ZLinkBackendReceived>();
                                            boolean request = method.getName().equals("request");
                                            var received =
                                                    new ZLinkBackendReceived(
                                                            ZLinkBackendRequestResult.OK,
                                                            Optional.of(RoutingId.from("client")),
                                                            Optional.empty(),
                                                            request
                                                                    ? Optional.of(1L)
                                                                    : Optional.empty(),
                                                            parts,
                                                            request
                                                                    ? response -> {
                                                                        assertTrue(
                                                                                ZLinkChannelEnvelope
                                                                                        .decodeHeader(
                                                                                                response
                                                                                                        .getFirst(),
                                                                                                false)
                                                                                        .metadata()
                                                                                        .isEmpty());
                                                                        reply.complete(
                                                                                new ZLinkBackendReceived(
                                                                                        ZLinkBackendRequestResult
                                                                                                .OK,
                                                                                        Optional
                                                                                                .empty(),
                                                                                        Optional
                                                                                                .empty(),
                                                                                        Optional
                                                                                                .empty(),
                                                                                        ZLinkChannelRuntime
                                                                                                .copyMessages(
                                                                                                        response),
                                                                                        null,
                                                                                        () -> {}));
                                                                    }
                                                                    : null,
                                                            () -> {});
                                            dispatcher.dispatchRequest("work", null, received);
                                            return request
                                                    ? reply
                                                    : CompletableFuture.completedFuture(null);
                                        }
                                        return null;
                                    });
            var descriptor =
                    new ZLinkClientServerServerDescriptor(
                            "work",
                            RoutingId.from("server"),
                            1,
                            1,
                            "tcp://127.0.0.1:10001",
                            100,
                            ZLinkFrameworkRuntimeState.SERVING,
                            "default",
                            "local",
                            1,
                            Instant.EPOCH);
            sockets.addClientServerConnection("server", descriptor, dealer);
            assertTrue(sockets.admitClientServerConnection("server", descriptor));
            try {
                var json = new com.fasterxml.jackson.databind.ObjectMapper();
                var fixtures =
                        json.readTree(
                                        java.nio.file.Path.of(
                                                        "../../../runtime/protocol/fixtures/client-server-metadata.json")
                                                .toFile())
                                .get("cases");
                for (var fixture : fixtures) {
                    if (fixture.get("valid").asBoolean()) continue;
                    var header =
                            json.createObjectNode()
                                    .put("formatMarker", 242)
                                    .put("kind", 1)
                                    .put("channelName", "work")
                                    .put("messageName", "Request")
                                    .put("contentType", "application/json");
                    header.set("metadata", fixture.get("metadata"));
                    String headerJson = header.toString();
                    if (fixture.has("receivedEncoded")) {
                        headerJson =
                                headerJson.substring(0, headerJson.lastIndexOf("\"metadata\":"))
                                        + "\"metadata\":"
                                        + fixture.get("receivedEncoded").asText()
                                        + "}";
                    }
                    var rejected =
                            new CompletableFuture<
                                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind>();
                    var invalid =
                            new ZLinkBackendReceived(
                                    ZLinkBackendRequestResult.OK,
                                    Optional.of(RoutingId.from("client")),
                                    Optional.empty(),
                                    Optional.of(2L),
                                    List.of(
                                            Message.from(headerJson),
                                            Message.from("{\"value\":\"bad\"}")),
                                    response ->
                                            rejected.complete(
                                                    systems.zlink.framework.runtime.messaging
                                                            .ZLinkFrameworkErrorReply.kind(
                                                            response)),
                                    () -> {});
                    dispatcher.dispatchRequest("work", null, invalid);
                    assertEquals(
                            systems.zlink.framework.errors.ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                            rejected.get(2, TimeUnit.SECONDS),
                            fixture.get("name").asText());
                    assertNull(requested);
                    assertNull(sent);
                }
                client.sendToChannel("work", new Send("send"))
                        .metadata("tenant", "blue")
                        .submit()
                        .toCompletableFuture()
                        .get(2, TimeUnit.SECONDS);
                sendHandled.get(2, TimeUnit.SECONDS);
                String reply =
                        client.requestToChannel("work", new Request("request"))
                                .metadata("tenant", "blue")
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(2, TimeUnit.SECONDS);
                assertEquals("blue", reply);
                assertEquals(Map.of("tenant", "blue"), sent);
                assertEquals(Map.of("tenant", "blue"), requested);

                sendHandled = new CompletableFuture<>();
                client.sendToChannel("work", new Send("new"))
                        .submit()
                        .toCompletableFuture()
                        .get(2, TimeUnit.SECONDS);
                sendHandled.get(2, TimeUnit.SECONDS);
                assertTrue(sent.isEmpty());
                assertEquals(
                        "empty",
                        client.requestToChannel("work", new Request("new"))
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(2, TimeUnit.SECONDS));
                assertTrue(requested.isEmpty());
            } finally {
                runtime.close();
                sockets.closeAll();
            }
        }
    }

    public record Send(String value) {}

    public record Request(String value) {}

    public static final class SendHandler implements ZLinkSendHandler<Send> {
        public CompletionStage<Void> handle(Send value, ZLinkMessageContext context) {
            assertThrows(
                    UnsupportedOperationException.class, () -> context.metadata().put("x", "y"));
            sent = Map.copyOf(context.metadata());
            sendHandled.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class RequestHandler implements ZLinkRequestHandler<Request, String> {
        public CompletionStage<String> handle(Request value, ZLinkMessageContext context) {
            requested = Map.copyOf(context.metadata());
            return CompletableFuture.completedFuture(
                    context.metadata().getOrDefault("tenant", "empty"));
        }
    }
}
