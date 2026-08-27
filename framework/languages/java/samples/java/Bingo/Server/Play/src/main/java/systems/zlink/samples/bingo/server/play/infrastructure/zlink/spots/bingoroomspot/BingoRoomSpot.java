package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot;
import java.util.ArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;


import java.time.Duration;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
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
    private static final Logger logger = LoggerFactory.getLogger(BingoRoomSpot.class);

    private final ZLinkSpotContext context;
    private final BingoRoomSettingsInitializer settingsInitializer;
    private final Map<String, PlayerActor> actors = new HashMap<>();
    private final Map<String, PlayerActor> observers = new HashMap<>();
    private final Map<String, Messages.BingoRoomJoinReq> pendingJoins = new HashMap<>();
    private final List<Messages.BingoRewardAcquiredEvent> pendingObserverRewards =
        new ArrayList<>();
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
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        return CompletableFuture.completedFuture(settingsInitializer.handle(this, request));
    }

    @Override
    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        Messages.BingoRoomJoinReq joinRequest = request.decode(Messages.BingoRoomJoinReq.class);
        validateJoin(actorId, joinRequest);
        Messages.BingoRoomState preview = joinRequest.getObserveOnly()
            ? observerJoinState(joinRequest)
            : game.previewJoin(actorId, joinRequest.getDisplayName());
        pendingJoins.put(actorId, joinRequest);
        return CompletableFuture.completedFuture(
            ZLinkSpotActorJoinResult.accept(BingoMessages.bingoRoomJoinRes(preview)));
    }

    @Override
    public CompletionStage<Void> onJoinedActor(
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
                return CompletableFuture.completedFuture(null);
            }
            return CompletableFuture.allOf(
                pendingRewards.stream()
                    .map(this::notifyObservers)
                    .map(CompletionStage::toCompletableFuture)
                    .toArray(CompletableFuture[]::new));
        }
        return context.outbound()
            .requestToChannel(SampleNames.ApiChannel, BingoMessages.getPlayerRecordReq(actor.actorId()))
            .timeout(SampleTimings.RequestTimeout)
            .yield(Messages.GetPlayerRecordRes.class)
            .thenAccept(record -> {
                logger.info("bingo-record fetched actor={} wins={} losses={}",
                    actor.actorId(), record.getWins(), record.getLosses());
                if (pendingJoins.get(actor.actorId()) != request) {
                    return;
                }
                pendingJoins.remove(actor.actorId());
                join(actor, request, record.getWins(), record.getLosses());
            });
    }

    @Override
    public CompletionStage<Void> onLeaveActor(
        PlayerActor actor) {
        if (!actors.containsKey(actor.actorId()) || game == null) {
            observers.remove(actor.actorId());
            logger.info("bingo-lifecycle room-leave actor={}", actor.actorId());
            return CompletableFuture.completedFuture(null);
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
            .thenAccept(record -> {
                logger.info("bingo-record reported actor={} wins={} losses={}",
                    actor.actorId(), record.getWins(), record.getLosses());
                actors.remove(actor.actorId());
                logger.info("bingo-lifecycle room-leave actor={}", actor.actorId());
            });
    }

    @Override
    public CompletionStage<Void> onDisconnectActor(
        PlayerActor actor) {
        actor.markDisconnected();
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onInitialize() {
        if (settings.observerMode()) {
            return CompletableFuture.completedFuture(null);
        }
        return context.addTimer(
                "bingo-draw",
                Duration.ofMillis(settings.drawPeriodMillis()),
                BingoRoomTimerHandler.class,
                null)
            .thenAccept(created -> timer = created);
    }

    @Override
    public CompletionStage<Void> onClosing() {
        return timer == null
            ? CompletableFuture.completedFuture(null)
            : timer.cancel();
    }

    @Override
    public CompletionStage<Void> onRelocationReadyCompleted(
        ZLinkSpotRelocationReadyCompletion completion) {
        return CompletableFuture.completedFuture(null);
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
        publishEvents(change.events(), actors::get);
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

    public CompletionStage<Void> tick() {
        if (game == null || cleanupStarted) {
            return CompletableFuture.completedFuture(null);
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

    public CompletionStage<Void> announceReward(
        Messages.BingoRewardAcquiredEvent event) {
        if (!settings.observerMode()
            || !event.getRoomId().equals(settings.observedRoomId())) {
            return CompletableFuture.completedFuture(null);
        }
        if (observers.isEmpty()) {
            pendingObserverRewards.add(event);
            return CompletableFuture.completedFuture(null);
        }
        return notifyObservers(event)
            .thenRun(() -> context.relocationReady().defer());
    }

    private CompletionStage<Void> notifyObservers(
        Messages.BingoRewardAcquiredEvent event) {
        List<CompletionStage<Void>> notifications =
            new ArrayList<>();
        for (PlayerActor observer : List.copyOf(observers.values())) {
            notifications.add(observer.push(rewardNotification(event)));
        }
        return CompletableFuture.allOf(
            notifications.stream()
                .map(CompletionStage::toCompletableFuture)
                .toArray(CompletableFuture[]::new));
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

    private CompletionStage<Void> leaveFinishedActors(BingoRoomGame.Change change) {
        if (cleanupStarted || !change.state().getStatus().equals("Finished")) {
            return CompletableFuture.completedFuture(null);
        }

        cleanupStarted = true;
        List<CompletableFuture<Void>> leaves = new ArrayList<>();
        for (PlayerActor actor : actors.values().toArray(PlayerActor[]::new)) {
            actor.markForDestroyAfterRoomLeave();
            leaves.add(context.leaveActor(actor).toCompletableFuture());
        }
        return CompletableFuture.allOf(
            leaves.toArray(CompletableFuture[]::new));
    }

    private CompletionStage<Void> publishWinner(
        BingoRoomGame.Change change) {
        if (!change.state().getStatus().equals("Finished") || change.state().getWinnersList().isEmpty()) {
            return CompletableFuture.completedFuture(null);
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
        Function<String, PlayerActor> actorResolver) {
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
