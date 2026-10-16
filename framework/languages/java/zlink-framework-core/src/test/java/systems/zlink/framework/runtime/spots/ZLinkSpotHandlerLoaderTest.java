package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
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
        ZLinkSpotActorHandlerCatalog actorHandlers =
                new ZLinkSpotActorHandlerCatalog(scannedHandlers, serializer);
        Set<Class<?>> preparedHandlers = new HashSet<>();
        ZLinkSpotHandlerLoader loader =
                new ZLinkSpotHandlerLoader(
                        scannedHandlers, actorHandlers, trackingActivator(preparedHandlers));

        loader.load(
                TestSpot.class,
                List.of(ConfiguredActorHandler.class),
                (name, period, handlerType, options) -> CompletableFuture.completedFuture(null));

        List<SpotActorPacketHandlerRegistration> registrations =
                actorHandlers.handlers("configured-request");
        assertEquals(1, registrations.size());
        assertEquals(ZLinkScannedHandlerKind.ACTOR_REQUEST, registrations.get(0).kind());
        assertEquals(TestSpot.class, registrations.get(0).spotType());
        assertTrue(preparedHandlers.contains(ConfiguredActorHandler.class));
        assertTrue(serializer.preparedTypes.contains(Request.class));
        assertTrue(serializer.preparedTypes.contains(Reply.class));
    }

    @Test
    void sameTopicAndPacketCannotDeclareDifferentSubscriptionTypes() {
        ZLinkScannedHandlerCatalog scannedHandlers = new ZLinkScannedHandlerCatalog(List.of());
        ZLinkSpotHandlerLoader loader =
                new ZLinkSpotHandlerLoader(
                        scannedHandlers,
                        new ZLinkSpotActorHandlerCatalog(scannedHandlers, new TrackingSerializer()),
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
                        new ZLinkSpotActorHandlerCatalog(scannedHandlers, new TrackingSerializer()),
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
                        new ZLinkSpotActorHandlerCatalog(scannedHandlers, new TrackingSerializer()),
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
                        new ZLinkSpotActorHandlerCatalog(scanned, new TrackingSerializer()),
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
                        new ZLinkSpotActorHandlerCatalog(scanned, new TrackingSerializer()),
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
                        new ZLinkSpotActorHandlerCatalog(scanned, new TrackingSerializer()),
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
