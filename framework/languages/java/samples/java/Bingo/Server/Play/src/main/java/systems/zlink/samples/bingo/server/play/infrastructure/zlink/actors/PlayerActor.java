package systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class PlayerActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;
    private String displayName;
    private String roomId = "";
    private String pendingRoomId;
    private final java.util.Set<ZLinkActorJoinOperationId>
        completedJoinOperations = new java.util.HashSet<>();
    private boolean destroyAfterEntrySpotJoin;
    private boolean disconnected;

    public PlayerActor(String actorId, ZLinkActorContext context) {
        this.actorId = actorId;
        this.context = context;
        this.displayName = actorId;
    }

    public String actorId() {
        return actorId;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    public String displayName() {
        return displayName;
    }

    public void setDisplayName(String displayName) {
        this.displayName = displayName;
    }

    public void joinRoom(String roomId) {
        this.roomId = roomId;
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

        String matchedRoomId = pendingRoomId;
        pendingRoomId = null;
        if (!(completion instanceof ZLinkActorJoinCompletion.Accepted accepted)) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }

        Messages.BingoRoomJoinRes joined = accepted.reply()
            .decode(Messages.BingoRoomJoinRes.class);
        if (matchedRoomId == null || matchedRoomId.isBlank()) {
            matchedRoomId = joined.getState().getRoomId();
        }
        joinRoom(matchedRoomId);
        return context.boundSession()
            .send(BingoMessages.matchBingoRes(matchedRoomId, joined.getState()))
            .submit();
    }

    public String roomId() {
        return roomId;
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

    public java.util.concurrent.CompletionStage<Void> push(Object message) {
        return context.boundSession()
            .send(message)
            .submit();
    }
}
