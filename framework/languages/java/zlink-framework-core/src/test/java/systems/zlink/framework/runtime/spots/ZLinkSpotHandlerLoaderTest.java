package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscription;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;
import systems.zlink.testfixtures.subscriptionmissing.MissingTopicSubscription;

import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkSpotHandlerLoaderTest {
    @Test
    void configuredActorHandlerIsIndexedAndPreparesSerializerTypes() {
        TrackingSerializer serializer = new TrackingSerializer();
        ZLinkScannedHandlerCatalog scannedHandlers = new ZLinkScannedHandlerCatalog(List.of());
        ZLinkSpotActorHandlerCatalog actorHandlers = new ZLinkSpotActorHandlerCatalog(serializer);
        Set<Class<?>> preparedHandlers = new HashSet<>();
        ZLinkSpotHandlerLoader loader =
                new ZLinkSpotHandlerLoader(
                        scannedHandlers, actorHandlers, trackingActivator(preparedHandlers));

        var loaded =
                loader.load(
                        TestSpot.class,
                        List.of(ConfiguredActorHandler.class),
                        (name, period, handlerType, options) ->
                                CompletableFuture.completedFuture(null));

        List<SpotActorPacketHandlerRegistration> registrations =
                loaded.actorPacketHandlers().get("configured-request");
        assertEquals(1, registrations.size());
        assertEquals(ZLinkScannedHandlerKind.ACTOR_REQUEST, registrations.get(0).kind());
        assertEquals(TestSpot.class, registrations.get(0).spotType());
        assertTrue(preparedHandlers.contains(ConfiguredActorHandler.class));
        assertTrue(serializer.preparedTypes.contains(Request.class));
        assertTrue(serializer.preparedTypes.contains(Reply.class));
    }

    @Test
    void duplicateActorHandlerIsIgnoredOnlyWithinTheSameInstance() {
        var scanned = new ZLinkScannedHandlerCatalog(List.of());
        var loader =
                new ZLinkSpotHandlerLoader(
                        scanned,
                        new ZLinkSpotActorHandlerCatalog(new TrackingSerializer()),
                        type -> null);
        var first =
                loader.load(
                        TestSpot.class,
                        List.of(ConfiguredActorHandler.class, ConfiguredActorHandler.class),
                        (name, period, type, options) -> CompletableFuture.completedFuture(null));
        var second =
                loader.load(
                        TestSpot.class,
                        List.of(OtherActorHandler.class),
                        (name, period, type, options) -> CompletableFuture.completedFuture(null));
        assertEquals(1, first.actorPacketHandlers().get("configured-request").size());
        assertEquals(
                ConfiguredActorHandler.class,
                first.actorPacketHandlers().get("configured-request").getFirst().handlerType());
        assertEquals(
                OtherActorHandler.class,
                second.actorPacketHandlers().get("configured-request").getFirst().handlerType());
        var error =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                loader.load(
                                        TestSpot.class,
                                        List.of(
                                                ConfiguredActorHandler.class,
                                                OtherActorHandler.class),
                                        (name, period, type, options) ->
                                                CompletableFuture.completedFuture(null)));
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, error.kind());
        assertInstanceOf(ZLinkConfigurationException.class, error.getCause());
        assertTrue(error.getMessage().contains("duplicate Spot actor packet handler"));
        assertThrows(
                UnsupportedOperationException.class, () -> first.actorPacketHandlers().clear());
        assertThrows(
                UnsupportedOperationException.class,
                () -> first.actorPacketHandlers().get("configured-request").clear());
    }

    @Test
    void scannedActorHandlerUsesEachMatchingSpotRegistrationAndDeduplicatesConfigure()
            throws Exception {
        var method =
                ScopedScannedActorHandler.class.getMethod(
                        "handle",
                        TestSpot.class,
                        TestActor.class,
                        systems.zlink.framework.ZLinkMessageContext.class,
                        Request.class);
        var scanned =
                new ZLinkScannedHandlerCatalog(
                        List.of(
                                new ZLinkScannedHandler(
                                        ZLinkScannedHandlerSurface.SPOT,
                                        ZLinkScannedHandlerKind.ACTOR_REQUEST,
                                        ScopedScannedActorHandler.class,
                                        method,
                                        TestSpot.class,
                                        Request.class,
                                        Reply.class,
                                        "configured-request",
                                        "",
                                        "",
                                        null,
                                        Set.of())));
        var serializer = new TrackingSerializer();
        var loader =
                new ZLinkSpotHandlerLoader(
                        scanned, new ZLinkSpotActorHandlerCatalog(serializer), type -> null);
        var first =
                loader.load(
                        TestSpot.class,
                        List.of(ScopedScannedActorHandler.class),
                        (name, period, type, options) -> CompletableFuture.completedFuture(null));
        var second =
                loader.load(
                        TestSpot.class,
                        List.of(),
                        (name, period, type, options) -> CompletableFuture.completedFuture(null));
        var unrelated =
                loader.load(
                        Object.class,
                        List.of(),
                        (name, period, type, options) -> CompletableFuture.completedFuture(null));
        assertEquals(1, first.actorPacketHandlers().get("configured-request").size());
        assertEquals(
                ScopedScannedActorHandler.class,
                second.actorPacketHandlers().get("configured-request").getFirst().handlerType());
        assertTrue(unrelated.actorPacketHandlers().isEmpty());
        assertTrue(serializer.preparedTypes.containsAll(Set.of(Request.class, Reply.class)));
    }

    @Test
    void scannedActorHandlerWithoutSpotTypeRemainsAvailableAsFallback() throws Exception {
        var method =
                ConfiguredActorHandler.class.getMethod("handle", TestActor.class, Request.class);
        var scanned =
                new ZLinkScannedHandlerCatalog(
                        List.of(
                                new ZLinkScannedHandler(
                                        ZLinkScannedHandlerSurface.SPOT,
                                        ZLinkScannedHandlerKind.ACTOR_REQUEST,
                                        ConfiguredActorHandler.class,
                                        method,
                                        Request.class,
                                        Reply.class,
                                        "configured-request",
                                        Set.of())));
        var loader =
                new ZLinkSpotHandlerLoader(
                        scanned,
                        new ZLinkSpotActorHandlerCatalog(new TrackingSerializer()),
                        type -> null);
        for (Class<?> spotType : List.of(TestSpot.class, Object.class)) {
            var loaded =
                    loader.load(
                            spotType,
                            List.of(),
                            (name, period, type, options) ->
                                    CompletableFuture.completedFuture(null));
            assertEquals(1, loaded.actorPacketHandlers().get("configured-request").size());
        }
    }

    @Test
    void sameTopicAndPacketCannotDeclareDifferentSubscriptionTypes() {
        ZLinkScannedHandlerCatalog scannedHandlers = new ZLinkScannedHandlerCatalog(List.of());
        ZLinkSpotHandlerLoader loader =
                new ZLinkSpotHandlerLoader(
                        scannedHandlers,
                        new ZLinkSpotActorHandlerCatalog(new TrackingSerializer()),
                        handlerType -> null);

        assertThrows(
                ZLinkFrameworkException.class,
                () ->
                        loader.load(
                                TestSpot.class,
                                List.of(FirstSubscription.class, SecondSubscription.class),
                                (name, period, handlerType, options) ->
                                        CompletableFuture.completedFuture(null)));
    }

    @Test
    void configuredInterfaceSubscriptionRequiresTopic() {
        ZLinkScannedHandlerCatalog scannedHandlers = new ZLinkScannedHandlerCatalog(List.of());
        ZLinkSpotHandlerLoader loader =
                new ZLinkSpotHandlerLoader(
                        scannedHandlers,
                        new ZLinkSpotActorHandlerCatalog(new TrackingSerializer()),
                        handlerType -> null);

        ZLinkFrameworkException error =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                loader.load(
                                        Object.class,
                                        List.of(MissingTopicSubscription.class),
                                        (name, period, handlerType, options) ->
                                                CompletableFuture.completedFuture(null)));

        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, error.kind());
        assertEquals(
                "SPOT subscription handler topic is required: "
                        + MissingTopicSubscription.class.getName(),
                error.getMessage());
    }

    @Test
    void configuredInterfaceSubscriptionWithTopicRegisters() {
        ZLinkScannedHandlerCatalog scannedHandlers = new ZLinkScannedHandlerCatalog(List.of());
        ZLinkSpotHandlerLoader loader =
                new ZLinkSpotHandlerLoader(
                        scannedHandlers,
                        new ZLinkSpotActorHandlerCatalog(new TrackingSerializer()),
                        handlerType -> null);

        ZLinkSpotHandlerCatalog.Registrations registrations =
                loader.load(
                        TestSpot.class,
                        List.of(ConfiguredTopicSubscription.class),
                        (name, period, handlerType, options) ->
                                CompletableFuture.completedFuture(null));

        assertEquals(
                1,
                registrations
                        .subscriptionHandlers()
                        .get(new ZLinkBackendSubscription("events", "shared-topic"))
                        .size());
        assertEquals(
                ConfiguredTopicSubscription.class,
                registrations
                        .subscriptionHandlers()
                        .get(new ZLinkBackendSubscription("events", "shared-topic"))
                        .get(0)
                        .handlerType());
    }

    @Test
    void duplicateSubscriptionIsRejected() {
        var scanned = new ZLinkScannedHandlerCatalog(List.of());
        var loader =
                new ZLinkSpotHandlerLoader(
                        scanned,
                        new ZLinkSpotActorHandlerCatalog(new TrackingSerializer()),
                        type -> null);
        assertThrows(
                ZLinkFrameworkException.class,
                () ->
                        loader.load(
                                TestSpot.class,
                                List.of(
                                        ConfiguredTopicSubscription.class,
                                        ConfiguredTopicSubscription.class),
                                (name, period, type, options) ->
                                        CompletableFuture.completedFuture(null)));
    }

    @Test
    void dynamicSubscriptionUsesRuntimeArgumentsWithoutAnnotation() throws Exception {
        var catalog =
                configuredSubscription(
                        DynamicSubscription.class,
                        "channel",
                        "dynamic." + TestSpot.class.getSimpleName());
        assertEquals(
                java.util.Set.of(
                        new ZLinkBackendSubscription(
                                "channel", "dynamic." + TestSpot.class.getSimpleName())),
                catalog.subscriptions());
        assertEquals(
                1,
                catalog.subscriptionHandlers("channel", "dynamic." + TestSpot.class.getSimpleName())
                        .size());
        assertTrue(
                catalog.subscriptionHandlers("other", "dynamic." + TestSpot.class.getSimpleName())
                        .isEmpty());
    }

    @Test
    void annotationAndDynamicSubscriptionCoexist() throws Exception {
        var catalog =
                configuredSubscription(ConfiguredTopicSubscription.class, "channel", "dynamic");
        assertEquals(2, catalog.subscriptions().size());
    }

    private ZLinkSpotHandlerCatalog configuredSubscription(
            Class<?> type, String channel, String topic) throws Exception {
        var catalog = new ZLinkSpotHandlerCatalog("closed");
        catalog.addSubscribe(type, channel, topic);
        var scanned = new ZLinkScannedHandlerCatalog(List.of());
        var loader =
                new ZLinkSpotHandlerLoader(
                        scanned,
                        new ZLinkSpotActorHandlerCatalog(new TrackingSerializer()),
                        ignored -> null);
        catalog.closeRegistration(
                (types, subscriptions) ->
                        loader.load(
                                TestSpot.class,
                                types,
                                subscriptions,
                                (name, period, handlerType, options) ->
                                        CompletableFuture.completedFuture(null)));
        return catalog;
    }

    private static final class DynamicSubscription
            implements ZLinkSpotSubscriptionHandler<TestSpot, FirstEvent> {
        public CompletionStage<Void> handle(TestSpot spot, FirstEvent event) {
            return CompletableFuture.completedFuture(null);
        }
    }

    @Test
    void duplicateDynamicAndAnnotationKeysAreRejected() {
        var scanned = new ZLinkScannedHandlerCatalog(List.of());
        var loader =
                new ZLinkSpotHandlerLoader(
                        scanned,
                        new ZLinkSpotActorHandlerCatalog(new TrackingSerializer()),
                        ignored -> null);
        var catalog = new ZLinkSpotHandlerCatalog("closed");
        catalog.addSubscribe(ConfiguredTopicSubscription.class, "events", "shared-topic");
        assertThrows(
                ZLinkFrameworkException.class,
                () ->
                        catalog.closeRegistration(
                                (types, subscriptions) ->
                                        loader.load(
                                                TestSpot.class,
                                                types,
                                                subscriptions,
                                                (name, period, type, options) ->
                                                        CompletableFuture.completedFuture(null))));
    }

    private static ZLinkHandlerActivator trackingActivator(Set<Class<?>> preparedHandlers) {
        return new ZLinkHandlerActivator() {
            @Override
            public Object create(Class<?> handlerType) {
                return null;
            }

            @Override
            public void prepare(Class<?> handlerType) {
                preparedHandlers.add(handlerType);
            }
        };
    }

    private static final class ConfiguredActorHandler {
        @ZLinkSpotActorRequest(packetName = "configured-request")
        public CompletionStage<Reply> handle(TestActor actor, Request request) {
            return CompletableFuture.completedFuture(new Reply());
        }
    }

    // Scoped to TestSpot: the handler scan of other test suites also finds this class, and an
    // unscoped second handler for the same packet would collide with ConfiguredActorHandler there.
    private static final class OtherActorHandler {
        @ZLinkSpotActorRequest(packetName = "configured-request")
        public CompletionStage<Reply> handle(
                TestSpot spot,
                TestActor actor,
                systems.zlink.framework.ZLinkMessageContext context,
                Request request) {
            return CompletableFuture.completedFuture(new Reply());
        }
    }

    private static final class ScopedScannedActorHandler {
        @ZLinkSpotActorRequest(packetName = "configured-request")
        public CompletionStage<Reply> handle(
                TestSpot spot,
                TestActor actor,
                systems.zlink.framework.ZLinkMessageContext context,
                Request request) {
            return CompletableFuture.completedFuture(new Reply());
        }
    }

    private static final class TestSpot {}

    private static final class TestActor {}

    @ZLinkSpotSubscription(channelName = "events", topic = "shared-topic")
    private static final class ConfiguredTopicSubscription
            implements ZLinkSpotSubscriptionHandler<TestSpot, FirstEvent> {
        @Override
        public CompletionStage<Void> handle(TestSpot spot, FirstEvent event) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class Request {}

    private static final class Reply {}

    @ZLinkPacket("shared-packet")
    private static final class FirstEvent {}

    @ZLinkPacket("shared-packet")
    private static final class SecondEvent {}

    private static final class FirstSubscription {
        @ZLinkSpotSubscription(channelName = "events", topic = "shared-topic")
        public CompletionStage<Void> handle(TestSpot spot, FirstEvent event) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class SecondSubscription {
        @ZLinkSpotSubscription(channelName = "events", topic = "shared-topic")
        public CompletionStage<Void> handle(TestSpot spot, SecondEvent event) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class TrackingSerializer implements ZLinkMessageSerializer {
        private final Set<Class<?>> preparedTypes = new HashSet<>();

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            throw new UnsupportedOperationException();
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new UnsupportedOperationException();
        }

        @Override
        public void prepare(Class<?> type) {
            preparedTypes.add(type);
        }
    }
}
