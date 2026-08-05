package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.HashMap;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActor;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.play.domain.tictactoe.TicTacToeMatch;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers.PlayActorLeaveGameHandler;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers.PlayActorPlaceMarkHandler;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers.TicTacToeGameCreatedHandler;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers.TicTacToeGameTimerHandler;
import systems.zlink.samples.tictactoe.shared.contracts.GameState;
import systems.zlink.samples.tictactoe.shared.contracts.GameStateNotify;
import systems.zlink.samples.tictactoe.shared.contracts.PlaceMarkRes;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerInfo;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerJoinedNotify;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerWinMilestoneMsg;
import systems.zlink.samples.tictactoe.shared.contracts.TicTacToeGameJoinReq;
import systems.zlink.samples.tictactoe.shared.contracts.TicTacToeGameJoinRes;

public final class TicTacToeGame implements ZLinkSpot<PlayActor> {
    private static final Duration GAME_TICK_PERIOD = Duration.ofSeconds(1);
    private static final Duration TURN_TIMEOUT = Duration.ofSeconds(15);

    private final ZLinkSpotContext context;
    private final String roomId;
    private final TicTacToeMatch match;
    private ZLinkTimer gameTick;
    private boolean created;
    private final TicTacToeGameCreatedHandler createdHandler;
    private final ObjectMapper json;
    private final Map<String, TicTacToeGameJoinReq> pendingJoins = new HashMap<>();

    public TicTacToeGame(
        ZLinkSpotContext context,
        TicTacToeGameCreatedHandler createdHandler,
        ObjectMapper json) {
        this.context = context;
        this.roomId = context.spotId();
        this.match = new TicTacToeMatch(roomId);
        this.createdHandler = createdHandler;
        this.json = json;
    }

    public String roomId() {
        return roomId;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public java.util.concurrent.CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        return java.util.concurrent.CompletableFuture.completedFuture(createdHandler.handle(this, request));
    }

