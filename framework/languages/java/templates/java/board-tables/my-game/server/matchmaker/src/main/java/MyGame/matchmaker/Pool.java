package MyGame.matchmaker;

import MyGame.*;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.stream.Collectors;

public final class Pool implements ZLinkInstanceSpot {
    private static final Logger LOG = LoggerFactory.getLogger(Pool.class);
    private final ZLinkInstanceSpotContext context;
    private final ZLinkSpotManager spots;
    private final ZLinkActorClient actors;
    private final MatchQueue queue = new MatchQueue();

    public Pool(ZLinkInstanceSpotContext context, ZLinkSpotManager spots, ZLinkActorClient actors) {
        this.context = context;
        this.spots = spots;
        this.actors = actors;
    }

    public ZLinkInstanceSpotContext context() {
        return context;
    }

    public void configure() {
        context.handlers().addPacket(PoolPackets.Prepare.class);
        context.handlers().addPacket(PoolPackets.Enter.class);
        context.handlers().addPacket(PoolPackets.Prepared.class);
    }

    public CompletionStage<Void> onInitialize() {
        return context.addTimer(
                        "match",
                        Duration.ofMillis(500),
                        PoolPackets.Tick.class,
                        new ZLinkTimerOptions(ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false))
                .thenAccept(timer -> {});
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
        if (assignment == null
                || assignment.entered == null
                || !assignment.id.equals(report.assignmentId()))
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
                    if (assignment == null) return CompletableFuture.completedFuture(null);
                    var members =
                            assignment.members.stream().map(MatchQueue.Ticket::playerId).toList();
                    // Creation yields from a packet turn so the recurring timer can finish.
                    return context.outbound()
                            .sendToSpot(context.spotId(), new PrepareRoom(assignment.id, members))
                            .instanceSpot(Names.POOL)
                            .submit();
                });
    }

    CompletionStage<Void> prepare(PrepareRoom message) {
        var assignment = queue.findAssignment(message.assignmentId());
        if (assignment == null) return CompletableFuture.completedFuture(null);
        return spots.getOrCreate(assignment.id, Names.TABLE)
                .inMesh(Names.MESH)
                .yield()
                .thenCompose(
                        result -> {
                            if (queue.findAssignment(assignment.id) == null)
                                return CompletableFuture.completedFuture(true);
                            if (System.nanoTime() - assignment.deadline >= 0
                                    || result.state() == ZLinkSpotCreateState.REJECTED)
                                return CompletableFuture.completedFuture(false);
                            return context.outbound()
                                    .sendToSpot(assignment.id, message)
                                    .submit()
                                    .thenApply(ignored -> true);
                        })
                .handle(
                        (submitted, failure) -> {
                            if (failure == null) return submitted;
                            var cause =
                                    failure instanceof CompletionException
                                            ? failure.getCause()
                                            : failure;
                            if (!(cause instanceof ZLinkFrameworkException))
                                throw new CompletionException(cause);
                            LOG.warn(
                                    "Room preparation submission failed for {}",
                                    assignment.id,
                                    cause);
                            return false;
                        })
                .thenCompose(
                        submitted ->
                                submitted
                                        ? CompletableFuture.completedFuture(null)
                                        : prepared(new RoomPrepared(assignment.id, false)));
    }

    CompletionStage<Void> prepared(RoomPrepared message) {
        var assignment = queue.findAssignment(message.assignmentId());
        if (assignment == null) return withdrawLatePreparation(message);
        if (assignment.entered != null) return CompletableFuture.completedFuture(null);
        if (System.nanoTime() - assignment.deadline >= 0)
            return expire(assignment).thenCompose(ignored -> withdrawLatePreparation(message));
        if (!message.ok()) return failPreparation(assignment);

        assignment.entered = new HashSet<>();
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (var ticket : assignment.members)
            sends =
                    sends.thenCompose(
                            ignored ->
                                    notify(
                                            ticket,
                                            new MatchAssigned(
                                                    ticket.id(), assignment.id, assignment.id)));
        return sends;
    }

    private CompletionStage<Void> withdrawLatePreparation(RoomPrepared message) {
        return message.ok()
                ? context.outbound()
                        .sendToSpot(
                                message.assignmentId(), new WithdrawRoom(message.assignmentId()))
                        .submit()
                : CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> failPreparation(MatchQueue.Assignment assignment) {
        return withdraw(
                assignment,
                assignment.members.stream().map(MatchQueue.Ticket::id).collect(Collectors.toSet()),
                MatchCancelled.CANCELLED,
                true);
    }

    private CompletionStage<Void> expire(MatchQueue.Assignment assignment) {
        var failed = new HashSet<String>();
        for (var ticket : assignment.members)
            if (assignment.entered == null || !assignment.entered.contains(ticket.id()))
                failed.add(ticket.id());
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
        CompletionStage<Void> sends =
                assignment.entered == null
                        ? CompletableFuture.completedFuture(null)
                        : context.outbound().sendToSpot(assignment.id, withdrawn).submit();
        for (var ticket : assignment.members) {
            if (assignment.entered != null)
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
