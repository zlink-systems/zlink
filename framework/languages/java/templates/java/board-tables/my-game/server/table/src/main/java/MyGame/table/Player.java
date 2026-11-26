package MyGame.table;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.channels.ZLinkRouteClient;

import java.util.UUID;
import java.util.concurrent.*;

public final class Player implements ZLinkActor {
    private static final String LEGACY_JOIN = "legacy";
    private static final String RETURN_TO_ENTRY = "entry";

    private final ZLinkActorContext context;
    private final ZLinkRouteClient routes;
    PlayerState state = new PlayerState();

    public Player(ZLinkActorContext context, ZLinkRouteClient routes) {
        this.context = context;
        this.routes = routes;
    }

    public ZLinkActorContext context() {
        return context;
    }

    CompletionStage<Void> loadPlayer(PlayerApi players) {
        if (state.chips != null) return CompletableFuture.completedFuture(null);
        return players.loadPlayer(context.actorId()).thenAccept(chips -> state.chips = chips);
    }

    CompletionStage<Void> find() {
        state.disconnected = false;
        if (state.ticketId != null) return reply(new MatchQueued(state.ticketId));
        return routes.requestToSpot(
                        Names.POOL_KEY,
                        new MatchRequest(UUID.randomUUID().toString(), context.actorId()))
                .instanceSpot(Names.POOL)
                .inMesh(Names.MESH)
                .submit(MatchQueued.class)
                .thenCompose(
                        queued -> {
                            state.ticketId = queued.ticketId();
                            state.withdrawnAssignments.clear();
                            System.out.println(
                                    "Player "
                                            + GameLog.shortId(context.actorId())
                                            + " queued for a match (ticket "
                                            + GameLog.shortId(queued.ticketId())
                                            + ")");
                            return reply(queued);
                        });
    }

    CompletionStage<Void> cancel(CancelMatch request) {
        if (!isCurrent(request.ticketId())) return CompletableFuture.completedFuture(null);
        return cancelTicket(MatchCancelled.CANCELLED);
    }

    CompletionStage<Void> disconnected() {
        state.disconnected = true;
        if (state.ticketId == null) return CompletableFuture.completedFuture(null);
        return cancelTicket(MatchCancelled.DISCONNECTED);
    }

    private CompletionStage<Void> cancelTicket(String reason) {
        return routes.requestToSpot(Names.POOL_KEY, new CancelTicket(state.ticketId, reason))
                .instanceSpot(Names.POOL)
                .inMesh(Names.MESH)
                .submit(MatchCancelled.class)
                .thenCompose(
                        message ->
                                message == null
                                        ? CompletableFuture.completedFuture(null)
                                        : cancelled(message));
    }

    CompletionStage<Void> cancelled(MatchCancelled message) {
        if (!isCurrent(message.ticketId())) return CompletableFuture.completedFuture(null);
        state.ticketId = null;
        System.out.println(
                "Player "
                        + GameLog.shortId(context.actorId())
                        + " left the match queue ("
                        + message.reason()
                        + ")");
        return state.disconnected ? CompletableFuture.completedFuture(null) : reply(message);
    }

    void assigned(MatchAssigned message) {
        if (!isCurrent(message.ticketId())
                || state.assignment != null
                || state.withdrawnAssignments.contains(message.assignmentId())) return;
        state.assignment = message;
        continueJoining();
    }

    void withdrawn(WithdrawRoom message) {
        if (state.ticketId != null) state.withdrawnAssignments.add(message.assignmentId());
        if (state.assignment == null
                || !state.assignment.assignmentId().equals(message.assignmentId())) return;
        state.withdrawnRoom = state.assignment.roomId();
        state.assignment = null;
        continueJoining();
    }

    void joinTable(String tableId) {
        requestAdmission(tableId);
        state.joining = LEGACY_JOIN;
    }

    private void continueJoining() {
        if (state.joining != null) return;
        if (state.withdrawnRoom != null) {
            var withdrawnRoom = state.withdrawnRoom;
            state.withdrawnRoom = null;
            if (context.spotId().filter(withdrawnRoom::equals).isPresent()) {
                context.joinEntrySpot().defer();
                state.joining = RETURN_TO_ENTRY;
                return;
            }
        }
        var assignment = state.assignment;
        if (assignment != null && isCurrent(assignment.ticketId())) {
            requestAdmission(assignment.roomId());
            state.joining = assignment.assignmentId();
        }
    }

    private void requestAdmission(String tableId) {
        context.joinSpot(tableId, new TableAdmission(context.actorId(), tableId, state.chips))
                .defer();
    }

    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        var joining = state.joining;
        state.joining = null;
        if (joining == null) return CompletableFuture.completedFuture(null);
        boolean joined = completion instanceof ZLinkActorJoinCompletion.Accepted;
        if (LEGACY_JOIN.equals(joining)) {
            continueJoining();
            return switch (completion) {
                case ZLinkActorJoinCompletion.Accepted accepted ->
                        CompletableFuture.completedFuture(null);
                case ZLinkActorJoinCompletion.Rejected rejected ->
                        reply(new ActionRejected(Rules.TABLE_FULL, 0));
                case ZLinkActorJoinCompletion.Failed failed ->
                        CompletableFuture.failedFuture(
                                new IllegalStateException("Join failed: " + failed.kind()));
            };
        }
        if (RETURN_TO_ENTRY.equals(joining)) {
            if (!joined)
                return CompletableFuture.failedFuture(
                        new IllegalStateException("Return to Entry failed: " + completion));
            continueJoining();
            return CompletableFuture.completedFuture(null);
        }
        var assignment = state.assignment;
        if (assignment == null || !assignment.assignmentId().equals(joining)) {
            continueJoining();
            return CompletableFuture.completedFuture(null);
        }
        if (state.disconnected) return CompletableFuture.completedFuture(null);
        return poolSend(new EnterReport(assignment.ticketId(), joining, joined));
    }

    CompletionStage<Void> tableView(TableView view) {
        var assignment = state.assignment;
        if (assignment == null
                || !assignment.roomId().equals(view.tableId())
                || view.yourSeat() < 0) return reply(view);
        if (!isCurrent(assignment.ticketId())) return CompletableFuture.completedFuture(null);
        state.assignment = null;
        state.ticketId = null;
        if (state.disconnected) return CompletableFuture.completedFuture(null);
        return reply(new MatchFound(assignment.ticketId(), view.tableId(), view.yourSeat()))
                .thenCompose(ignored -> reply(view));
    }

    CompletionStage<Void> settled(HandResult result) {
        state.chips =
                result.seats().stream()
                        .filter(seat -> context.actorId().equals(seat.playerId()))
                        .findFirst()
                        .orElseThrow()
                        .chips();
        return reply(result);
    }

    CompletionStage<Void> reply(Object message) {
        return context.boundSession().send(message).submit();
    }

    private CompletionStage<Void> poolSend(Object message) {
        return routes.sendToSpot(Names.POOL_KEY, message)
                .instanceSpot(Names.POOL)
                .inMesh(Names.MESH)
                .submit();
    }

    private boolean isCurrent(String ticketId) {
        return ticketId != null && ticketId.equals(state.ticketId);
    }
}
