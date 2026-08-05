package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot;

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
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.entryspot.handlers.PlayerWinMilestoneMsgHandler;
import systems.zlink.samples.tictactoe.shared.contracts.ObserveMilestoneRes;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerInfo;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerWinMilestoneMsg;
import systems.zlink.samples.tictactoe.shared.contracts.WinMilestoneNotify;

// --8<-- [start:doc-entry-spot]
public final class PlayEntrySpot implements ZLinkEntrySpot<PlayActor> {
    private static final Logger LOGGER = LoggerFactory.getLogger(PlayEntrySpot.class);
    private final ZLinkEntrySpotContext context;
    private final PlaySettings settings;
    private final java.util.List<PlayActor> milestoneObservers = new java.util.ArrayList<>();

    public PlayEntrySpot(
        ZLinkEntrySpotContext context,
        PlaySettings settings) {
        this.context = context;
        this.settings = settings;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public java.util.concurrent.CompletionStage<ZLinkActorCreateResponse> onCreateActor(
        PlayActor actor,
        ZLinkMessage createRequest) {
        if (createRequest.isEmpty()) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkActorCreateResponse.accept());
        }
        actor.applyPlayer(createRequest.decode(PlayerInfo.class));
        return java.util.concurrent.CompletableFuture.completedFuture(
            ZLinkActorCreateResponse.accept());
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onJoinedActor(PlayActor actor) {
        if (actor.destroyAfterEntrySpotJoin()) {
            return context.destroyActor(actor)
                .thenRun(() -> LOGGER.info(
                    "tictactoe actor destroy completed actor={}", actor.actorId()));
        }
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onLeaveActor(PlayActor actor) {
        milestoneObservers.removeIf(existing -> existing.actorId().equals(actor.actorId()));
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDisconnectActor(PlayActor actor) {
        actor.markDisconnected();
        milestoneObservers.removeIf(existing -> existing.actorId().equals(actor.actorId()));
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    public ObserveMilestoneRes observeMilestone(PlayActor actor) {
        rememberObserver(actor);
        return new ObserveMilestoneRes(true);
    }

    public void notifyMilestone(PlayerWinMilestoneMsg event) {
        WinMilestoneNotify payload = new WinMilestoneNotify(
            event.roomId(),
            event.actorId(),
            event.displayName(),
            event.wins());
        for (PlayActor observer : java.util.List.copyOf(milestoneObservers)) {
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
