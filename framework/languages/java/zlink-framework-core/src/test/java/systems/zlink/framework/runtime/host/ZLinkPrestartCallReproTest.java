package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.host.ZLinkFrameworkRuntimeBootstrap;

import java.lang.reflect.Proxy;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkPrestartCallReproTest {
    enum Entry {
        CHANNEL_SEND,
        CHANNEL_REQUEST,
        NODE_SEND,
        NODE_REQUEST,
        SPOT_SEND,
        SPOT_REQUEST,
        ACTOR_SEND,
        ACTOR_REQUEST,
        FANOUT_PUBLISH,
        SPOT_PUBLISH,
        OUTBOUND_SPOT_SEND,
        OUTBOUND_SPOT_REQUEST,
        OUTBOUND_CHANNEL_SEND,
        OUTBOUND_CHANNEL_REQUEST,
        OUTBOUND_PUBLISH,
        ACTOR_FIND,
        SPOT_FIND
    }

    @ParameterizedTest
    @EnumSource(Entry.class)
    void prestartPublicMessagingEndsWithInvalidOperation(Entry entry) {
        var options = new DefaultZLinkFrameworkOptions();
        var storeCalls = new AtomicInteger();
        options.addLocationStore(
                (ZLinkLocationStore)
                        Proxy.newProxyInstance(
                                ZLinkLocationStore.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationStore.class},
                                (proxy, method, args) -> {
                                    storeCalls.incrementAndGet();
                                    throw new AssertionError(
                                            "prestart call accessed Store: " + method.getName());
                                }));
        options.addRouteMesh("mesh")
                .listen("inproc://prestart-repro")
                .setRoutingId(RoutingId.from("source"))
                .objects()
                .client();
        options.addClientServerChannel("channel").client();
        options.addFanoutChannel("fanout")
                .enablePublisher("inproc://prestart-fanout")
                .setRoutingId(RoutingId.from("publisher"));
        options.validate();
        try (var runtime = ZLinkFrameworkRuntimeBootstrap.prepare(options, null)) {
            try {
                if (entry.name().startsWith("OUTBOUND_")) {
                    assertThrows(
                            systems.zlink.framework.errors.ZLinkConfigurationException.class,
                            () -> terminal(runtime, entry));
                    return;
                }
                CompletionStage<?> terminal = terminal(runtime, entry);
                var failure =
                        assertThrows(
                                CompletionException.class,
                                () -> terminal.toCompletableFuture().join());
                var framework = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
                assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, framework.kind());
            } finally {
                assertEquals(ZLinkFrameworkRuntimeState.PREPARING, runtime.state());
                assertEquals(0, storeCalls.get());
            }
        }
    }

    @org.junit.jupiter.api.Test
    void preparedFacadeAndUnsubmittedBuilderWorkAfterStart() {
        var options = new DefaultZLinkFrameworkOptions();
        options.addFanoutChannel("fanout")
                .enablePublisher("inproc://prepared-fanout")
                .setRoutingId(RoutingId.from("publisher"));
        try (var runtime = ZLinkFrameworkRuntimeBootstrap.prepare(options, null)) {
            var facade = runtime.fanout();
            var reused = facade.publish("fanout", "topic", "before");
            var rejected = reused.submit();
            var deferred = facade.publish("fanout", "topic", "after");
            ZLinkFrameworkRuntimeBootstrap.startPrepared(
                    runtime,
                    options,
                    new systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory(),
                    systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
                            .reflection());
            org.junit.jupiter.api.Assertions.assertSame(facade, runtime.fanout());
            deferred.submit().toCompletableFuture().join();
            reused.submit().toCompletableFuture().join();
            facade.publish("fanout", "topic", "new").submit().toCompletableFuture().join();
            var failure =
                    assertThrows(
                            CompletionException.class, () -> rejected.toCompletableFuture().join());
            assertEquals(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        }
    }

    private static CompletionStage<?> terminal(ZLinkFrameworkRuntime runtime, Entry entry) {
        return switch (entry) {
            case CHANNEL_SEND -> runtime.client().sendToChannel("channel", "message").submit();
            case CHANNEL_REQUEST ->
                    runtime.client().requestToChannel("channel", "message").submit(String.class);
            case NODE_SEND ->
                    runtime.route().sendToNode("mesh", RoutingId.from("node"), "message").submit();
            case NODE_REQUEST ->
                    runtime.route()
                            .requestToNode("mesh", RoutingId.from("node"), "message")
                            .submit(String.class);
            case SPOT_SEND -> runtime.route().sendToSpot("spot", "message").submit();
            case SPOT_REQUEST ->
                    runtime.route().requestToSpot("spot", "message").submit(String.class);
            case ACTOR_SEND -> runtime.actorClient().sendToActor("actor", "message").submit();
            case ACTOR_REQUEST ->
                    runtime.actorClient().requestToActor("actor", "message").submit(String.class);
            case FANOUT_PUBLISH -> runtime.fanout().publish("fanout", "message").submit();
            case SPOT_PUBLISH ->
                    runtime.spotPublisherClient()
                            .publish("mesh", "channel", "topic", "message")
                            .submit();
            case OUTBOUND_SPOT_SEND ->
                    runtime.spotOutbound().sendToSpot("spot", "message").submit();
            case OUTBOUND_SPOT_REQUEST ->
                    runtime.spotOutbound().requestToSpot("spot", "message").submit(String.class);
            case OUTBOUND_CHANNEL_SEND ->
                    runtime.spotOutbound().sendToChannel("channel", "message").submit();
            case OUTBOUND_CHANNEL_REQUEST ->
                    runtime.spotOutbound()
                            .requestToChannel("channel", "message")
                            .submit(String.class);
            case OUTBOUND_PUBLISH ->
                    runtime.spotOutbound().publish("channel", "topic", "message").submit();
            case ACTOR_FIND -> runtime.actorManager().find("actor");
            case SPOT_FIND -> runtime.spotManager().find("spot");
        };
    }
}
