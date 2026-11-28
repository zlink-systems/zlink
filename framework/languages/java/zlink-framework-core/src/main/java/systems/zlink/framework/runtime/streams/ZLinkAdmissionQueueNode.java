package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermit;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermitReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Connection;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ConnectionRef;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Delivery;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Enqueue;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.EnqueueReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReport;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReportReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ReleasePermit;

import java.util.ArrayList;
import java.util.Base64;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.IntSupplier;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/**
 * The STREAM node side of the login admission queue (Authentication Admission §3.4-§3.5.3): ticket
 * check before the authentication handler, the AdmitPermit/ReleasePermit calls of the global limit
 * and the NodeReport the queue Spot budgets from. The report cadence belongs to the STREAM
 * runtime's existing timer owner; this class starts no timer and no retry loop.
 */
final class ZLinkAdmissionQueueNode {
    static final String QUEUE_REQUIRED_METADATA = "$zlink.admission.queueRequired";
    private static final Base64.Encoder ENCODER = Base64.getUrlEncoder().withoutPadding();

    /** The Spot calls of §3.5.3; the runtime binds them to the queue Instance Spot. */
    interface SpotLink {
        CompletionStage<NodeReportReply> report(NodeReport report);

        CompletionStage<EnqueueReply> enqueue(Enqueue enqueue);

        CompletionStage<AdmitPermitReply> admitPermit(AdmitPermit request);

        void releasePermit(ReleasePermit release);
    }

    /** Node owner lease recorded with each permit (§3.5.3). */
    record OwnerLease(String ownerId, long leaseGeneration) {}

    record Identity(byte[] nodeRid, long nodeLifecycleGeneration) {}

    /** AdmitPermit outcome; {@link Granted#permitId()} is 0 when no global limit is set. */
    sealed interface PermitOutcome permits Granted, Denied, Unavailable {}

    record Granted(long permitId) implements PermitOutcome {}

    record Denied() implements PermitOutcome {}

    record Unavailable(Throwable cause) implements PermitOutcome {}

    private final ZLinkAdmissionTickets tickets;
    private final Integer globalLimit;
    private final Identity identity;
    private final String nodeRid;
    private final Supplier<OwnerLease> owner;
    private final IntSupplier stageOneLimit;
    private final IntSupplier stageOneInFlight;
    private final Consumer<Delivery> pushSink;
    private final SpotLink link;
    private final LongSupplier wallClockMs;

    private final Object lock = new Object();
    private volatile Long reportedEpoch;
    private long nextRequestSeq = 1;
    private final TreeSet<Long> pendingRequests = new TreeSet<>();
    private final Set<Long> heldPermits = new LinkedHashSet<>();
    private final Set<ConnectionRef> used = new LinkedHashSet<>();
    private final Set<ConnectionRef> ended = new LinkedHashSet<>();
    private boolean reporting;

    ZLinkAdmissionQueueNode(
            ZLinkAdmissionTickets tickets,
            Integer globalLimit,
            Identity identity,
            Supplier<OwnerLease> owner,
            IntSupplier stageOneLimit,
            IntSupplier stageOneInFlight,
            Consumer<Delivery> pushSink,
            SpotLink link,
            LongSupplier wallClockMs) {
        this.tickets = tickets;
        this.globalLimit = globalLimit;
        this.identity = identity;
        this.nodeRid = ENCODER.encodeToString(identity.nodeRid());
        this.owner = owner;
        this.stageOneLimit = stageOneLimit;
        this.stageOneInFlight = stageOneInFlight;
        this.pushSink = pushSink;
        this.link = link;
        this.wallClockMs = wallClockMs;
    }

    boolean hasGlobalLimit() {
        return globalLimit != null;
    }

    Long reportedEpoch() {
        return reportedEpoch;
    }

    /**
     * Ticket check that runs once, before the stage-one decision and the handler (§3.5.2). A node
     * that has no report Response yet accepts no ticket (§3.5.1).
     */
    boolean ticketAccepted(String streamName, byte[] sessionRid, String ticket) {
        Long epoch = reportedEpoch;
        if (epoch == null || ticket == null) {
            return false;
        }
        return tickets.verifies(
                ticket,
                new ZLinkAdmissionTickets.TicketConnection(
                        identity.nodeRid(),
                        identity.nodeLifecycleGeneration(),
                        streamName,
                        sessionRid),
                epoch,
                wallClockMs.getAsLong());
    }

