package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class ZLinkDynamicSubscriptionTest {
    private static final int WAIT_SECONDS = 5;

    @Test
    void dispatchPreservesChannelAndRuntimeTopicAlongsideAnnotation() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("game");
        node.listen("tcp://127.0.0.1:0");
        node.channelName("events").server();
        node.channelName("other").server();
        node.objects()
                .server()
                .addSpotFactory(
                        "Subscriber", Subscriber.class, factory -> factory.disableRelocation());
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            runtime.spotManager()
                    .create("Subscriber")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            var spot = Subscriber.created;
            var context = spot.context();
            context.outbound()
                    .publish("other", spot.topic, "other")
                    .submit()
                    .toCompletableFuture()
                    .join();
            spot.other.get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertFalse(spot.dynamic.isDone());
            context.outbound()
                    .publish("events", spot.topic, "dynamic")
                    .submit()
                    .toCompletableFuture()
                    .join();
            assertEquals("dynamic", spot.dynamic.get(WAIT_SECONDS, TimeUnit.SECONDS));
            assertFalse(spot.annotated.isDone());
            context.outbound()
                    .publish("events", "annotated", "annotation")
                    .submit()
                    .toCompletableFuture()
                    .join();
            assertEquals("annotation", spot.annotated.get(WAIT_SECONDS, TimeUnit.SECONDS));
        }
    }

    public static final class Subscriber implements ZLinkSpot<ZLinkActor> {
        static volatile Subscriber created;
        final ZLinkSpotContext context;
        final String topic;
        final CompletableFuture<String> dynamic = new CompletableFuture<>();
        final CompletableFuture<String> other = new CompletableFuture<>();
        final CompletableFuture<String> annotated = new CompletableFuture<>();

        public Subscriber(ZLinkSpotContext context) {
            this.context = context;
            topic = "runtime." + context.spotId();
            created = this;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addSubscribe(EventHandler.class, "events", topic);
            context.handlers().addSubscribe(OtherHandler.class, "other", topic);
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    @ZLinkSpotSubscription(spotNodeName = "game", channelName = "events", topic = "annotated")
    public static final class EventHandler
            implements ZLinkSpotSubscriptionHandler<Subscriber, String> {
        public CompletionStage<Void> handle(Subscriber spot, String message) {
            if (message.equals("annotation")) spot.annotated.complete(message);
            else spot.dynamic.complete(message);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class OtherHandler
            implements ZLinkSpotSubscriptionHandler<Subscriber, String> {
        public CompletionStage<Void> handle(Subscriber spot, String message) {
            spot.other.complete(message);
            return CompletableFuture.completedFuture(null);
        }
    }
}
