package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Managed codec output stays immutable until the existing admission owner releases its parts. */
public final class ZLinkManagedSendPayloadTest {
    private static final String CHANNEL = "managed-payload";
    private static final RoutingId TARGET = RoutingId.from("managed-target");

    @Test
    void successFailureAndCloseKeepTheExistingAdmissionLifetime() {
        for (boolean channel : new boolean[] {false, true}) {
            for (int iteration = 0; iteration < 100; iteration++) {
                try (var fixture = new Fixture(channel)) {
                    byte[] input = "payload".getBytes(StandardCharsets.UTF_8);
                    var payload =
                            ZLinkPayloadEncoding.encodeMessage(
                                    new ZLinkJsonMessageSerializer(), input, "application/json");
                    input[0] = 'X';
                    var success = fixture.call(payload);
                    var clone = success.metadata("key", "value");
                    var admission = new CompletableFuture<Void>();
                    fixture.submission = admission;
                    var result = success.submit().toCompletableFuture();
                    var pending = fixture.parts;
                    assertFalse(result.isDone());
                    assertEquals(
                            "payload",
                            StandardCharsets.UTF_8
                                    .decode(ZLinkFrameworkMultipartView.buffer(pending, 1))
                                    .toString());
                    assertThrows(
                            CompletionException.class,
                            () -> clone.submit().toCompletableFuture().join());
                    admission.complete(null);
                    result.join();
                    assertThrows(
                            IllegalStateException.class,
                            () -> ZLinkFrameworkMultipartView.buffer(payload, 0));
                    assertThrows(
                            IllegalStateException.class,
                            () -> ZLinkFrameworkMultipartView.buffer(pending, 1));

                    var rejection = new IllegalStateException("submit rejected");
                    fixture.submission = CompletableFuture.failedFuture(rejection);
                    var rejected = fixture.call(encoded());
                    var failure =
                            assertThrows(
                                    CompletionException.class,
                                    () -> rejected.submit().toCompletableFuture().join());
                    assertSame(rejection, failure.getCause());
                    assertThrows(
                            IllegalStateException.class,
                            () -> ZLinkFrameworkMultipartView.buffer(fixture.parts, 1));

                    fixture.submission = new CompletableFuture<>();
                    var closing = fixture.call(encoded()).submit().toCompletableFuture();
                    var owned = fixture.parts;
                    var nativePart = owned.getLast();
                    fixture.close();
                    assertThrows(IllegalStateException.class, nativePart::refCount);
                    assertThrows(
                            IllegalStateException.class,
                            () -> ZLinkFrameworkMultipartView.buffer(owned, 1));
                    assertFalse(closing.isDone());
                    ((CompletableFuture<Void>) fixture.submission).complete(null);
                    closing.join();
                }
            }
        }
    }

    private static ZLinkFrameworkMultipartView encoded() {
        return ZLinkPayloadEncoding.encodeMessage(
                new ZLinkJsonMessageSerializer(),
                "payload".getBytes(StandardCharsets.UTF_8),
                "application/json");
    }

    @Test
    void envelopeMovesMaterializedStorageAndKeepsFailedConstructionOwnedByTheCaller() {
        for (int iteration = 0; iteration < 100; iteration++) {
            var source = encoded();
            assertThrows(
                    NullPointerException.class,
                    () -> ZLinkFrameworkMultipartView.encoded(null, source));
            assertEquals(
                    "payload",
                    StandardCharsets.UTF_8
                            .decode(ZLinkFrameworkMultipartView.buffer(source, 0))
                            .toString());
            var nativeBody = source.getFirst();
            var header =
                    ZLinkChannelEnvelope.create(
                            ZLinkChannelEnvelope.KIND_COMMAND,
                            "",
                            "Payload",
                            "application/json",
                            null,
                            java.util.Map.of(),
                            null);
            var parts = ZLinkChannelEnvelope.encode(header, source);
            try (var wire = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(parts)) {
                assertTrue(wire.size() > nativeBody.size());
            } finally {
                ZLinkFrameworkMultipartView.closeAll(parts);
            }
            assertThrows(IllegalStateException.class, nativeBody::refCount);
            assertThrows(
                    IllegalStateException.class,
                    () -> ZLinkFrameworkMultipartView.buffer(source, 0));
        }
    }

    public static void main(String[] args) {
        var test = new ZLinkManagedSendPayloadTest();
        test.successFailureAndCloseKeepTheExistingAdmissionLifetime();
        test.envelopeMovesMaterializedStorageAndKeepsFailedConstructionOwnedByTheCaller();
        System.out.println(
                "channel/node: success=100/100 submit-failure=100/100 close=100/100; immutable input and duplicate-submit controls passed");
    }

    private static final class Fixture implements AutoCloseable {
        final boolean channel;
        final ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        final ZLinkChannelCallRuntime runtime;
        CompletionStage<Void> submission;
        List<Message> parts = List.of();

        Fixture(boolean channel) {
            this.channel = channel;
            var options = new DefaultZLinkFrameworkOptions();
            runtime =
                    new ZLinkChannelCallRuntime(
                            new ZLinkMessageFlowTracer(
                                    options.registration().dispatchOptions(),
                                    ZLinkHandlerActivator.reflection(),
                                    Runnable::run),
                            null,
                            null,
                            null);
            var node =
                    (ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkInternalSpotNode.class},
                                    (proxy, method, arguments) ->
                                            switch (method.getName()) {
                                                case "sendToChannel", "sendToNode" -> {
                                                    @SuppressWarnings("unchecked")
                                                    var sent =
                                                            (List<Message>)
                                                                    arguments[arguments.length - 1];
                                                    parts = sent;
                                                    yield submission;
                                                }
                                                case "routingId" -> RoutingId.from("other-node");
                                                case "classifyNodeSendTarget" ->
                                                        java.util.Optional.empty();
                                                default -> null;
                                            });
            sockets.registerSpotRouterNode(CHANNEL, node);
            options.registration().applicationJobQueue().close();
        }

        ZLinkSendCall call(ZLinkFrameworkMultipartView payload) {
            return channel
                    ? new ChannelSendCall(runtime, CHANNEL, sockets, payload, "Payload")
                    : new RouteSendCall(
                            runtime,
                            CHANNEL,
                            sockets,
                            TARGET,
                            payload,
                            "Payload",
                            "application/json",
                            null);
        }

        @Override
        public void close() {
            runtime.beginClose();
            sockets.closeAll();
            ZLinkFrameworkMultipartView.closeAll(parts);
        }
    }
}
