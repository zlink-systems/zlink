package systems.zlink.e2e.registrymessaging.shared;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

/** Small public object-server fixture used by RM-A7. */
public final class IdentityObjects {
    private IdentityObjects() {
    }

    public static final class Actor implements ZLinkActor {
        private final ZLinkActorContext context;
        private final AtomicInteger requests = new AtomicInteger();

        public Actor(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        int nextRequest() {
            return requests.incrementAndGet();
        }
    }

    public static final class ActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Actor(context));
        }
    }

    public static final class EntrySpot implements ZLinkEntrySpot<Actor> {
        private final ZLinkEntrySpotContext context;

        public EntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ActorRequestHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    @ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
    public static final class ActorRequestHandler implements
        ZLinkEntrySpotActorRequestHandler<
            EntrySpot, Actor, Contracts.IdentityActorPingReq, Contracts.IdentityActorPingRes> {
        @Override
        public CompletionStage<Contracts.IdentityActorPingRes> handle(
            EntrySpot entrySpot,
            Actor actor,
            ZLinkMessageContext context,
            Contracts.IdentityActorPingReq request) {
            return CompletableFuture.completedFuture(new Contracts.IdentityActorPingRes(
                request.marker(),
                actor.context().actorId(),
                actor.context().objectGeneration(),
                actor.nextRequest(),
                actor.context().meshName()));
        }
    }

    public static final class Spot implements ZLinkSpot<Actor> {
        private final ZLinkSpotContext context;
        private final AtomicInteger requests = new AtomicInteger();

        public Spot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(SpotRequestHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(
            systems.zlink.framework.messaging.ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }

        int nextRequest() {
            return requests.incrementAndGet();
        }
    }

    @ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
    public static final class SpotRequestHandler implements
        ZLinkSpotRequestHandler<Spot, Contracts.IdentitySpotPingReq, Contracts.IdentitySpotPingRes> {
        @Override
        public CompletionStage<Contracts.IdentitySpotPingRes> handle(
            Spot spot,
            Contracts.IdentitySpotPingReq request) {
            return CompletableFuture.completedFuture(new Contracts.IdentitySpotPingRes(
                request.marker(),
                spot.context().spotId(),
                spot.context().objectGeneration(),
                spot.nextRequest(),
                spot.context().nodeRid().toString(),
                spot.context().nodeRid().toString()));
        }
    }
}
