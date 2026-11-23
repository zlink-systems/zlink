package MyGame.matchmaker;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.stream.Collectors;

public final class Pool implements ZLinkSpot<ZLinkActor> {
    private final ZLinkSpotContext context;
    private final ZLinkSpotManager spots;
    private final ZLinkActorClient actors;
    private final MatchQueue queue = new MatchQueue();

    public Pool(ZLinkSpotContext context, ZLinkSpotManager spots, ZLinkActorClient actors) {
        this.context = context;
        this.spots = spots;
        this.actors = actors;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public void configure() {
        context.handlers().addHandler(PoolPackets.Enter.class);
    }

    public CompletionStage<Void> onInitialize() {
        return context.addTimer(
                        "match",
                        Duration.ofMillis(500),
                        PoolPackets.Tick.class,
                        new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false))
                .thenAccept(timer -> {});
    }

    public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    CompletionStage<MatchQueued> request(MatchRequest request) {
        var ticket = queue.enqueue(request.requestKey(), request.playerId(), System.nanoTime());
        return tick().thenApply(ignored -> new MatchQueued(ticket.id()));
    }

    CompletionStage<MatchCancelled> cancel(CancelTicket request) {
        var result = new MatchCancelled(request.ticketId(), request.reason());
        var assignment = queue.assignment(request.ticketId());
        if (assignment != null) {
            if (MatchCancelled.DISCONNECTED.equals(request.reason()))
                return CompletableFuture.completedFuture(
                        null); // Reserved tickets settle by entry deadline.
            return withdraw(assignment, Set.of(request.ticketId()), request.reason(), false)
                    .thenApply(ignored -> result);
        }
        return CompletableFuture.completedFuture(
                queue.cancelWaiting(request.ticketId()) == null ? null : result);
    }

    CompletionStage<Void> report(EnterReport report) {
        var assignment = queue.assignment(report.ticketId());
        if (assignment == null || !assignment.id.equals(report.assignmentId()))
            return CompletableFuture.completedFuture(null);
        if (System.nanoTime() - assignment.deadline >= 0) return expire(assignment);
        if (!report.ok())
            return withdraw(assignment, Set.of(report.ticketId()), MatchCancelled.CANCELLED, true);
        if (!assignment.entered.add(report.ticketId())
                || assignment.entered.size() != assignment.members.size())
            return CompletableFuture.completedFuture(null);
        queue.settle(assignment, Set.of());
        var players =
                assignment.members.stream()
                        .map(ticket -> GameLog.shortId(ticket.playerId()))
                        .collect(Collectors.joining(", "));
        GameLog.table(assignment.id, "matched " + players);
        return context.outbound().sendToSpot(assignment.id, new CommitRoom(assignment.id)).submit();
    }

    CompletionStage<Void> tick() {
        long now = System.nanoTime();
        CompletionStage<Void> work = CompletableFuture.completedFuture(null);
        for (var ticket : queue.expireWaiting(now)) {
            work =
                    work.thenCompose(
                            ignored ->
                                    notify(
                                            ticket,
                                            new MatchCancelled(
                                                    ticket.id(), MatchCancelled.EXPIRED)));
        }
        for (var assignment : queue.expireAssignments(now))
            work = work.thenCompose(ignored -> expire(assignment));
        return work.thenCompose(
                ignored -> {
                    var assignment = queue.pair("room-" + UUID.randomUUID(), System.nanoTime());
                    return assignment == null
                            ? CompletableFuture.completedFuture(null)
                            : prepare(assignment);
                });
    }

    private CompletionStage<Void> prepare(MatchQueue.Assignment assignment) {
        var members = assignment.members.stream().map(MatchQueue.Ticket::playerId).toList();
        return spots.getOrCreate(assignment.id, Names.TABLE)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        result -> {
                            if (result.state() == ZLinkSpotCreateState.REJECTED)
                                return failPreparation(assignment);
                            return context.outbound()
                                    .requestToSpot(
                                            assignment.id, new PrepareRoom(assignment.id, members))
                                    .submit(RoomPrepared.class)
                                    .thenCompose(
                                            prepared -> {
                                                if (!prepared.ok()
                                                        || !assignment.id.equals(
                                                                prepared.assignmentId()))
                                                    return failPreparation(assignment);
                                                if (System.nanoTime() - assignment.deadline >= 0)
                                                    return expire(assignment);
                                                CompletionStage<Void> sends =
                                                        CompletableFuture.completedFuture(null);
                                                for (var ticket : assignment.members)
                                                    sends =
                                                            sends.thenCompose(
                                                                    ignored ->
                                                                            notify(
                                                                                    ticket,
                                                                                    new MatchAssigned(
                                                                                            ticket
                                                                                                    .id(),
                                                                                            assignment
                                                                                                    .id,
                                                                                            assignment
                                                                                                    .id)));
                                                return sends;
                                            });
                        });
    }

    private CompletionStage<Void> failPreparation(MatchQueue.Assignment assignment) {
        queue.settle(assignment, Set.of());
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var ticket : assignment.members)
            sends =
                    sends.thenCompose(
                            ignored ->
                                    notify(
                                            ticket,
                                            new MatchCancelled(
                                                    ticket.id(), MatchCancelled.CANCELLED)));
        return sends;
    }

    private CompletionStage<Void> expire(MatchQueue.Assignment assignment) {
        var failed = new HashSet<String>();
        for (var ticket : assignment.members)
            if (!assignment.entered.contains(ticket.id())) failed.add(ticket.id());
        return withdraw(assignment, failed, MatchCancelled.EXPIRED, true);
    }

    private CompletionStage<Void> withdraw(
            MatchQueue.Assignment assignment,
            Set<String> failed,
            String reason,
            boolean notifyFailed) {
        var survivors = new HashSet<String>();
        for (var ticket : assignment.members)
            if (!failed.contains(ticket.id())) survivors.add(ticket.id());
        queue.settle(assignment, survivors);
        var withdrawn = new WithdrawRoom(assignment.id);
        var sends = context.outbound().sendToSpot(assignment.id, withdrawn).submit();
        for (var ticket : assignment.members) {
            sends = sends.thenCompose(ignored -> notify(ticket, withdrawn));
            if (notifyFailed && failed.contains(ticket.id()))
                sends =
                        sends.thenCompose(
                                ignored -> notify(ticket, new MatchCancelled(ticket.id(), reason)));
        }
        return sends;
    }

    private CompletionStage<Void> notify(MatchQueue.Ticket ticket, Object message) {
        return actors.sendToActor(ticket.playerId(), message).submit();
    }
}
