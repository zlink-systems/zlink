package systems.zlink.e2e.channelegress.role;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.channelegress.shared.Contracts;
import systems.zlink.e2e.channelegress.shared.EvidenceState;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;

public final class Config12ActorComponents {
    private Config12ActorComponents() {
    }

    public static final class Actor implements ZLinkActor {
        private final String actorId;
        private final ZLinkActorContext context;

        public Actor(ZLinkActorContext context) {
            this.actorId = context.actorId();
            this.context = context;
        }

        public String actorId() {
            return actorId;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
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
        private final EvidenceState evidence;

        public EntrySpot(ZLinkEntrySpotContext context, EvidenceState evidence) {
            this.context = context;
            this.evidence = evidence;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ActorProbeHandler.class);
        }

        @Override
        public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
            Actor actor,
            ZLinkMessage request) {
            evidence.add("actor-create", "actor=" + actor.actorId());
            return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
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

    public static final class ActorProbeHandler implements ZLinkEntrySpotActorRequestHandler<
        EntrySpot,
        Actor,
        Contracts.ObjectProbeReq,
        Contracts.ObjectProbeRes> {
        private final EvidenceState evidence;

        public ActorProbeHandler(EvidenceState evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.ObjectProbeRes> handle(
            EntrySpot entrySpot,
            Actor actor,
            ZLinkMessageContext context,
            Contracts.ObjectProbeReq request) {
            evidence.add("actor-request", "actor=" + actor.actorId() + "|id=" + request.id());
            return CompletableFuture.completedFuture(new Contracts.ObjectProbeRes(
                request.id(), "actor", actor.actorId(), evidence.role()));
        }
    }
}
