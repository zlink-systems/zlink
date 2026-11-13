package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;

public final class ZLinkSpotActorHandlerConcurrentActivationTest {
    private static final int WAIT = 10;
    private static final int SPOT_TYPES = 48;
    private static final int HANDLERS_PER_SPOT = 3;
    private static final int CREATORS = 16;
    private static CompletableFuture<ZLinkActorJoinCompletion> joinCompleted;

    @RepeatedTest(3)
    void allFirstSpotActivationsMustRetainEveryActorHandler() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("game");
        node.listen("inproc://actor-handlers-" + UUID.randomUUID());
        var objects = node.objects().server();
        objects.addEntrySpot(Lobby.class);
        objects.addActorFactory(
                "player", Player.class, PlayerFactory.class, f -> f.disableRelocation());
        for (int k = 0; k < SPOT_TYPES; k++) {
            @SuppressWarnings("unchecked")
            Class<? extends ZLinkSpot<?>> spotType =
                    (Class<? extends ZLinkSpot<?>>)
                            Class.forName(getClass().getName() + "$Spot" + k);
            objects.addSpotFactory("s" + k, spotType, f -> f.disableRelocation());
        }
        var pool = Executors.newFixedThreadPool(CREATORS);
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            var go = new CountDownLatch(1);
            var ready = new CountDownLatch(CREATORS);
            List<Future<?>> creates = new ArrayList<>();
            for (int k = 0; k < SPOT_TYPES; k++) {
                int type = k;
                creates.add(
                        pool.submit(
                                () -> {
                                    ready.countDown();
                                    assertTrue(go.await(WAIT, TimeUnit.SECONDS));
                                    runtime.spotManager()
                                            .getOrCreate("spot-" + type, "s" + type)
                                            .submit()
                                            .toCompletableFuture()
                                            .get(WAIT, TimeUnit.SECONDS);
                                    return null;
                                }));
            }
            assertTrue(ready.await(WAIT, TimeUnit.SECONDS));
            go.countDown();
            assertAll(creates.stream().map(create -> () -> create.get(WAIT, TimeUnit.SECONDS)));
            for (int k = 0; k < SPOT_TYPES; k++) {
                String actor = "actor-" + k;
                joinCompleted = new CompletableFuture<>();
                runtime.actorManager()
                        .create(actor, "player")
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT, TimeUnit.SECONDS);
                runtime.actorClient()
                        .requestToActor(actor, new Join("spot-" + k))
                        .timeout(Duration.ofSeconds(WAIT))
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(WAIT, TimeUnit.SECONDS);
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        joinCompleted.get(WAIT, TimeUnit.SECONDS));
                for (int j = 0; j < HANDLERS_PER_SPOT; j++) {
                    Object request =
                            Class.forName(getClass().getName() + "$Req" + k + "x" + j)
                                    .getConstructor(int.class)
                                    .newInstance(42);
                    var reply =
                            runtime.actorClient()
                                    .requestToActor(actor, request)
                                    .timeout(Duration.ofSeconds(WAIT))
                                    .submit(Reply.class)
                                    .toCompletableFuture()
                                    .get(WAIT, TimeUnit.SECONDS);
                    assertEquals(new Reply(k, j, 42), reply);
                }
            }
        } finally {
            pool.shutdownNow();
            assertTrue(pool.awaitTermination(WAIT, TimeUnit.SECONDS));
        }
    }

    @Test
    void sameSpotTypeInstancesDispatchThroughTheirOwnConfiguredHandlers() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("game");
        node.listen("inproc://instance-actor-handlers-" + UUID.randomUUID());
        var objects = node.objects().server();
        objects.addEntrySpot(Lobby.class);
        objects.addActorFactory(
                "player", Player.class, PlayerFactory.class, f -> f.disableRelocation());
        objects.addSpotFactory("conditional", ConditionalSpot.class, f -> f.disableRelocation());
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            for (String spotId : List.of("first", "second")) {
                runtime.spotManager()
                        .getOrCreate(spotId, "conditional")
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT, TimeUnit.SECONDS);
            }
            for (String spotId : List.of("first", "second", "first")) {
                String actor = "actor-" + UUID.randomUUID();
                joinCompleted = new CompletableFuture<>();
                runtime.actorManager()
                        .create(actor, "player")
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT, TimeUnit.SECONDS);
                runtime.actorClient()
                        .requestToActor(actor, new Join(spotId))
                        .timeout(Duration.ofSeconds(WAIT))
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(WAIT, TimeUnit.SECONDS);
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        joinCompleted.get(WAIT, TimeUnit.SECONDS));
                var reply =
                        runtime.actorClient()
                                .requestToActor(actor, new InstanceRequest(42))
                                .timeout(Duration.ofSeconds(WAIT))
                                .submit(Reply.class)
                                .toCompletableFuture()
                                .get(WAIT, TimeUnit.SECONDS);
                assertEquals(new Reply(spotId.equals("first") ? 1 : 2, 0, 42), reply);
            }
        }
    }

    public record InstanceRequest(int value) {}

    public static final class ConditionalSpot implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;

        public ConditionalSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers()
                    .addHandler(
                            context.spotId().equals("first")
                                    ? FirstInstanceHandler.class
                                    : SecondInstanceHandler.class);
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String id, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class FirstInstanceHandler
            implements ZLinkSpotActorRequestHandler<
                    ConditionalSpot, Player, InstanceRequest, Reply> {
        public CompletionStage<Reply> handle(
                ConditionalSpot spot,
                Player actor,
                ZLinkMessageContext context,
                InstanceRequest request) {
            return CompletableFuture.completedFuture(new Reply(1, 0, request.value()));
        }
    }

    public static final class SecondInstanceHandler
            implements ZLinkSpotActorRequestHandler<
                    ConditionalSpot, Player, InstanceRequest, Reply> {
        public CompletionStage<Reply> handle(
                ConditionalSpot spot,
                Player actor,
                ZLinkMessageContext context,
                InstanceRequest request) {
            return CompletableFuture.completedFuture(new Reply(2, 0, request.value()));
        }
    }

    public record Join(String spotId) {}

    public record Reply(int type, int handler, int value) {}

    public static final class Player implements ZLinkActor {
        final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }

        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            joinCompleted.complete(result);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Lobby implements ZLinkEntrySpot<Player> {
        final ZLinkEntrySpotContext context;

        public Lobby(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, Join, String> {
        public CompletionStage<String> handle(
                Lobby spot, Player actor, ZLinkMessageContext ctx, Join request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    private abstract static class TypedSpot implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;
        private final Class<?> handlerType;

        TypedSpot(ZLinkSpotContext context, Class<?> handlerType) {
            this.context = context;
            this.handlerType = handlerType;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(handlerType);
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String id, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static Reply reply(int type, int handler, int value) {
        return new Reply(type, handler, value);
    }

    public static final class Spot0 extends TypedSpot {
        public Spot0(ZLinkSpotContext context) {
            super(context, Handler0.class);
        }
    }

    public static final class Spot1 extends TypedSpot {
        public Spot1(ZLinkSpotContext context) {
            super(context, Handler1.class);
        }
    }

    public static final class Spot2 extends TypedSpot {
        public Spot2(ZLinkSpotContext context) {
            super(context, Handler2.class);
        }
    }

    public static final class Spot3 extends TypedSpot {
        public Spot3(ZLinkSpotContext context) {
            super(context, Handler3.class);
        }
    }

    public static final class Spot4 extends TypedSpot {
        public Spot4(ZLinkSpotContext context) {
            super(context, Handler4.class);
        }
    }

    public static final class Spot5 extends TypedSpot {
        public Spot5(ZLinkSpotContext context) {
            super(context, Handler5.class);
        }
    }

    public static final class Spot6 extends TypedSpot {
        public Spot6(ZLinkSpotContext context) {
            super(context, Handler6.class);
        }
    }

    public static final class Spot7 extends TypedSpot {
        public Spot7(ZLinkSpotContext context) {
            super(context, Handler7.class);
        }
    }

    public static final class Spot8 extends TypedSpot {
        public Spot8(ZLinkSpotContext context) {
            super(context, Handler8.class);
        }
    }

    public static final class Spot9 extends TypedSpot {
        public Spot9(ZLinkSpotContext context) {
            super(context, Handler9.class);
        }
    }

    public static final class Spot10 extends TypedSpot {
        public Spot10(ZLinkSpotContext context) {
            super(context, Handler10.class);
        }
    }

    public static final class Spot11 extends TypedSpot {
        public Spot11(ZLinkSpotContext context) {
            super(context, Handler11.class);
        }
    }

    public static final class Spot12 extends TypedSpot {
        public Spot12(ZLinkSpotContext context) {
            super(context, Handler12.class);
        }
    }

    public static final class Spot13 extends TypedSpot {
        public Spot13(ZLinkSpotContext context) {
            super(context, Handler13.class);
        }
    }

    public static final class Spot14 extends TypedSpot {
        public Spot14(ZLinkSpotContext context) {
            super(context, Handler14.class);
        }
    }

    public static final class Spot15 extends TypedSpot {
        public Spot15(ZLinkSpotContext context) {
            super(context, Handler15.class);
        }
    }

    public static final class Spot16 extends TypedSpot {
        public Spot16(ZLinkSpotContext context) {
            super(context, Handler16.class);
        }
    }

    public static final class Spot17 extends TypedSpot {
        public Spot17(ZLinkSpotContext context) {
            super(context, Handler17.class);
        }
    }

    public static final class Spot18 extends TypedSpot {
        public Spot18(ZLinkSpotContext context) {
            super(context, Handler18.class);
        }
    }

    public static final class Spot19 extends TypedSpot {
        public Spot19(ZLinkSpotContext context) {
            super(context, Handler19.class);
        }
    }

    public static final class Spot20 extends TypedSpot {
        public Spot20(ZLinkSpotContext context) {
            super(context, Handler20.class);
        }
    }

    public static final class Spot21 extends TypedSpot {
        public Spot21(ZLinkSpotContext context) {
            super(context, Handler21.class);
        }
    }

    public static final class Spot22 extends TypedSpot {
        public Spot22(ZLinkSpotContext context) {
            super(context, Handler22.class);
        }
    }

    public static final class Spot23 extends TypedSpot {
        public Spot23(ZLinkSpotContext context) {
            super(context, Handler23.class);
        }
    }

    public static final class Spot24 extends TypedSpot {
        public Spot24(ZLinkSpotContext context) {
            super(context, Handler24.class);
        }
    }

    public static final class Spot25 extends TypedSpot {
        public Spot25(ZLinkSpotContext context) {
            super(context, Handler25.class);
        }
    }

    public static final class Spot26 extends TypedSpot {
        public Spot26(ZLinkSpotContext context) {
            super(context, Handler26.class);
        }
    }

    public static final class Spot27 extends TypedSpot {
        public Spot27(ZLinkSpotContext context) {
            super(context, Handler27.class);
        }
    }

    public static final class Spot28 extends TypedSpot {
        public Spot28(ZLinkSpotContext context) {
            super(context, Handler28.class);
        }
    }

    public static final class Spot29 extends TypedSpot {
        public Spot29(ZLinkSpotContext context) {
            super(context, Handler29.class);
        }
    }

    public static final class Spot30 extends TypedSpot {
        public Spot30(ZLinkSpotContext context) {
            super(context, Handler30.class);
        }
    }

    public static final class Spot31 extends TypedSpot {
        public Spot31(ZLinkSpotContext context) {
            super(context, Handler31.class);
        }
    }

    public static final class Spot32 extends TypedSpot {
        public Spot32(ZLinkSpotContext context) {
            super(context, Handler32.class);
        }
    }

    public static final class Spot33 extends TypedSpot {
        public Spot33(ZLinkSpotContext context) {
            super(context, Handler33.class);
        }
    }

    public static final class Spot34 extends TypedSpot {
        public Spot34(ZLinkSpotContext context) {
            super(context, Handler34.class);
        }
    }

    public static final class Spot35 extends TypedSpot {
        public Spot35(ZLinkSpotContext context) {
            super(context, Handler35.class);
        }
    }

    public static final class Spot36 extends TypedSpot {
        public Spot36(ZLinkSpotContext context) {
            super(context, Handler36.class);
        }
    }

    public static final class Spot37 extends TypedSpot {
        public Spot37(ZLinkSpotContext context) {
            super(context, Handler37.class);
        }
    }

    public static final class Spot38 extends TypedSpot {
        public Spot38(ZLinkSpotContext context) {
            super(context, Handler38.class);
        }
    }

    public static final class Spot39 extends TypedSpot {
        public Spot39(ZLinkSpotContext context) {
            super(context, Handler39.class);
        }
    }

    public static final class Spot40 extends TypedSpot {
        public Spot40(ZLinkSpotContext context) {
            super(context, Handler40.class);
        }
    }

    public static final class Spot41 extends TypedSpot {
        public Spot41(ZLinkSpotContext context) {
            super(context, Handler41.class);
        }
    }

    public static final class Spot42 extends TypedSpot {
        public Spot42(ZLinkSpotContext context) {
            super(context, Handler42.class);
        }
    }

    public static final class Spot43 extends TypedSpot {
        public Spot43(ZLinkSpotContext context) {
            super(context, Handler43.class);
        }
    }

    public static final class Spot44 extends TypedSpot {
        public Spot44(ZLinkSpotContext context) {
            super(context, Handler44.class);
        }
    }

    public static final class Spot45 extends TypedSpot {
        public Spot45(ZLinkSpotContext context) {
            super(context, Handler45.class);
        }
    }

    public static final class Spot46 extends TypedSpot {
        public Spot46(ZLinkSpotContext context) {
            super(context, Handler46.class);
        }
    }

    public static final class Spot47 extends TypedSpot {
        public Spot47(ZLinkSpotContext context) {
            super(context, Handler47.class);
        }
    }

    public record Req0x0(int value) {}

    public record Req0x1(int value) {}

    public record Req0x2(int value) {}

    public static final class Handler0 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot0 spot, Player actor, ZLinkMessageContext ctx, Req0x0 req) {
            return CompletableFuture.completedFuture(reply(0, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot0 spot, Player actor, ZLinkMessageContext ctx, Req0x1 req) {
            return CompletableFuture.completedFuture(reply(0, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot0 spot, Player actor, ZLinkMessageContext ctx, Req0x2 req) {
            return CompletableFuture.completedFuture(reply(0, 2, req.value()));
        }
    }

    public record Req1x0(int value) {}

    public record Req1x1(int value) {}

    public record Req1x2(int value) {}

    public static final class Handler1 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot1 spot, Player actor, ZLinkMessageContext ctx, Req1x0 req) {
            return CompletableFuture.completedFuture(reply(1, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot1 spot, Player actor, ZLinkMessageContext ctx, Req1x1 req) {
            return CompletableFuture.completedFuture(reply(1, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot1 spot, Player actor, ZLinkMessageContext ctx, Req1x2 req) {
            return CompletableFuture.completedFuture(reply(1, 2, req.value()));
        }
    }

    public record Req2x0(int value) {}

    public record Req2x1(int value) {}

    public record Req2x2(int value) {}

    public static final class Handler2 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot2 spot, Player actor, ZLinkMessageContext ctx, Req2x0 req) {
            return CompletableFuture.completedFuture(reply(2, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot2 spot, Player actor, ZLinkMessageContext ctx, Req2x1 req) {
            return CompletableFuture.completedFuture(reply(2, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot2 spot, Player actor, ZLinkMessageContext ctx, Req2x2 req) {
            return CompletableFuture.completedFuture(reply(2, 2, req.value()));
        }
    }

    public record Req3x0(int value) {}

    public record Req3x1(int value) {}

    public record Req3x2(int value) {}

    public static final class Handler3 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot3 spot, Player actor, ZLinkMessageContext ctx, Req3x0 req) {
            return CompletableFuture.completedFuture(reply(3, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot3 spot, Player actor, ZLinkMessageContext ctx, Req3x1 req) {
            return CompletableFuture.completedFuture(reply(3, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot3 spot, Player actor, ZLinkMessageContext ctx, Req3x2 req) {
            return CompletableFuture.completedFuture(reply(3, 2, req.value()));
        }
    }

    public record Req4x0(int value) {}

    public record Req4x1(int value) {}

    public record Req4x2(int value) {}

    public static final class Handler4 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot4 spot, Player actor, ZLinkMessageContext ctx, Req4x0 req) {
            return CompletableFuture.completedFuture(reply(4, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot4 spot, Player actor, ZLinkMessageContext ctx, Req4x1 req) {
            return CompletableFuture.completedFuture(reply(4, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot4 spot, Player actor, ZLinkMessageContext ctx, Req4x2 req) {
            return CompletableFuture.completedFuture(reply(4, 2, req.value()));
        }
    }

    public record Req5x0(int value) {}

    public record Req5x1(int value) {}

    public record Req5x2(int value) {}

    public static final class Handler5 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot5 spot, Player actor, ZLinkMessageContext ctx, Req5x0 req) {
            return CompletableFuture.completedFuture(reply(5, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot5 spot, Player actor, ZLinkMessageContext ctx, Req5x1 req) {
            return CompletableFuture.completedFuture(reply(5, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot5 spot, Player actor, ZLinkMessageContext ctx, Req5x2 req) {
            return CompletableFuture.completedFuture(reply(5, 2, req.value()));
        }
    }

    public record Req6x0(int value) {}

    public record Req6x1(int value) {}

    public record Req6x2(int value) {}

    public static final class Handler6 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot6 spot, Player actor, ZLinkMessageContext ctx, Req6x0 req) {
            return CompletableFuture.completedFuture(reply(6, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot6 spot, Player actor, ZLinkMessageContext ctx, Req6x1 req) {
            return CompletableFuture.completedFuture(reply(6, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot6 spot, Player actor, ZLinkMessageContext ctx, Req6x2 req) {
            return CompletableFuture.completedFuture(reply(6, 2, req.value()));
        }
    }

    public record Req7x0(int value) {}

    public record Req7x1(int value) {}

    public record Req7x2(int value) {}

    public static final class Handler7 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot7 spot, Player actor, ZLinkMessageContext ctx, Req7x0 req) {
            return CompletableFuture.completedFuture(reply(7, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot7 spot, Player actor, ZLinkMessageContext ctx, Req7x1 req) {
            return CompletableFuture.completedFuture(reply(7, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot7 spot, Player actor, ZLinkMessageContext ctx, Req7x2 req) {
            return CompletableFuture.completedFuture(reply(7, 2, req.value()));
        }
    }

    public record Req8x0(int value) {}

    public record Req8x1(int value) {}

    public record Req8x2(int value) {}

    public static final class Handler8 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot8 spot, Player actor, ZLinkMessageContext ctx, Req8x0 req) {
            return CompletableFuture.completedFuture(reply(8, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot8 spot, Player actor, ZLinkMessageContext ctx, Req8x1 req) {
            return CompletableFuture.completedFuture(reply(8, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot8 spot, Player actor, ZLinkMessageContext ctx, Req8x2 req) {
            return CompletableFuture.completedFuture(reply(8, 2, req.value()));
        }
    }

    public record Req9x0(int value) {}

    public record Req9x1(int value) {}

    public record Req9x2(int value) {}

    public static final class Handler9 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot9 spot, Player actor, ZLinkMessageContext ctx, Req9x0 req) {
            return CompletableFuture.completedFuture(reply(9, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot9 spot, Player actor, ZLinkMessageContext ctx, Req9x1 req) {
            return CompletableFuture.completedFuture(reply(9, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot9 spot, Player actor, ZLinkMessageContext ctx, Req9x2 req) {
            return CompletableFuture.completedFuture(reply(9, 2, req.value()));
        }
    }

    public record Req10x0(int value) {}

    public record Req10x1(int value) {}

    public record Req10x2(int value) {}

    public static final class Handler10 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot10 spot, Player actor, ZLinkMessageContext ctx, Req10x0 req) {
            return CompletableFuture.completedFuture(reply(10, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot10 spot, Player actor, ZLinkMessageContext ctx, Req10x1 req) {
            return CompletableFuture.completedFuture(reply(10, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot10 spot, Player actor, ZLinkMessageContext ctx, Req10x2 req) {
            return CompletableFuture.completedFuture(reply(10, 2, req.value()));
        }
    }

    public record Req11x0(int value) {}

    public record Req11x1(int value) {}

    public record Req11x2(int value) {}

    public static final class Handler11 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot11 spot, Player actor, ZLinkMessageContext ctx, Req11x0 req) {
            return CompletableFuture.completedFuture(reply(11, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot11 spot, Player actor, ZLinkMessageContext ctx, Req11x1 req) {
            return CompletableFuture.completedFuture(reply(11, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot11 spot, Player actor, ZLinkMessageContext ctx, Req11x2 req) {
            return CompletableFuture.completedFuture(reply(11, 2, req.value()));
        }
    }

    public record Req12x0(int value) {}

    public record Req12x1(int value) {}

    public record Req12x2(int value) {}

    public static final class Handler12 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot12 spot, Player actor, ZLinkMessageContext ctx, Req12x0 req) {
            return CompletableFuture.completedFuture(reply(12, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot12 spot, Player actor, ZLinkMessageContext ctx, Req12x1 req) {
            return CompletableFuture.completedFuture(reply(12, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot12 spot, Player actor, ZLinkMessageContext ctx, Req12x2 req) {
            return CompletableFuture.completedFuture(reply(12, 2, req.value()));
        }
    }

    public record Req13x0(int value) {}

    public record Req13x1(int value) {}

    public record Req13x2(int value) {}

    public static final class Handler13 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot13 spot, Player actor, ZLinkMessageContext ctx, Req13x0 req) {
            return CompletableFuture.completedFuture(reply(13, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot13 spot, Player actor, ZLinkMessageContext ctx, Req13x1 req) {
            return CompletableFuture.completedFuture(reply(13, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot13 spot, Player actor, ZLinkMessageContext ctx, Req13x2 req) {
            return CompletableFuture.completedFuture(reply(13, 2, req.value()));
        }
    }

    public record Req14x0(int value) {}

    public record Req14x1(int value) {}

    public record Req14x2(int value) {}

    public static final class Handler14 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot14 spot, Player actor, ZLinkMessageContext ctx, Req14x0 req) {
            return CompletableFuture.completedFuture(reply(14, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot14 spot, Player actor, ZLinkMessageContext ctx, Req14x1 req) {
            return CompletableFuture.completedFuture(reply(14, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot14 spot, Player actor, ZLinkMessageContext ctx, Req14x2 req) {
            return CompletableFuture.completedFuture(reply(14, 2, req.value()));
        }
    }

    public record Req15x0(int value) {}

    public record Req15x1(int value) {}

    public record Req15x2(int value) {}

    public static final class Handler15 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot15 spot, Player actor, ZLinkMessageContext ctx, Req15x0 req) {
            return CompletableFuture.completedFuture(reply(15, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot15 spot, Player actor, ZLinkMessageContext ctx, Req15x1 req) {
            return CompletableFuture.completedFuture(reply(15, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot15 spot, Player actor, ZLinkMessageContext ctx, Req15x2 req) {
            return CompletableFuture.completedFuture(reply(15, 2, req.value()));
        }
    }

    public record Req16x0(int value) {}

    public record Req16x1(int value) {}

    public record Req16x2(int value) {}

    public static final class Handler16 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot16 spot, Player actor, ZLinkMessageContext ctx, Req16x0 req) {
            return CompletableFuture.completedFuture(reply(16, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot16 spot, Player actor, ZLinkMessageContext ctx, Req16x1 req) {
            return CompletableFuture.completedFuture(reply(16, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot16 spot, Player actor, ZLinkMessageContext ctx, Req16x2 req) {
            return CompletableFuture.completedFuture(reply(16, 2, req.value()));
        }
    }

    public record Req17x0(int value) {}

    public record Req17x1(int value) {}

    public record Req17x2(int value) {}

    public static final class Handler17 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot17 spot, Player actor, ZLinkMessageContext ctx, Req17x0 req) {
            return CompletableFuture.completedFuture(reply(17, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot17 spot, Player actor, ZLinkMessageContext ctx, Req17x1 req) {
            return CompletableFuture.completedFuture(reply(17, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot17 spot, Player actor, ZLinkMessageContext ctx, Req17x2 req) {
            return CompletableFuture.completedFuture(reply(17, 2, req.value()));
        }
    }

    public record Req18x0(int value) {}

    public record Req18x1(int value) {}

    public record Req18x2(int value) {}

    public static final class Handler18 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot18 spot, Player actor, ZLinkMessageContext ctx, Req18x0 req) {
            return CompletableFuture.completedFuture(reply(18, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot18 spot, Player actor, ZLinkMessageContext ctx, Req18x1 req) {
            return CompletableFuture.completedFuture(reply(18, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot18 spot, Player actor, ZLinkMessageContext ctx, Req18x2 req) {
            return CompletableFuture.completedFuture(reply(18, 2, req.value()));
        }
    }

    public record Req19x0(int value) {}

    public record Req19x1(int value) {}

    public record Req19x2(int value) {}

    public static final class Handler19 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot19 spot, Player actor, ZLinkMessageContext ctx, Req19x0 req) {
            return CompletableFuture.completedFuture(reply(19, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot19 spot, Player actor, ZLinkMessageContext ctx, Req19x1 req) {
            return CompletableFuture.completedFuture(reply(19, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot19 spot, Player actor, ZLinkMessageContext ctx, Req19x2 req) {
            return CompletableFuture.completedFuture(reply(19, 2, req.value()));
        }
    }

    public record Req20x0(int value) {}

    public record Req20x1(int value) {}

    public record Req20x2(int value) {}

    public static final class Handler20 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot20 spot, Player actor, ZLinkMessageContext ctx, Req20x0 req) {
            return CompletableFuture.completedFuture(reply(20, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot20 spot, Player actor, ZLinkMessageContext ctx, Req20x1 req) {
            return CompletableFuture.completedFuture(reply(20, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot20 spot, Player actor, ZLinkMessageContext ctx, Req20x2 req) {
            return CompletableFuture.completedFuture(reply(20, 2, req.value()));
        }
    }

    public record Req21x0(int value) {}

    public record Req21x1(int value) {}

    public record Req21x2(int value) {}

    public static final class Handler21 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot21 spot, Player actor, ZLinkMessageContext ctx, Req21x0 req) {
            return CompletableFuture.completedFuture(reply(21, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot21 spot, Player actor, ZLinkMessageContext ctx, Req21x1 req) {
            return CompletableFuture.completedFuture(reply(21, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot21 spot, Player actor, ZLinkMessageContext ctx, Req21x2 req) {
            return CompletableFuture.completedFuture(reply(21, 2, req.value()));
        }
    }

    public record Req22x0(int value) {}

    public record Req22x1(int value) {}

    public record Req22x2(int value) {}

    public static final class Handler22 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot22 spot, Player actor, ZLinkMessageContext ctx, Req22x0 req) {
            return CompletableFuture.completedFuture(reply(22, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot22 spot, Player actor, ZLinkMessageContext ctx, Req22x1 req) {
            return CompletableFuture.completedFuture(reply(22, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot22 spot, Player actor, ZLinkMessageContext ctx, Req22x2 req) {
            return CompletableFuture.completedFuture(reply(22, 2, req.value()));
        }
    }

    public record Req23x0(int value) {}

    public record Req23x1(int value) {}

    public record Req23x2(int value) {}

    public static final class Handler23 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot23 spot, Player actor, ZLinkMessageContext ctx, Req23x0 req) {
            return CompletableFuture.completedFuture(reply(23, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot23 spot, Player actor, ZLinkMessageContext ctx, Req23x1 req) {
            return CompletableFuture.completedFuture(reply(23, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot23 spot, Player actor, ZLinkMessageContext ctx, Req23x2 req) {
            return CompletableFuture.completedFuture(reply(23, 2, req.value()));
        }
    }

    public record Req24x0(int value) {}

    public record Req24x1(int value) {}

    public record Req24x2(int value) {}

    public static final class Handler24 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot24 spot, Player actor, ZLinkMessageContext ctx, Req24x0 req) {
            return CompletableFuture.completedFuture(reply(24, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot24 spot, Player actor, ZLinkMessageContext ctx, Req24x1 req) {
            return CompletableFuture.completedFuture(reply(24, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot24 spot, Player actor, ZLinkMessageContext ctx, Req24x2 req) {
            return CompletableFuture.completedFuture(reply(24, 2, req.value()));
        }
    }

    public record Req25x0(int value) {}

    public record Req25x1(int value) {}

    public record Req25x2(int value) {}

    public static final class Handler25 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot25 spot, Player actor, ZLinkMessageContext ctx, Req25x0 req) {
            return CompletableFuture.completedFuture(reply(25, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot25 spot, Player actor, ZLinkMessageContext ctx, Req25x1 req) {
            return CompletableFuture.completedFuture(reply(25, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot25 spot, Player actor, ZLinkMessageContext ctx, Req25x2 req) {
            return CompletableFuture.completedFuture(reply(25, 2, req.value()));
        }
    }

    public record Req26x0(int value) {}

    public record Req26x1(int value) {}

    public record Req26x2(int value) {}

    public static final class Handler26 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot26 spot, Player actor, ZLinkMessageContext ctx, Req26x0 req) {
            return CompletableFuture.completedFuture(reply(26, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot26 spot, Player actor, ZLinkMessageContext ctx, Req26x1 req) {
            return CompletableFuture.completedFuture(reply(26, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot26 spot, Player actor, ZLinkMessageContext ctx, Req26x2 req) {
            return CompletableFuture.completedFuture(reply(26, 2, req.value()));
        }
    }

    public record Req27x0(int value) {}

    public record Req27x1(int value) {}

    public record Req27x2(int value) {}

    public static final class Handler27 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot27 spot, Player actor, ZLinkMessageContext ctx, Req27x0 req) {
            return CompletableFuture.completedFuture(reply(27, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot27 spot, Player actor, ZLinkMessageContext ctx, Req27x1 req) {
            return CompletableFuture.completedFuture(reply(27, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot27 spot, Player actor, ZLinkMessageContext ctx, Req27x2 req) {
            return CompletableFuture.completedFuture(reply(27, 2, req.value()));
        }
    }

    public record Req28x0(int value) {}

    public record Req28x1(int value) {}

    public record Req28x2(int value) {}

    public static final class Handler28 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot28 spot, Player actor, ZLinkMessageContext ctx, Req28x0 req) {
            return CompletableFuture.completedFuture(reply(28, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot28 spot, Player actor, ZLinkMessageContext ctx, Req28x1 req) {
            return CompletableFuture.completedFuture(reply(28, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot28 spot, Player actor, ZLinkMessageContext ctx, Req28x2 req) {
            return CompletableFuture.completedFuture(reply(28, 2, req.value()));
        }
    }

    public record Req29x0(int value) {}

    public record Req29x1(int value) {}

    public record Req29x2(int value) {}

    public static final class Handler29 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot29 spot, Player actor, ZLinkMessageContext ctx, Req29x0 req) {
            return CompletableFuture.completedFuture(reply(29, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot29 spot, Player actor, ZLinkMessageContext ctx, Req29x1 req) {
            return CompletableFuture.completedFuture(reply(29, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot29 spot, Player actor, ZLinkMessageContext ctx, Req29x2 req) {
            return CompletableFuture.completedFuture(reply(29, 2, req.value()));
        }
    }

    public record Req30x0(int value) {}

    public record Req30x1(int value) {}

    public record Req30x2(int value) {}

    public static final class Handler30 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot30 spot, Player actor, ZLinkMessageContext ctx, Req30x0 req) {
            return CompletableFuture.completedFuture(reply(30, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot30 spot, Player actor, ZLinkMessageContext ctx, Req30x1 req) {
            return CompletableFuture.completedFuture(reply(30, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot30 spot, Player actor, ZLinkMessageContext ctx, Req30x2 req) {
            return CompletableFuture.completedFuture(reply(30, 2, req.value()));
        }
    }

    public record Req31x0(int value) {}

    public record Req31x1(int value) {}

    public record Req31x2(int value) {}

    public static final class Handler31 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot31 spot, Player actor, ZLinkMessageContext ctx, Req31x0 req) {
            return CompletableFuture.completedFuture(reply(31, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot31 spot, Player actor, ZLinkMessageContext ctx, Req31x1 req) {
            return CompletableFuture.completedFuture(reply(31, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot31 spot, Player actor, ZLinkMessageContext ctx, Req31x2 req) {
            return CompletableFuture.completedFuture(reply(31, 2, req.value()));
        }
    }

    public record Req32x0(int value) {}

    public record Req32x1(int value) {}

    public record Req32x2(int value) {}

    public static final class Handler32 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot32 spot, Player actor, ZLinkMessageContext ctx, Req32x0 req) {
            return CompletableFuture.completedFuture(reply(32, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot32 spot, Player actor, ZLinkMessageContext ctx, Req32x1 req) {
            return CompletableFuture.completedFuture(reply(32, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot32 spot, Player actor, ZLinkMessageContext ctx, Req32x2 req) {
            return CompletableFuture.completedFuture(reply(32, 2, req.value()));
        }
    }

    public record Req33x0(int value) {}

    public record Req33x1(int value) {}

    public record Req33x2(int value) {}

    public static final class Handler33 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot33 spot, Player actor, ZLinkMessageContext ctx, Req33x0 req) {
            return CompletableFuture.completedFuture(reply(33, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot33 spot, Player actor, ZLinkMessageContext ctx, Req33x1 req) {
            return CompletableFuture.completedFuture(reply(33, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot33 spot, Player actor, ZLinkMessageContext ctx, Req33x2 req) {
            return CompletableFuture.completedFuture(reply(33, 2, req.value()));
        }
    }

    public record Req34x0(int value) {}

    public record Req34x1(int value) {}

    public record Req34x2(int value) {}

    public static final class Handler34 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot34 spot, Player actor, ZLinkMessageContext ctx, Req34x0 req) {
            return CompletableFuture.completedFuture(reply(34, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot34 spot, Player actor, ZLinkMessageContext ctx, Req34x1 req) {
            return CompletableFuture.completedFuture(reply(34, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot34 spot, Player actor, ZLinkMessageContext ctx, Req34x2 req) {
            return CompletableFuture.completedFuture(reply(34, 2, req.value()));
        }
    }

    public record Req35x0(int value) {}

    public record Req35x1(int value) {}

    public record Req35x2(int value) {}

    public static final class Handler35 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot35 spot, Player actor, ZLinkMessageContext ctx, Req35x0 req) {
            return CompletableFuture.completedFuture(reply(35, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot35 spot, Player actor, ZLinkMessageContext ctx, Req35x1 req) {
            return CompletableFuture.completedFuture(reply(35, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot35 spot, Player actor, ZLinkMessageContext ctx, Req35x2 req) {
            return CompletableFuture.completedFuture(reply(35, 2, req.value()));
        }
    }

    public record Req36x0(int value) {}

    public record Req36x1(int value) {}

    public record Req36x2(int value) {}

    public static final class Handler36 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot36 spot, Player actor, ZLinkMessageContext ctx, Req36x0 req) {
            return CompletableFuture.completedFuture(reply(36, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot36 spot, Player actor, ZLinkMessageContext ctx, Req36x1 req) {
            return CompletableFuture.completedFuture(reply(36, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot36 spot, Player actor, ZLinkMessageContext ctx, Req36x2 req) {
            return CompletableFuture.completedFuture(reply(36, 2, req.value()));
        }
    }

    public record Req37x0(int value) {}

    public record Req37x1(int value) {}

    public record Req37x2(int value) {}

    public static final class Handler37 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot37 spot, Player actor, ZLinkMessageContext ctx, Req37x0 req) {
            return CompletableFuture.completedFuture(reply(37, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot37 spot, Player actor, ZLinkMessageContext ctx, Req37x1 req) {
            return CompletableFuture.completedFuture(reply(37, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot37 spot, Player actor, ZLinkMessageContext ctx, Req37x2 req) {
            return CompletableFuture.completedFuture(reply(37, 2, req.value()));
        }
    }

    public record Req38x0(int value) {}

    public record Req38x1(int value) {}

    public record Req38x2(int value) {}

    public static final class Handler38 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot38 spot, Player actor, ZLinkMessageContext ctx, Req38x0 req) {
            return CompletableFuture.completedFuture(reply(38, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot38 spot, Player actor, ZLinkMessageContext ctx, Req38x1 req) {
            return CompletableFuture.completedFuture(reply(38, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot38 spot, Player actor, ZLinkMessageContext ctx, Req38x2 req) {
            return CompletableFuture.completedFuture(reply(38, 2, req.value()));
        }
    }

    public record Req39x0(int value) {}

    public record Req39x1(int value) {}

    public record Req39x2(int value) {}

    public static final class Handler39 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot39 spot, Player actor, ZLinkMessageContext ctx, Req39x0 req) {
            return CompletableFuture.completedFuture(reply(39, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot39 spot, Player actor, ZLinkMessageContext ctx, Req39x1 req) {
            return CompletableFuture.completedFuture(reply(39, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot39 spot, Player actor, ZLinkMessageContext ctx, Req39x2 req) {
            return CompletableFuture.completedFuture(reply(39, 2, req.value()));
        }
    }

    public record Req40x0(int value) {}

    public record Req40x1(int value) {}

    public record Req40x2(int value) {}

    public static final class Handler40 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot40 spot, Player actor, ZLinkMessageContext ctx, Req40x0 req) {
            return CompletableFuture.completedFuture(reply(40, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot40 spot, Player actor, ZLinkMessageContext ctx, Req40x1 req) {
            return CompletableFuture.completedFuture(reply(40, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot40 spot, Player actor, ZLinkMessageContext ctx, Req40x2 req) {
            return CompletableFuture.completedFuture(reply(40, 2, req.value()));
        }
    }

    public record Req41x0(int value) {}

    public record Req41x1(int value) {}

    public record Req41x2(int value) {}

    public static final class Handler41 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot41 spot, Player actor, ZLinkMessageContext ctx, Req41x0 req) {
            return CompletableFuture.completedFuture(reply(41, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot41 spot, Player actor, ZLinkMessageContext ctx, Req41x1 req) {
            return CompletableFuture.completedFuture(reply(41, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot41 spot, Player actor, ZLinkMessageContext ctx, Req41x2 req) {
            return CompletableFuture.completedFuture(reply(41, 2, req.value()));
        }
    }

    public record Req42x0(int value) {}

    public record Req42x1(int value) {}

    public record Req42x2(int value) {}

    public static final class Handler42 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot42 spot, Player actor, ZLinkMessageContext ctx, Req42x0 req) {
            return CompletableFuture.completedFuture(reply(42, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot42 spot, Player actor, ZLinkMessageContext ctx, Req42x1 req) {
            return CompletableFuture.completedFuture(reply(42, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot42 spot, Player actor, ZLinkMessageContext ctx, Req42x2 req) {
            return CompletableFuture.completedFuture(reply(42, 2, req.value()));
        }
    }

    public record Req43x0(int value) {}

    public record Req43x1(int value) {}

    public record Req43x2(int value) {}

    public static final class Handler43 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot43 spot, Player actor, ZLinkMessageContext ctx, Req43x0 req) {
            return CompletableFuture.completedFuture(reply(43, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot43 spot, Player actor, ZLinkMessageContext ctx, Req43x1 req) {
            return CompletableFuture.completedFuture(reply(43, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot43 spot, Player actor, ZLinkMessageContext ctx, Req43x2 req) {
            return CompletableFuture.completedFuture(reply(43, 2, req.value()));
        }
    }

    public record Req44x0(int value) {}

    public record Req44x1(int value) {}

    public record Req44x2(int value) {}

    public static final class Handler44 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot44 spot, Player actor, ZLinkMessageContext ctx, Req44x0 req) {
            return CompletableFuture.completedFuture(reply(44, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot44 spot, Player actor, ZLinkMessageContext ctx, Req44x1 req) {
            return CompletableFuture.completedFuture(reply(44, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot44 spot, Player actor, ZLinkMessageContext ctx, Req44x2 req) {
            return CompletableFuture.completedFuture(reply(44, 2, req.value()));
        }
    }

    public record Req45x0(int value) {}

    public record Req45x1(int value) {}

    public record Req45x2(int value) {}

    public static final class Handler45 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot45 spot, Player actor, ZLinkMessageContext ctx, Req45x0 req) {
            return CompletableFuture.completedFuture(reply(45, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot45 spot, Player actor, ZLinkMessageContext ctx, Req45x1 req) {
            return CompletableFuture.completedFuture(reply(45, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot45 spot, Player actor, ZLinkMessageContext ctx, Req45x2 req) {
            return CompletableFuture.completedFuture(reply(45, 2, req.value()));
        }
    }

    public record Req46x0(int value) {}

    public record Req46x1(int value) {}

    public record Req46x2(int value) {}

    public static final class Handler46 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot46 spot, Player actor, ZLinkMessageContext ctx, Req46x0 req) {
            return CompletableFuture.completedFuture(reply(46, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot46 spot, Player actor, ZLinkMessageContext ctx, Req46x1 req) {
            return CompletableFuture.completedFuture(reply(46, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot46 spot, Player actor, ZLinkMessageContext ctx, Req46x2 req) {
            return CompletableFuture.completedFuture(reply(46, 2, req.value()));
        }
    }

    public record Req47x0(int value) {}

    public record Req47x1(int value) {}

    public record Req47x2(int value) {}

    public static final class Handler47 {
        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m0(
                Spot47 spot, Player actor, ZLinkMessageContext ctx, Req47x0 req) {
            return CompletableFuture.completedFuture(reply(47, 0, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m1(
                Spot47 spot, Player actor, ZLinkMessageContext ctx, Req47x1 req) {
            return CompletableFuture.completedFuture(reply(47, 1, req.value()));
        }

        @ZLinkSpotActorRequest
        public CompletionStage<Reply> m2(
                Spot47 spot, Player actor, ZLinkMessageContext ctx, Req47x2 req) {
            return CompletableFuture.completedFuture(reply(47, 2, req.value()));
        }
    }
}
