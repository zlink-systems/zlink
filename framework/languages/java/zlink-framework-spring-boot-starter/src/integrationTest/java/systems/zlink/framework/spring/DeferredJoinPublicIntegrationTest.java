package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.core.env.Environment;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Two real hosts, Redis stores, typed nonempty request, empty admission reply. Public APIs only.
 */
public class DeferredJoinPublicIntegrationTest {
    static final AtomicReference<ZLinkActorJoinCompletion> completion = new AtomicReference<>();
    static CountDownLatch completed;
    static CountDownLatch joined;
    static CountDownLatch restoreEntered;
    static CompletableFuture<Void> restoreRelease;

    public record Join(String spotId) {}

    public record Probe() {}

    @ParameterizedTest
    @ValueSource(longs = {0, 1500})
    void typedDeferredJoinAcrossHosts(long restoreDelayMs) throws Exception {
        String redis = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(redis != null && !redis.isBlank(), "Redis integration endpoint is required");
        completion.set(null);
        completed = new CountDownLatch(1);
        joined = new CountDownLatch(1);
        restoreEntered = new CountDownLatch(1);
        restoreRelease = new CompletableFuture<>();
        String prefix = "deferred-join:" + UUID.randomUUID() + ":";
        try (var source = host("source", prefix);
                var target = host("target", prefix)) {
            var s = source.getBean(ZLinkFrameworkRuntime.class);
            var t = target.getBean(ZLinkFrameworkRuntime.class);
            awaitPeer(s, "target");
            awaitPeer(t, "source");
            var room =
                    t.spotManager()
                            .getOrCreate("room", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS);
            assertEquals(RoutingId.from("target"), room.spot().nodeRid());
            var result =
                    s.actorManager()
                            .create("player", "player")
                            .submit()
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS);
            var actor = assertInstanceOf(ZLinkActorCreateResult.Created.class, result).actor();
            assertEquals(RoutingId.from("source"), actor.nodeRid());
            assertEquals(
                    "scheduled",
                    s.actorClient()
                            .requestToActor("player", new Join("room"))
                            .timeout(Duration.ofSeconds(3))
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS));
            assertTrue(restoreEntered.await(3, TimeUnit.SECONDS), "target restore started");
            // Application restore remains within the explicit five-second Join budget.
            // The target's configured one-second preparation window expires during Restore.
            Thread.sleep(restoreDelayMs);
            restoreRelease.complete(null);
            assertTrue(completed.await(3, TimeUnit.SECONDS), "completion callback");
            var accepted =
                    assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, completion.get());
            assertTrue(accepted.reply().isEmpty());
            assertTrue(joined.await(3, TimeUnit.SECONDS), "target joined callback");
            assertEquals(
                    "room",
                    t.actorClient()
                            .requestToActor("player", new Probe())
                            .timeout(Duration.ofSeconds(3))
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS));
        }
    }

    static org.springframework.context.ConfigurableApplicationContext host(
            String role, String prefix) {
        return new SpringApplicationBuilder(Config.class)
                .web(WebApplicationType.NONE)
                .properties(
                        "spring.main.banner-mode=off",
                        "repro.role=" + role,
                        "repro.prefix=" + prefix)
                .run();
    }

    static void awaitPeer(ZLinkFrameworkRuntime r, String peer) throws Exception {
        var ready = new CompletableFuture<Void>();
        var subscription = new AtomicReference<Flow.Subscription>();
        r.routeMeshRuntime()
                .observe("game", 16)
                .subscribe(
                        new Flow.Subscriber<>() {
                            public void onSubscribe(Flow.Subscription s) {
                                subscription.set(s);
                                s.request(Long.MAX_VALUE);
                            }

                            public void onNext(
                                    systems.zlink.framework.monitoring.ZLinkObservedStatus<
                                                    systems.zlink.framework.monitoring
                                                            .ZLinkMeshNodeSnapshot>
                                            o) {
                                if (o.status().readyPeerCount() > 0
                                        && o.status().peers().stream()
                                                .anyMatch(
                                                        p ->
                                                                p.nodeRid()
                                                                                .equals(
                                                                                        RoutingId
                                                                                                .from(
                                                                                                        peer))
                                                                        && p.state()
                                                                                == systems.zlink
                                                                                        .framework
                                                                                        .monitoring
                                                                                        .ZLinkPeerState
                                                                                        .READY))
                                    ready.complete(null);
                            }

                            public void onError(Throwable e) {
                                ready.completeExceptionally(e);
                            }

                            public void onComplete() {}
                        });
        try {
            ready.get(3, TimeUnit.SECONDS);
        } finally {
            if (subscription.get() != null) subscription.get().cancel();
        }
    }

    @Configuration(proxyBeanMethods = false)
    @EnableZLinkFramework
    public static class Config {
        @Bean(destroyMethod = "close")
        ZLinkRedisLocationStore locations(Environment e) {
            return new ZLinkRedisLocationStore(
                    new ZLinkRedisLocationOptions()
                            .setConnectionString(System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT"))
                            .setKeyPrefix(e.getRequiredProperty("repro.prefix") + "locations:"));
        }

        @Bean(destroyMethod = "close")
        ZLinkRedisRelocationStore relocations(Environment e) {
            return new ZLinkRedisRelocationStore(
                    new ZLinkRedisRelocationOptions()
                            .setConnectionString(System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT"))
                            .setKeyPrefix(e.getRequiredProperty("repro.prefix") + "relocation:"));
        }

        @Bean
        ZLinkFrameworkConfigurer framework(
                Environment e, ZLinkRedisLocationStore l, ZLinkRedisRelocationStore r) {
            return o -> {
                o.setDefaultRequestTimeout(Duration.ofSeconds(1));
                o.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                o.addLocationStore(l);
                o.addRelocationStore(r);
                String role = e.getRequiredProperty("repro.role");
                var server =
                        o.addRouteMesh("game")
                                .listen("tcp://127.0.0.1:0")
                                .setRoutingId(RoutingId.from(role))
                                .objects()
                                .server();
                server.addEntrySpot(Entry.class)
                        .addActorFactory(
                                "player",
                                Player.class,
                                Factory.class,
                                f -> f.preserveStateWith(Transfer.class));
                if (role.equals("target"))
                    server.addSpotFactory("room", Room.class, f -> f.disableRelocation());
            };
        }
    }

    public static class Player implements ZLinkActor {
        final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }

        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion c) {
            completion.set(c);
            completed.countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext c) {
            return CompletableFuture.completedFuture(new Player(c));
        }
    }

    public static class Transfer implements ZLinkActorRelocationAdapter<Player> {
        public CompletionStage<byte[]> capture(Player p, ZLinkRelocationCancellation c) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        public CompletionStage<Void> restore(Player p, byte[] b, ZLinkRelocationCancellation c) {
            restoreEntered.countDown();
            return restoreRelease;
        }
    }

    public static class Entry implements ZLinkEntrySpot<Player> {
        final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext c) {
            context = c;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Player p) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player p) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static class Room implements ZLinkSpot<Player> {
        final ZLinkSpotContext context;

        public Room(ZLinkSpotContext c) {
            context = c;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage m) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(Player p) {
            joined.countDown();
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player p) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Join, String> {
        public CompletionStage<String> handle(Entry s, Player p, ZLinkMessageContext c, Join m) {
            p.context().joinSpot(m.spotId(), m).timeout(Duration.ofSeconds(5)).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static class ProbeHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, Probe, String> {
        public CompletionStage<String> handle(Room s, Player p, ZLinkMessageContext c, Probe m) {
            return CompletableFuture.completedFuture(p.context().spotId().orElseThrow());
        }
    }
}
