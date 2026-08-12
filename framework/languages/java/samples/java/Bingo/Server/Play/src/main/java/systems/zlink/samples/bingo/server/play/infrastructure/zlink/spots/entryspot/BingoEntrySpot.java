package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.entryspot;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;


import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.samples.bingo.server.configuration.SampleTimings;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActor;
import systems.zlink.samples.bingo.server.play.domain.bingo.BingoRoomModels;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class BingoEntrySpot implements ZLinkEntrySpot<PlayerActor> {
    private final ZLinkEntrySpotContext context;
    private final ZLinkSpotManager spots;

    public BingoEntrySpot(
        ZLinkEntrySpotContext context,
        ZLinkSpotManager spots) {
        this.context = context;
        this.spots = spots;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
        PlayerActor actor,
        ZLinkMessage createRequest) {
        Messages.EnsurePlayerActorReq request =
            createRequest.decode(Messages.EnsurePlayerActorReq.class);
        actor.setDisplayName(request.getDisplayName());
        return CompletableFuture.completedFuture(
            ZLinkActorCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onJoinedActor(
        PlayerActor actor) {
        if (actor.destroyAfterEntrySpotJoin()) {
            return context.destroyActor(actor);
        }
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(
        PlayerActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnectActor(
        PlayerActor actor) {
        actor.markDisconnected();
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Messages.ObserveBingoEventsRes> observeEvents(
        PlayerActor actor,
        Messages.ObserveBingoEventsReq request) {
        String observerSpotId = "observe:" + request.getRoomId() + ":" + actor.actorId();
        BingoRoomModels.BingoRoomSettings settings =
            BingoRoomModels.BingoRoomSettings.createObserver(
                request.getRoomId(),
                actor.actorId(),
                SampleTimings.DrawPeriod.toMillis());
        Messages.BingoRoomSettingsPayload settingsPayload =
            Messages.BingoRoomSettingsPayload.newBuilder()
                .setRoomName(settings.roomName())
                .setMode(settings.mode())
                .setRequiredPlayers(settings.requiredPlayers())
                .setMaxDrawNumber(settings.maxDrawNumber())
                .setPurpose(settings.purpose())
                .setObservedRoomId(settings.observedRoomId())
                .build();
        return spots.getOrCreate(observerSpotId, SampleNames.RoomSpotType)
            .inMesh(SampleNames.Mesh)
            .request(BingoMessages.bingoRoomCreateReq(settingsPayload))
            .submit()
            .thenApply(ignored -> {
                actor.context().joinSpot(
                    observerSpotId,
                    BingoMessages.bingoRoomJoinReq(
                        request.getRoomId(),
                        actor.actorId(),
                        actor.displayName(),
                        true))
                    .defer();
                return BingoMessages.observeBingoEventsRes(true);
            });
    }

}
