package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot;


import java.time.Duration;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCompletion;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.server.configuration.SampleTimings;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActor;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers.BingoRoomSettingsInitializer;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers.BingoRoomTimerHandler;
import systems.zlink.samples.bingo.server.play.domain.bingo.BingoGame;
import systems.zlink.samples.bingo.server.play.domain.bingo.BingoRoomGame;
import systems.zlink.samples.bingo.server.play.domain.bingo.BingoRoomModels;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class BingoRoomSpot implements ZLinkSpot<PlayerActor> {
    private final ZLinkSpotContext context;
    private final BingoRoomSettingsInitializer settingsInitializer;
    private final Map<String, PlayerActor> actors = new HashMap<>();
    private final Map<String, PlayerActor> observers = new HashMap<>();
    private final Map<String, Messages.BingoRoomJoinReq> pendingJoins = new HashMap<>();
    private final List<Messages.BingoRewardAcquiredEvent> pendingObserverRewards =
        new java.util.ArrayList<>();
    private BingoRoomModels.BingoRoomSettings settings =
        BingoRoomModels.BingoRoomSettings.create("two-player", 0, SampleTimings.DrawPeriod.toMillis());
    private BingoRoomGame game;
    private ZLinkTimer timer;
    private boolean cleanupStarted;

    public BingoRoomSpot(
        ZLinkSpotContext context,
        BingoRoomSettingsInitializer settingsInitializer) {
        this.context = context;
        this.settingsInitializer = settingsInitializer;
        this.game = BingoGame.room(context.spotId(), settings);
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public java.util.concurrent.CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        return java.util.concurrent.CompletableFuture.completedFuture(settingsInitializer.handle(this, request));
    }

    @Override
    public java.util.concurrent.CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        Messages.BingoRoomJoinReq joinRequest = request.decode(Messages.BingoRoomJoinReq.class);
        validateJoin(actorId, joinRequest);
        Messages.BingoRoomState preview = joinRequest.getObserveOnly()
            ? observerJoinState(joinRequest)
            : game.previewJoin(actorId, joinRequest.getDisplayName());
        pendingJoins.put(actorId, joinRequest);
        return java.util.concurrent.CompletableFuture.completedFuture(
            ZLinkSpotActorJoinResult.accept(BingoMessages.bingoRoomJoinRes(preview)));
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onJoinedActor(
        PlayerActor actor) {
        Messages.BingoRoomJoinReq request = pendingJoins.get(actor.actorId());
        if (request == null) {
            throw new IllegalStateException("joined actor does not have a pending admission");
        }
        if (request.getObserveOnly()) {
            pendingJoins.remove(actor.actorId());
            join(actor, request, 0, 0);
            List<Messages.BingoRewardAcquiredEvent> pendingRewards =
                List.copyOf(pendingObserverRewards);
            pendingObserverRewards.clear();
            if (pendingRewards.isEmpty()) {
                return java.util.concurrent.CompletableFuture.completedFuture(null);
            }
            return java.util.concurrent.CompletableFuture.allOf(
                pendingRewards.stream()
                    .map(this::notifyObservers)
                    .map(java.util.concurrent.CompletionStage::toCompletableFuture)
                    .toArray(java.util.concurrent.CompletableFuture[]::new));
        }
        return context.outbound()
            .requestToChannel(SampleNames.ApiChannel, BingoMessages.getPlayerRecordReq(actor.actorId()))
            .timeout(SampleTimings.RequestTimeout)
            .yield(Messages.GetPlayerRecordRes.class)
            .thenAccept(record -> {
                if (pendingJoins.get(actor.actorId()) != request) {
                    return;
                }
                pendingJoins.remove(actor.actorId());
                join(actor, request, record.getWins(), record.getLosses());
            });
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onLeaveActor(
        PlayerActor actor) {
        if (!actors.containsKey(actor.actorId()) || game == null) {
            observers.remove(actor.actorId());
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        Messages.BingoRoomState finalState = game.snapshot();
        Messages.ReportBingoResultReq report = BingoMessages.reportBingoResultReq(
            finalState.getRoomId(),
            actor.actorId(),
            finalState.getWinnersList().contains(actor.actorId()),
            finalState.getDrawSeq());
        return context.outbound()
            .requestToChannel(SampleNames.ApiChannel, report)
            .timeout(SampleTimings.RequestTimeout)
            .yield(Messages.ReportBingoResultRes.class)
            .thenAccept(ignored -> actors.remove(actor.actorId()));
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDisconnectActor(
        PlayerActor actor) {
        actor.markDisconnected();
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onInitialize() {
        if (settings.observerMode()) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        return context.addTimer(
                "bingo-draw",
                Duration.ofMillis(settings.drawPeriodMillis()),
                BingoRoomTimerHandler.class,
                null)
            .thenAccept(created -> timer = created);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onClosing() {
        return timer == null
            ? java.util.concurrent.CompletableFuture.completedFuture(null)
            : timer.cancel();
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onRelocationReadyCompleted(
        ZLinkSpotRelocationReadyCompletion completion) {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    public Messages.BingoRoomJoinRes join(
        PlayerActor actor,
        Messages.BingoRoomJoinReq request,
        int wins,
        int losses) {
        validateJoin(actor.actorId(), request);
        actor.setDisplayName(request.getDisplayName());
        actor.joinRoom(request.getRoomId());
        if (request.getObserveOnly()) {
            observers.put(actor.actorId(), actor);
            context.relocationReady().defer();
            return BingoMessages.bingoRoomJoinRes(observerJoinState(request));
        }
        actors.put(actor.actorId(), actor);
        BingoRoomGame.Change change = game.join(
            actor.actorId(), request.getDisplayName(), wins, losses);
        publishEvents(
            change.events(),
            actorId -> actorId.equals(actor.actorId()) ? null : actors.get(actorId));
        return BingoMessages.bingoRoomJoinRes(change.state());
    }

    private void validateJoin(
        String actorId,
        Messages.BingoRoomJoinReq request) {
        if (!actorId.equals(request.getActorId())) {
            throw new IllegalStateException("Join request actor id does not match bound actor.");
        }
        if (!request.getObserveOnly() && !request.getRoomId().equals(context.spotId())) {
            throw new IllegalStateException("Join request room id does not match bingo room.");
        }
        if (request.getObserveOnly()) {
            if (!settings.observerMode() || !request.getRoomId().equals(settings.observedRoomId())) {
                throw new IllegalStateException("Observe-only actor can join only its observer BingoRoom.");
            }
            return;
        }
        if (settings.observerMode()) {
            throw new IllegalStateException("Player actor cannot join an observer BingoRoom.");
        }
    }

    public Messages.SubmitBingoCardRes submitCard(
        PlayerActor actor,
        Messages.SubmitBingoCardReq request) {
        if (!request.getRoomId().equals(context.spotId())) {
            throw new IllegalStateException("Submit request room id does not match bingo room.");
        }
        if (game == null) {
            throw new IllegalStateException("Observer BingoRoom does not own game state.");
        }
        BingoRoomGame.Change change = game.submitCard(actor.actorId(), request.getCardList());
        publishEvents(change.events(), actors::get);
        return BingoMessages.submitBingoCardRes(change.state());
    }

    public java.util.concurrent.CompletionStage<Void> tick() {
        if (game == null || cleanupStarted) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        BingoRoomGame.Change change = game.drawNext();
        publishEvents(change.events(), actors::get);
        return publishWinner(change)
            .thenCompose(ignored -> leaveFinishedActors(change))
            .thenRun(() -> {
                if (change.state().getStatus().equals("Finished")) {
                    context.relocationReady().defer();
                }
            });
    }

    public java.util.concurrent.CompletionStage<Void> announceReward(
        Messages.BingoRewardAcquiredEvent event) {
        if (!settings.observerMode()
            || !event.getRoomId().equals(settings.observedRoomId())) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        if (observers.isEmpty()) {
            pendingObserverRewards.add(event);
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        return notifyObservers(event)
            .thenRun(() -> context.relocationReady().defer());
    }

    private java.util.concurrent.CompletionStage<Void> notifyObservers(
        Messages.BingoRewardAcquiredEvent event) {
        java.util.List<java.util.concurrent.CompletionStage<Void>> notifications =
            new java.util.ArrayList<>();
        for (PlayerActor observer : List.copyOf(observers.values())) {
            notifications.add(observer.push(rewardNotification(event)));
        }
        return java.util.concurrent.CompletableFuture.allOf(
            notifications.stream()
                .map(java.util.concurrent.CompletionStage::toCompletableFuture)
                .toArray(java.util.concurrent.CompletableFuture[]::new));
    }

    private Messages.BingoRewardAnnouncedNotify rewardNotification(
        Messages.BingoRewardAcquiredEvent event) {
        return BingoMessages.bingoRewardAnnouncedNotify(
            event.getRoomId(),
            event.getActorId(),
            event.getDrawSeq(),
            event.getItemId(),
            event.getItemName(),
            event.getRarity());
    }

    public Messages.StopObservingBingoEventsRes stopObserving(
        PlayerActor actor,
        Messages.StopObservingBingoEventsReq request) {
        if (!settings.observerMode()
            || !request.getRoomId().equals(settings.observedRoomId())
            || !observers.containsKey(actor.actorId())) {
            return BingoMessages.stopObservingBingoEventsRes(false);
        }
        observers.remove(actor.actorId());
        context.leaveActor(actor).exceptionally(error -> null);
        return BingoMessages.stopObservingBingoEventsRes(true);
    }

    public void applySettings(BingoRoomModels.BingoRoomSettings settings) {
        if (!settings.observerMode() && settings.requiredPlayers() <= 0) {
            throw new IllegalStateException("Bingo room requires at least one player.");
        }
        if (settings.maxDrawNumber() <= 0) {
            throw new IllegalStateException("Bingo room requires at least one draw number.");
        }
        if (settings.drawPeriodMillis() <= 0) {
            throw new IllegalStateException("Bingo room draw period must be positive.");
        }
        this.settings = settings;
        this.game = settings.observerMode() ? null : BingoGame.room(context.spotId(), settings);
        this.cleanupStarted = false;
    }

    RelocationState captureRelocationState() {
        Messages.BingoRoomState state = game == null
            ? BingoMessages.bingoRoomState(
                context.spotId(), "Running", "", false, 0, null,
                List.of(), List.of(), List.of())
            : game.snapshot();
        return new RelocationState(settings, state);
    }

    void restoreRelocationState(RelocationState state) {
        settings = state.settings();
        game = settings.observerMode()
            ? null
            : BingoRoomGame.restore(context.spotId(), settings, state.state());
        cleanupStarted = false;
    }

    record RelocationState(
        BingoRoomModels.BingoRoomSettings settings,
        Messages.BingoRoomState state) {
    }

    private java.util.concurrent.CompletionStage<Void> leaveFinishedActors(BingoRoomGame.Change change) {
        if (cleanupStarted || !change.state().getStatus().equals("Finished")) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }

        cleanupStarted = true;
        java.util.List<java.util.concurrent.CompletableFuture<Void>> leaves = new java.util.ArrayList<>();
        for (PlayerActor actor : actors.values().toArray(PlayerActor[]::new)) {
            actor.markForDestroyAfterRoomLeave();
            leaves.add(context.leaveActor(actor).toCompletableFuture());
        }
        return java.util.concurrent.CompletableFuture.allOf(
            leaves.toArray(java.util.concurrent.CompletableFuture[]::new));
    }

    private java.util.concurrent.CompletionStage<Void> publishWinner(
        BingoRoomGame.Change change) {
        if (!change.state().getStatus().equals("Finished") || change.state().getWinnersList().isEmpty()) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        String winner = change.state().getWinnersList().getFirst();
        return context.outbound()
            .publish(
                SampleNames.RoomRewardChannel,
                SampleNames.WinnerTopic,
                BingoMessages.bingoRewardAcquiredEvent(
                    change.state().getRoomId(),
                    winner,
                    change.state().getDrawSeq(),
                    "rare-golden-dauber",
                    "Golden Dauber",
                    "Legendary"))
            .submit();
    }

    private void publishEvents(
        List<BingoRoomModels.RoomEvent> events,
        java.util.function.Function<String, PlayerActor> actorResolver) {
        for (BingoRoomModels.RoomEvent event : events) {
            publishEvent(event, actorResolver.apply(event.recipientActorId()));
        }
    }

    private void publishEvent(BingoRoomModels.RoomEvent event, PlayerActor recipient) {
        if (recipient == null) {
            return;
        }
        switch (event.kind()) {
            case PLAYER_JOINED -> recipient.push(BingoMessages.playerJoinedNotify(
                    event.state().getRoomId(),
                    event.joinedActorId(),
                    event.joinedDisplayName(),
                    event.seat(),
                    event.host(),
                    event.state()));
            case GAME_STARTED -> recipient.push(BingoMessages.bingoGameStartedNotify(event.state()));
            case NUMBER_DRAWN -> recipient.push(BingoMessages.bingoNumberDrawnNotify(
                    event.state().getRoomId(),
                    event.state().getDrawSeq(),
                    event.drawnNumber(),
                    event.state()));
            case STATE -> recipient.push(BingoMessages.bingoStateNotify(event.state()));
            case GAME_ENDED -> recipient.push(BingoMessages.bingoGameEndedNotify(event.state()));
        }
    }

    private Messages.BingoRoomState observerJoinState(
        Messages.BingoRoomJoinReq request) {
        return BingoMessages.bingoRoomState(
            request.getRoomId(),
            "Running",
            "",
            false,
            0,
            null,
            List.of(),
            List.of(),
            List.of());
    }

}
