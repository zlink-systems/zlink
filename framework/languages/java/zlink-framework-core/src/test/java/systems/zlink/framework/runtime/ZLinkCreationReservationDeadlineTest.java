package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorMembershipLifecycle;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.Duration;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkCreationReservationDeadlineTest {
    @ParameterizedTest
    @CsvSource({"actor,true", "spot,true", "actor,false", "spot,false"})
    void getOrCreateClassifiesReservationCancellation(String kind, boolean expire) {
        var store = new ReservationStore(kind, expire);
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRouteMesh("deadline")
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addEntrySpot(Entry.class)
                .addActorFactory(
                        "player",
                        Player.class,
                        PlayerFactory.class,
                        factory -> factory.disableRelocation())
                .addSpotFactory("room", Room.class, factory -> factory.disableRelocation());

        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            Duration timeout = expire ? Duration.ofSeconds(1) : Duration.ofSeconds(30);
            CompletionStage<?> call =
                    kind.equals("actor")
                            ? runtime.actorManager()
                                    .getOrCreate("deadline-object", "player")
                                    .timeout(timeout)
                                    .submit()
                            : runtime.spotManager()
                                    .getOrCreate("deadline-object", "room")
                                    .timeout(timeout)
                                    .submit();

            var failure =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    call.toCompletableFuture()
                                            .copy()
                                            .orTimeout(5, TimeUnit.SECONDS)
                                            .join());
            assertTrue(store.reservationAttempted.get(), "the reservation must reach the Store");
            if (expire) {
                assertEquals(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            } else {
                var unavailable =
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
                assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, unavailable.kind());
                assertInstanceOf(CancellationException.class, unavailable.getCause());
            }
        }
    }

    private static final class ReservationStore implements ZLinkLocationStore {
        private final ZLinkLocationStore inner = new ZLinkInMemoryLocationStore();
        private final AtomicBoolean reservationAttempted = new AtomicBoolean();
        private final String authorityKey;
        private final boolean expire;

        ReservationStore(String kind, boolean expire) {
            this.authorityKey = "authority\0" + kind + "\0deadline-object";
            this.expire = expire;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (request.mutations().stream()
                    .noneMatch(
                            mutation ->
                                    mutation instanceof ZLinkStorePut put
                                            && put.key().value().equals(authorityKey))) {
                return inner.write(request, cancellation);
            }
            reservationAttempted.set(true);
            return failWhenCancelled(cancellation);
        }

        private CompletionStage<ZLinkStoreWriteResult> failWhenCancelled(
                ZLinkStoreCancellation cancellation) {
            if (!expire || cancellation.isCancellationRequested()) {
                return CompletableFuture.failedFuture(new CancellationException("Store cancelled"));
            }
            return CompletableFuture.runAsync(
                            () -> {}, CompletableFuture.delayedExecutor(5, TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> failWhenCancelled(cancellation));
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    public record Player(ZLinkActorContext context) implements ZLinkActor {}

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public record Entry(ZLinkEntrySpotContext context)
            implements ZLinkEntrySpot<ZLinkActor>, Membership {}

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<ZLinkActor>, Membership {}

    private interface Membership extends ZLinkSpotActorMembershipLifecycle<ZLinkActor> {
        @Override
        default CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        default CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