    /**
     * One AdmitPermit per authentication before the handler (§3.5.3). Waits for the Framework Spot
     * request default timeout through {@link SpotLink}; no extra timer here.
     */
    CompletionStage<PermitOutcome> acquirePermit(String streamName, byte[] sessionRid) {
        if (globalLimit == null) {
            return CompletableFuture.completedFuture(new Granted(0));
        }
        Long epoch = reportedEpoch;
        if (epoch == null) {
            return CompletableFuture.completedFuture(new Denied());
        }
        long requestSeq;
        OwnerLease lease;
        try {
            lease = owner.get();
        } catch (RuntimeException failure) {
            return CompletableFuture.completedFuture(new Unavailable(failure));
        }
        synchronized (lock) {
            requestSeq = nextRequestSeq++;
            pendingRequests.add(requestSeq);
        }
        CompletionStage<AdmitPermitReply> call;
        try {
            call =
                    link.admitPermit(
                            new AdmitPermit(
                                    nodeRid,
                                    identity.nodeLifecycleGeneration(),
                                    lease.ownerId(),
                                    lease.leaseGeneration(),
                                    streamName,
                                    ENCODER.encodeToString(sessionRid),
                                    epoch,
                                    requestSeq));
        } catch (RuntimeException failure) {
            call = CompletableFuture.failedFuture(failure);
        }
        return call.handle(
                (reply, failure) -> {
                    synchronized (lock) {
                        pendingRequests.remove(requestSeq);
                        if (failure == null && reply != null && reply.granted()) {
                            heldPermits.add(reply.permitId());
                        }
                    }
                    if (failure != null || reply == null) {
                        return new Unavailable(failure);
                    }
                    return reply.granted()
                            ? new Granted(reply.permitId())
                            : (PermitOutcome) new Denied();
                });
    }

    /** The handler ended (success, failure or cancellation): give the permit back (§3.5.3). */
    void releasePermit(long permitId) {
        if (permitId == 0) {
            return;
        }
        synchronized (lock) {
            heldPermits.remove(permitId);
        }
        link.releasePermit(
                new ReleasePermit(nodeRid, identity.nodeLifecycleGeneration(), permitId));
    }

    /** A handler ran with a ticket: the next report names the connection as used (§3.5.3). */
    void ticketUsed(String streamName, byte[] sessionRid) {
        synchronized (lock) {
            used.add(new ConnectionRef(streamName, ENCODER.encodeToString(sessionRid)));
        }
    }

    /** A connection ended: the next report names it so the Spot drops its waiter (§3.5.3). */
    void sessionEnded(String streamName, byte[] sessionRid) {
        synchronized (lock) {
            ended.add(new ConnectionRef(streamName, ENCODER.encodeToString(sessionRid)));
        }
    }

    CompletionStage<EnqueueReply> enqueue(String streamName, byte[] sessionRid) {
        return link.enqueue(
                new Enqueue(
                        new Connection(
                                nodeRid,
                                identity.nodeLifecycleGeneration(),
                                streamName,
                                ENCODER.encodeToString(sessionRid))));
    }

    /** The NodeReport for the current state, without sending it. */
    NodeReport buildReport() {
        synchronized (lock) {
            return new NodeReport(
                    nodeRid,
                    identity.nodeLifecycleGeneration(),
                    stageOneLimit.getAsInt(),
                    stageOneInFlight.getAsInt(),
                    List.copyOf(used),
                    List.copyOf(ended),
                    List.copyOf(heldPermits),
                    pendingRequests.isEmpty() ? nextRequestSeq - 1 : pendingRequests.first() - 1);
        }
    }

    /** Sends one report; a report still waiting for its Response suppresses the next one. */
    CompletionStage<Void> reportOnce() {
        NodeReport report;
        synchronized (lock) {
            if (reporting) {
                return CompletableFuture.completedFuture(null);
            }
            reporting = true;
        }
        try {
            report = buildReport();
        } catch (RuntimeException failure) {
            synchronized (lock) {
                reporting = false;
            }
            return CompletableFuture.failedFuture(failure);
        }
        CompletionStage<NodeReportReply> call;
        try {
            call = link.report(report);
        } catch (RuntimeException failure) {
            call = CompletableFuture.failedFuture(failure);
        }
        return call.handle(
                (reply, failure) -> {
                    synchronized (lock) {
                        reporting = false;
                        if (failure == null && reply != null) {
                            used.removeAll(report.usedTickets());
                            ended.removeAll(report.endedSessions());
                            Long previous = reportedEpoch;
                            if (previous != null && !previous.equals(reply.epoch())) {
                                // A new queue incarnation counts none of the old permits (§3.5.3).
                                heldPermits.clear();
                            }
                            reportedEpoch = reply.epoch();
                        }
                    }
                    if (failure == null && reply != null) {
                        List<Delivery> pushes = new ArrayList<>(reply.deliveries());
                        pushes.forEach(pushSink);
                    }
                    return null;
                });
    }
}