    @Override
    public java.util.concurrent.CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        TicTacToeGameJoinReq joinRequest = request.decode(TicTacToeGameJoinReq.class);
        if (!actorId.equals(joinRequest.player().actorId())) {
            throw new IllegalStateException("join request actor id does not match bound actor");
        }
        validateJoin(joinRequest.roomId(), joinRequest.player());
        if (!match.canJoin(actorId)) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkSpotActorJoinResult.reject(new TicTacToeGameJoinRes(match.snapshot())));
        }
        TicTacToeMatch.JoinResult preview = match.previewJoin(actorId);
        pendingJoins.put(actorId, joinRequest);
        return java.util.concurrent.CompletableFuture.completedFuture(
            ZLinkSpotActorJoinResult.accept(new TicTacToeGameJoinRes(preview.state())));
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onJoinedActor(PlayActor actor) {
        TicTacToeGameJoinReq joinRequest = pendingJoins.remove(actor.actorId());
        if (joinRequest == null) {
            throw new IllegalStateException("joined actor does not have a pending admission");
        }
        join(actor, joinRequest.roomId(), joinRequest.player());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onLeaveActor(PlayActor actor) {
        actors.removeIf(existing -> existing.actorId().equals(actor.actorId()));
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDisconnectActor(PlayActor actor) {
        actor.markDisconnected();
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onInitialize() {
        return context.addTimer(
                "game-tick",
                GAME_TICK_PERIOD,
                TicTacToeGameTimerHandler.class,
                null)
            .thenAccept(timer -> gameTick = timer);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onClosing() {
        if (gameTick != null) {
            return gameTick.cancel();
        }
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    public void markCreated(ZLinkMessage request) {
        if (!request.isEmpty()) {
            throw new IllegalArgumentException("tic-tac-toe game creation does not accept payload");
        }
        created = true;
    }

    public TicTacToeGameJoinRes join(PlayActor actor, String roomId, PlayerInfo player) {
        validateJoin(roomId, player);
        actor.applyPlayer(player);
        TicTacToeMatch.JoinResult joined = match.join(
            actor.actorId(),
            Instant.now(),
            TURN_TIMEOUT);
        actor.joinGame(roomId);
        rememberActor(actor);
        if (joined.newlyJoined()) {
            notifyPlayerJoined(actor, joined.mark(), joined.state());
        }
        broadcast(joined.state(), actor.actorId());
        return new TicTacToeGameJoinRes(joined.state());
    }

    private void validateJoin(String roomId, PlayerInfo player) {
        ensureCreated();
        if (!this.roomId.equals(roomId)) {
            throw new IllegalStateException("join request room id does not match game room");
        }
        if (player.level() < SampleNames.RequiredLevel) {
            throw new IllegalStateException("player level does not satisfy room requirement");
        }
    }

    public PlaceMarkRes placeMark(PlayActor actor, int cell) {
        ensureCreated();
        GameState before = match.snapshot();
        GameState state = match.placeMark(
            actor.actorId(),
            cell,
            Instant.now(),
            TURN_TIMEOUT);
        broadcast(state, actor.actorId());
        publishWinMilestone(actor, before, state);
        return new PlaceMarkRes(state);
    }

    public String winner() {
        return snapshot().winner();
    }

    public boolean hasPlayer(String actorId) {
        return actors.stream().anyMatch(actor -> actor.actorId().equals(actorId));
    }

    public GameState snapshot() {
        ensureCreated();
        return match.snapshot();
    }

    public java.util.concurrent.CompletionStage<Void> tick() {
        ensureCreated();
        GameState timedOut = match.timeOutCurrentTurn(Instant.now());
        if (timedOut == null) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        broadcast(timedOut, null);
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    private void ensureCreated() {
        if (!created) {
            throw new IllegalStateException("tic-tac-toe game has not completed creation");
        }
    }

    private final List<PlayActor> actors = new java.util.ArrayList<>();

    private void broadcast(GameState state, String excludedActorId) {
        actors.stream()
            .filter(actor -> excludedActorId == null || !actor.actorId().equals(excludedActorId))
            .forEach(actor -> actor.context().boundSession()
                .send(new GameStateNotify(state))
                .submit());
    }

    private void notifyPlayerJoined(
        PlayActor joinedActor,
        String mark,
        GameState state) {
        PlayerInfo player = joinedActor.requirePlayer();
        PlayerJoinedNotify message = new PlayerJoinedNotify(
            state.roomId(),
            joinedActor.actorId(),
            player.displayName(),
            player.level(),
            mark,
            state);
        actors.stream()
            .filter(actor -> !actor.actorId().equals(joinedActor.actorId()))
            .forEach(actor -> actor.context().boundSession()
                .send(message)
                .submit());
    }

    private void rememberActor(PlayActor actor) {
        for (int i = 0; i < actors.size(); i++) {
            if (actors.get(i).actorId().equals(actor.actorId())) {
                actors.set(i, actor);
                return;
            }
        }
        actors.add(actor);
    }

    private static boolean isTerminal(GameState state) {
        return "Won".equals(state.status())
            || "Draw".equals(state.status())
            || "TurnTimedOut".equals(state.status());
    }

    public java.util.concurrent.CompletionStage<Void> leaveGame(PlayActor actor, String roomId) {
        if (!this.roomId.equals(roomId)) {
            throw new IllegalStateException("leave request room id does not match game room");
        }
        if (!isTerminal(snapshot())) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        actor.markForDestroyAfterRoomLeave();
        return context.leaveActor(actor);
    }

    private void publishWinMilestone(
        PlayActor actor,
        GameState before,
        GameState after) {
        if (!"Won".equals(after.status())
            || "Won".equals(before.status())
            || !actor.actorId().equals(after.winner())) {
            return;
        }
        PlayerInfo player = actor.requirePlayer();
        int wins = actor.incrementWins();
        if (player.wins() < 99 || wins != 100) {
            return;
        }
        context.outbound()
            .publish(SampleNames.PlayNode, SampleNames.PlayerMilestoneTopic,
                new PlayerWinMilestoneMsg(
                after.roomId(),
                actor.actorId(),
                player.displayName(),
                wins))
            .submit();
    }
}
