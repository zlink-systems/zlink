package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.samples.tictactoe.server.configuration.PlaySettings;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActor;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers.PlayActorJoinGameHandler;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers.PlayActorObserveMilestoneHandler;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers.PlayerWinMilestoneEventHandler;
import systems.zlink.samples.tictactoe.shared.contracts.ObserveMilestoneRes;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerActorCreateReq;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerWinMilestoneEvent;
import systems.zlink.samples.tictactoe.shared.contracts.WinMilestoneNotify;

// --8<-- [start:doc-entry-spot]
public final class PlayEntrySpot implements ZLinkEntrySpot<PlayActor> {
    private static final Logger LOGGER = LoggerFactory.getLogger(PlayEntrySpot.class);
    private final ZLinkEntrySpotContext context;
    private final PlaySettings settings;
    private final List<PlayActor> milestoneObservers = new ArrayList<>();

    public PlayEntrySpot(
        ZLinkEntrySpotContext context,
        PlaySettings settings) {
        this.context = context;
        this.settings = settings;
        // send: JoinGameMsg를 받고 join 완료 뒤 current session으로 결과를 push한다.
        context.handlers().addHandler(PlayActorJoinGameHandler.class);
        // request: ObserveMilestoneReq에 ObserveMilestoneRes로 응답한다.
        context.handlers().addHandler(PlayActorObserveMilestoneHandler.class);
        // subscribe: PlayerWinMilestoneEvent를 받아 observer session에 알린다.
        context.handlers().addHandler(PlayerWinMilestoneEventHandler.class);
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
        PlayActor actor,
        ZLinkMessage createRequest) {
        if (createRequest.isEmpty()) {
            return CompletableFuture.completedFuture(
                ZLinkActorCreateResponse.accept());
        }
        PlayerActorCreateReq request = createRequest.decode(PlayerActorCreateReq.class);
        actor.applyPlayer(request.player());
        return CompletableFuture.completedFuture(
            ZLinkActorCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onJoinedActor(PlayActor actor) {
        if (actor.destroyAfterEntrySpotJoin()) {
            return context.destroyActor(actor)
                .thenRun(() -> LOGGER.info(
                    "tictactoe-lifecycle actor-destroy-complete actor={}", actor.actorId()));
        }
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(PlayActor actor) {
        milestoneObservers.removeIf(existing -> existing.actorId().equals(actor.actorId()));
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnectActor(PlayActor actor) {
        actor.markDisconnected();
        milestoneObservers.removeIf(existing -> existing.actorId().equals(actor.actorId()));
        return CompletableFuture.completedFuture(null);
    }

    public ObserveMilestoneRes observeMilestone(PlayActor actor) {
        rememberObserver(actor);
        return new ObserveMilestoneRes(true);
    }

    public void notifyMilestone(PlayerWinMilestoneEvent event) {
        WinMilestoneNotify payload = new WinMilestoneNotify(
            event.roomId(),
            event.actorId(),
            event.displayName(),
            event.wins());
        for (PlayActor observer : List.copyOf(milestoneObservers)) {
            observer.context().boundSession()
                .send(payload)
                .submit();
        }
    }

    private void rememberObserver(PlayActor actor) {
        milestoneObservers.removeIf(existing -> existing.actorId().equals(actor.actorId()));
        milestoneObservers.add(actor);
    }
}
// --8<-- [end:doc-entry-spot]
