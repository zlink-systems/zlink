package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkStreamNodeBuilder;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkSessionRelocationSealTimeoutWiringTest {
    @Test
    void configuredRootLocationValueReachesTheActualSessionSealOwner() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        Object locations = options.configureLocations();
        locations
                .getClass()
                .getMethod("setSessionRelocationSealTimeout", Duration.class)
                .invoke(locations, Duration.ofMillis(17));
        options.addLocationStore(
                new systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore());
        options.addRouteMesh("game").listen("tcp://127.0.0.1:0").objects().client();
        options.addStreamNode("gateway")
                .bind("inproc://session-seal-timeout-owner")
                .enableActorDispatch()
                .registerSession(TestSession.class);

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            locations
                    .getClass()
                    .getMethod("setSessionRelocationSealTimeout", Duration.class)
                    .invoke(locations, Duration.ofMillis(29));
            Object sessionOwner =
                    runtime.sessionActors("gateway", RoutingId.from("session-timeout"));
            Field timeout =
                    sessionOwner.getClass().getDeclaredField("sessionRelocationSealTimeout");
            timeout.setAccessible(true);
            assertEquals(Duration.ofMillis(17), timeout.get(sessionOwner));
        }
    }

    /** Session-Actor binding §4: binding generations are unique across Sessions of one node. */
    @Test
    void bindingGenerationsAreUniqueAcrossSessionsOfOneSessionOwnerNode() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(
                new systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore());
        var server = options.addRouteMesh("game").listen("tcp://127.0.0.1:0").objects().server();
        server.addEntrySpot(TestEntry.class);
        server.addActorFactory(
                "probe",
                TestActor.class,
                TestFactory.class,
                factory -> factory.disableRelocation());
        options.addStreamNode("gateway")
                .bind("tcp://127.0.0.1:0")
                .enableActorDispatch()
                .registerSession(TestSession.class);
        TestSession.connected.clear();
        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            var created =
                    (systems.zlink.framework.actors.ZLinkActorCreateResult.Created)
                            runtime.actorManager()
                                    .getOrCreate("actor-1", "probe")
                                    .submit()
                                    .toCompletableFuture()
                                    .get(3, java.util.concurrent.TimeUnit.SECONDS);
            var endpoint =
                    java.net.URI.create(
                            runtime.listenerStatus(
                                            systems.zlink.framework.monitoring.ZLinkListenerKind
                                                    .STREAM,
                                            "gateway")
                                    .endpoint());
            try (var firstClient = new java.net.Socket(endpoint.getHost(), endpoint.getPort())) {
                var firstContext =
                        TestSession.connected.poll(3, java.util.concurrent.TimeUnit.SECONDS);
                org.junit.jupiter.api.Assertions.assertNotNull(firstContext);
                var first =
                        runtime.sessionActors("gateway", firstContext.routingId().orElseThrow());
                first.bindOrGet(created.actor())
                        .toCompletableFuture()
                        .get(3, java.util.concurrent.TimeUnit.SECONDS);
                long firstGeneration = currentBindingGeneration(first, "actor-1");
                try (var secondClient =
                        new java.net.Socket(endpoint.getHost(), endpoint.getPort())) {
                    var secondContext =
                            TestSession.connected.poll(3, java.util.concurrent.TimeUnit.SECONDS);
                    org.junit.jupiter.api.Assertions.assertNotNull(secondContext);
                    var second =
                            runtime.sessionActors(
                                    "gateway", secondContext.routingId().orElseThrow());
                    second.bindOrGet(created.actor())
                            .toCompletableFuture()
                            .get(3, java.util.concurrent.TimeUnit.SECONDS);
                    long secondGeneration = currentBindingGeneration(second, "actor-1");
                    assertNotEquals(firstGeneration, secondGeneration);
                }
            }
        }
    }

    private static long currentBindingGeneration(Object sessionOwner, String actorId)
            throws Exception {
        var lookup =
                sessionOwner.getClass().getDeclaredMethod("currentBindingGeneration", String.class);
        lookup.setAccessible(true);
        return (long) lookup.invoke(sessionOwner, actorId);
    }

    public record TestActor(systems.zlink.framework.actors.ZLinkActorContext context)
            implements systems.zlink.framework.actors.ZLinkActor {}

    public static final class TestFactory
            implements systems.zlink.framework.actors.ZLinkActorFactory {
        public CompletionStage<systems.zlink.framework.actors.ZLinkActor> create(
                systems.zlink.framework.actors.ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new TestActor(context));
        }
    }

    public record TestEntry(systems.zlink.framework.spots.ZLinkEntrySpotContext context)
            implements systems.zlink.framework.spots.ZLinkEntrySpot<TestActor> {
        public CompletionStage<Void> onJoinedActor(TestActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(TestActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    @Test
    void streamBuilderDoesNotExposeASecondTimeoutOwner() {
        assertThrows(
                NoSuchMethodException.class,
                () ->
                        ZLinkStreamNodeBuilder.class.getMethod(
                                "sessionRelocationSealTimeout", Duration.class));
    }

    public static final class TestSession implements ZLinkSession {
        static final java.util.concurrent.BlockingQueue<ZLinkSessionContext> connected =
                new java.util.concurrent.LinkedBlockingQueue<>();
        private final ZLinkSessionContext context;

        public TestSession(ZLinkSessionContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            connected.add(context);
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
