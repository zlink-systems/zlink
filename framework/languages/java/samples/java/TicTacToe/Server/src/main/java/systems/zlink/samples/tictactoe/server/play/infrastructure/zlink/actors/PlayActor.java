package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.samples.tictactoe.shared.contracts.JoinGameFailedNotify;
import systems.zlink.samples.tictactoe.shared.contracts.JoinGameRes;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerInfo;
import systems.zlink.samples.tictactoe.shared.contracts.TicTacToeGameJoinRes;

public final class PlayActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;
    private PlayerInfo player;
    private String joinedRoomId;
    private String pendingRoomId;
    private final java.util.Set<ZLinkActorJoinOperationId>
        completedJoinOperations = new java.util.HashSet<>();
    private boolean destroyAfterEntrySpotJoin;
    private boolean disconnected;

    public PlayActor(String actorId, ZLinkActorContext context) {
        this.actorId = actorId;
        this.context = context;
    }

    public String actorId() {
        return actorId;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    public void applyPlayer(PlayerInfo player) {
        if (!actorId.equals(player.actorId())) {
            throw new IllegalArgumentException("player actor id does not match actor");
        }
        this.player = player;
    }

    public PlayerInfo requirePlayer() {
        if (player == null) {
            throw new IllegalStateException("actor has not been authenticated");
        }
        return player;
    }

    public PlayerInfo playerOrNull() {
        return player;
    }

    public int incrementWins() {
        PlayerInfo current = requirePlayer();
        PlayerInfo updated = new PlayerInfo(
            current.actorId(),
            current.displayName(),
            current.level(),
            current.wins() + 1);
        this.player = updated;
        return updated.wins();
    }

    public void joinGame(String roomId) {
        this.joinedRoomId = roomId;
    }

    public void trackDeferredJoin(String roomId) {
        if (pendingRoomId != null) {
            throw new IllegalStateException("a room join is already pending");
        }
        pendingRoomId = roomId;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onJoinCompleted(
        ZLinkActorJoinCompletion completion) {
        ZLinkActorJoinOperationId operationId = completion
            instanceof ZLinkActorJoinCompletion.Accepted accepted
                ? accepted.operationId()
                : completion instanceof ZLinkActorJoinCompletion.Rejected rejected
                    ? rejected.operationId()
                    : ((ZLinkActorJoinCompletion.Failed) completion).operationId();
        if (!completedJoinOperations.add(operationId)) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        String roomId = pendingRoomId;
        pendingRoomId = null;
        if (completion instanceof ZLinkActorJoinCompletion.Accepted accepted) {
            TicTacToeGameJoinRes reply = accepted.reply()
                .decode(TicTacToeGameJoinRes.class);
            if (roomId == null || roomId.isBlank()) {
                // A remote join completes on the relocated Actor. The target
                // reconstructs the room from the durable accepted reply.
                roomId = reply.state().roomId();
            }
            joinGame(roomId);
            return context.boundSession()
                .send(new JoinGameRes(reply.state()))
                .submit();
        }
        if (roomId == null) {
            roomId = "";
        }
        if (completion instanceof ZLinkActorJoinCompletion.Rejected) {
            return context.boundSession()
                .send(new JoinGameFailedNotify(roomId, "Rejected", false))
                .submit();
        }
        var failed = (ZLinkActorJoinCompletion.Failed) completion;
        return context.boundSession()
            .send(new JoinGameFailedNotify(
                roomId, failed.kind().name(), false))
            .submit();
    }

    public String requireJoinedGame() {
        if (joinedRoomId == null || joinedRoomId.isEmpty()) {
            throw new IllegalStateException("actor has not joined a room");
        }
        return joinedRoomId;
    }

    public String joinedRoomId() {
        return joinedRoomId;
    }

    public boolean destroyAfterEntrySpotJoin() {
        return destroyAfterEntrySpotJoin;
    }

    public void markForDestroyAfterRoomLeave() {
        destroyAfterEntrySpotJoin = true;
    }

    public boolean disconnected() {
        return disconnected;
    }

    public void markDisconnected() {
        disconnected = true;
    }
}
