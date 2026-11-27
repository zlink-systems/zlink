package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermit;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Connection;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ConnectionRef;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Delivery;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.EnqueueReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReport;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReportReply;

import java.security.SecureRandom;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Base64;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.LongSupplier;

/**
 * Memory of one queue Instance Spot incarnation (Authentication Admission §3.5.1-§3.5.3): epoch,
 * waiters, node budgets and global-limit permits. The Spot's serial execution owns it; every method
 * is also synchronized so the conformance tests can drive it directly.
 */
final class ZLinkAdmissionQueueState {
    static final long TICKET_LIFETIME_MS = 30_000;
    static final long REPORT_VALIDITY_MS = 3_000;
    static final long POSITION_PUSH_MS = 5_000;
    private static final long ESTIMATE_WINDOW_MS = 30_000;
    private static final long MAX_ESTIMATE_MS = Integer.MAX_VALUE;
    private static final Base64.Decoder DECODER = Base64.getUrlDecoder();
    private static final SecureRandom RANDOM = new SecureRandom();

    /** AdmitPermit outcome; a refusal reaches the node as the unavailable result. */
    record PermitResult(boolean granted, long permitId) {}

    private record NodeKey(String nodeRid, long generation) {}

    private static final class Node {
        int limit;
        int inFlight;
        long reportedAtMs = Long.MIN_VALUE;
        int issuedSinceReport;
    }

    private static final class Entry {
        final Connection waiter;
        String ticket;
        long ticketExpiresAtMs;
        boolean ticketPushed;
        long lastPushedPosition = -1;
        long lastPushedEstimate = -1;
        long lastPushedAtMs = Long.MIN_VALUE;

        Entry(Connection waiter) {
            this.waiter = waiter;
        }

        NodeKey node() {
            return new NodeKey(waiter.nodeRid(), waiter.nodeLifecycleGeneration());
        }
    }

    private record Permit(NodeKey node, String ownerId, long leaseGeneration, long requestSeq) {}

    private final ZLinkAdmissionTickets tickets;
    private final Integer globalLimit;
    private final LongSupplier wallClockMs;
    private final long epoch;
    private final Map<Connection, Entry> waiters = new LinkedHashMap<>();
    private final Map<NodeKey, Node> nodes = new HashMap<>();
    private final ArrayDeque<Long> issuedAtMs = new ArrayDeque<>();
    private final Map<Long, Permit> permits = new HashMap<>();

    ZLinkAdmissionQueueState(
            ZLinkAdmissionTickets tickets,
            Integer globalLimit,
            LongSupplier wallClockMs,
            long epoch) {
        this.tickets = tickets;
        this.globalLimit = globalLimit;
        this.wallClockMs = wallClockMs;
        this.epoch = epoch;
    }

    /** A non-zero 64-bit epoch from the CSPRNG (§3.5.1). */
    static long newEpoch() {
        long value;
        do {
            value = RANDOM.nextLong();
        } while (value == 0);
        return value;
    }

    long epoch() {
        return epoch;
    }

    /** {@code ceil(position * 1000 / r)} with r = tickets issued in the last 30 s / 30 (§3.5.3). */
    static long estimatedWaitMs(long position, long ticketsLast30Seconds) {
        long estimate =
                ticketsLast30Seconds <= 0
                        ? position * 1000
                        : Math.ceilDiv(position * 1000 * 30, ticketsLast30Seconds);
        return Math.min(estimate, MAX_ESTIMATE_MS);
    }

    synchronized NodeReportReply report(NodeReport report) {
        long now = wallClockMs.getAsLong();
        sweepExpired(now);
        NodeKey key = new NodeKey(report.nodeRid(), report.nodeLifecycleGeneration());
        Node node = nodes.computeIfAbsent(key, ignored -> new Node());
        node.limit = report.limit();
        node.inFlight = report.inFlight();
        node.reportedAtMs = now;
        node.issuedSinceReport = 0;
        for (ConnectionRef used : report.usedTickets()) {
            waiters.remove(waiter(key, used));
        }
        for (ConnectionRef ended : report.endedSessions()) {
            waiters.remove(waiter(key, ended));
        }
        reclaimMissingPermits(key, report);
        issue(key, node, now);
        List<Delivery> pushes = new ArrayList<>();
        int position = 0;
        for (Entry entry : waiters.values()) {
            // position counts the ticketless waiters ahead plus one (§3.5.3).
            if (entry.ticket == null) {
                position++;
            }
            if (!entry.node().equals(key)) {
                continue;
            }
            Delivery push = pushFor(entry, position, now);
            if (push != null) {
                pushes.add(push);
            }
        }
        return new NodeReportReply(epoch, pushes);
    }

    /** Enqueue is idempotent per connection identity (§3.5.3). */
    synchronized EnqueueReply enqueue(Connection waiter) {
        long now = wallClockMs.getAsLong();
        sweepExpired(now);
        Entry entry = waiters.computeIfAbsent(waiter, Entry::new);
        NodeKey key = entry.node();
        Node node = nodes.get(key);
        if (node != null) {
            issue(key, node, now);
        }
        if (entry.ticket != null) {
            entry.ticketPushed = true;
            return new EnqueueReply(
                    epoch,
                    new Delivery(
                            waiter.streamName(),
                            waiter.sessionRid(),
                            Delivery.TICKET,
                            null,
                            null,
                            entry.ticket,
                            (int) (entry.ticketExpiresAtMs - now)));
        }
        long position = positionOf(waiter);
        long estimate = estimate(position, now);
        entry.lastPushedPosition = position;
        entry.lastPushedEstimate = estimate;
        entry.lastPushedAtMs = now;
        return new EnqueueReply(
                epoch,
                new Delivery(
                        waiter.streamName(),
                        waiter.sessionRid(),
                        Delivery.POSITION,
                        (int) position,
                        (int) estimate,
                        null,
                        null));
    }

    synchronized PermitResult admitPermit(AdmitPermit request) {
        long now = wallClockMs.getAsLong();
        Connection key =
                new Connection(
                        request.nodeRid(),
                        request.nodeLifecycleGeneration(),
                        request.streamName(),
                        request.sessionRid());
        Entry holder = waiters.get(key);
        // Only a connection that holds a valid ticket (issued, unexpired, not invalidated) is
        // considered for a permit (§3.5.3).
        if (holder == null || holder.ticket == null || now > holder.ticketExpiresAtMs) {
            return new PermitResult(false, 0);
        }
        if (request.epoch() != epoch || (globalLimit != null && permits.size() >= globalLimit)) {
            returnWaiterToNodeFront(key);
            return new PermitResult(false, 0);
        }
        long id;
        do {
            id = RANDOM.nextLong();
        } while (id == 0 || permits.containsKey(id));
        permits.put(
                id,
                new Permit(
                        new NodeKey(request.nodeRid(), request.nodeLifecycleGeneration()),
                        request.ownerId(),
                        request.leaseGeneration(),
                        request.requestSeq()));
        return new PermitResult(true, id);
    }

    /** A denied connection loses its ticket and goes back to the front of its node FIFO. */
    private void returnWaiterToNodeFront(Connection key) {
        Entry denied = waiters.get(key);
        if (denied == null) {
            return;
        }
        denied.ticket = null;
        denied.ticketPushed = false;
        denied.lastPushedAtMs = Long.MIN_VALUE;
        List<Entry> ordered = new ArrayList<>(waiters.values());
        ordered.remove(denied);
        int front = 0;
        while (front < ordered.size() && !ordered.get(front).node().equals(denied.node())) {
            front++;
        }
        ordered.add(front, denied);
        waiters.clear();
        ordered.forEach(entry -> waiters.put(entry.waiter, entry));
    }

    synchronized void releasePermit(long permitId) {
        permits.remove(permitId);
    }

    /** The node owner lease ended: every permit it held is reclaimed (§3.5.3). */
    synchronized void ownerLeaseEnded(String ownerId, long leaseGeneration) {
        permits.values()
                .removeIf(
                        permit ->
                                permit.ownerId().equals(ownerId)
                                        && permit.leaseGeneration() == leaseGeneration);
    }

    /**
     * Owner leases to check in the Store while handling this report: only when the global cap keeps
     * a ticket from a waiting connection of the reporting node (§3.5.3), never otherwise.
     */
    synchronized Set<Map.Entry<String, Long>> ownersToCheckForReport(NodeReport report) {
        if (globalLimit == null || permits.isEmpty()) {
            return Set.of();
        }
        NodeKey key = new NodeKey(report.nodeRid(), report.nodeLifecycleGeneration());
        Set<Connection> gone = new HashSet<>();
        report.usedTickets().forEach(ref -> gone.add(waiter(key, ref)));
        report.endedSessions().forEach(ref -> gone.add(waiter(key, ref)));
        long ticketless =
                waiters.values().stream()
                        .filter(
                                entry ->
                                        entry.node().equals(key)
                                                && entry.ticket == null
                                                && !gone.contains(entry.waiter))
                        .count();
        long nodeBudget = Math.max(0, report.limit() - report.inFlight());
        return globalCap() < Math.min(nodeBudget, ticketless) ? permitOwners() : Set.of();
    }

    /** Owner leases that currently hold at least one permit, for the liveness sweep. */
    synchronized Set<Map.Entry<String, Long>> permitOwners() {
        Set<Map.Entry<String, Long>> owners = new HashSet<>();
        permits.values()
                .forEach(
                        permit ->
                                owners.add(Map.entry(permit.ownerId(), permit.leaseGeneration())));
        return owners;
    }

    synchronized int runningPermits() {
        return permits.size();
    }

    synchronized List<Connection> waitersInOrder() {
        return List.copyOf(waiters.keySet());
    }

    synchronized long positionOf(Connection waiter) {
        long position = 0;
        for (Entry candidate : waiters.values()) {
            if (candidate.ticket == null) {
                position++;
            }
            if (candidate.waiter.equals(waiter)) {
                return position;
            }
        }
        return -1;
    }

    synchronized int budget(String nodeRid, long generation) {
        Node node = nodes.get(new NodeKey(nodeRid, generation));
        return node == null ? 0 : Math.min(budget(node, wallClockMs.getAsLong()), globalCap());
    }

    synchronized String ticketOf(Connection waiter) {
        Entry entry = waiters.get(waiter);
        return entry == null ? null : entry.ticket;
    }

    /** Sets a node's last report as if it arrived now; conformance setup only. */
    synchronized void seedNode(
            String nodeRid, long generation, int limit, int inFlight, int issuedSinceReport) {
        Node node = nodes.computeIfAbsent(new NodeKey(nodeRid, generation), ignored -> new Node());
        node.limit = limit;
        node.inFlight = inFlight;
        node.reportedAtMs = wallClockMs.getAsLong();
        node.issuedSinceReport = issuedSinceReport;
    }

    /** Seeds a permit that a node already holds; conformance setup only. */
    synchronized void seedPermit(
            long id,
            String nodeRid,
            long generation,
            String ownerId,
            long leaseGeneration,
            long requestSeq) {
        permits.put(
                id,
                new Permit(new NodeKey(nodeRid, generation), ownerId, leaseGeneration, requestSeq));
    }

    /** Issues tickets to a node's waiters now, in enqueue order within the node's budget. */
    synchronized List<Connection> issueNow(String nodeRid, long generation) {
        long now = wallClockMs.getAsLong();
        NodeKey key = new NodeKey(nodeRid, generation);
        Node node = nodes.get(key);
        return node == null ? List.of() : issue(key, node, now);
    }

    /** Gives a waiting connection a ticket as if issued earlier; conformance setup only. */
    synchronized String seedTicket(Connection waiter) {
        Entry entry = waiters.computeIfAbsent(waiter, Entry::new);
        entry.ticketExpiresAtMs = wallClockMs.getAsLong() + TICKET_LIFETIME_MS;
        entry.ticket = tickets.issue(connection(waiter), entry.ticketExpiresAtMs, epoch);
        return entry.ticket;
    }

    synchronized void sweep() {
        sweepExpired(wallClockMs.getAsLong());
    }

    /** Global-limit part of the ticket budget (§3.5.3): limit - permits - unused tickets. */
    private int globalCap() {
        if (globalLimit == null) {
            return Integer.MAX_VALUE;
        }
        long unusedTickets =
                waiters.values().stream().filter(entry -> entry.ticket != null).count();
        return (int) Math.max(0, globalLimit - permits.size() - unusedTickets);
    }

    private int budget(Node node, long now) {
        if (now - node.reportedAtMs > REPORT_VALIDITY_MS) {
            return 0;
        }
        return Math.max(0, node.limit - node.inFlight - node.issuedSinceReport);
    }

    private List<Connection> issue(NodeKey key, Node node, long now) {
        List<Connection> issued = new ArrayList<>();
        int budget = Math.min(budget(node, now), globalCap());
        for (Entry entry : waiters.values()) {
            if (budget == 0) {
                break;
            }
            if (!entry.node().equals(key) || entry.ticket != null) {
                continue;
            }
            entry.ticketExpiresAtMs = now + TICKET_LIFETIME_MS;
            entry.ticket = tickets.issue(connection(entry.waiter), entry.ticketExpiresAtMs, epoch);
            entry.ticketPushed = false;
            node.issuedSinceReport++;
            issuedAtMs.addLast(now);
            budget--;
            issued.add(entry.waiter);
        }
        return issued;
    }

    private Delivery pushFor(Entry entry, long position, long now) {
        Connection waiter = entry.waiter;
        if (entry.ticket != null) {
            if (entry.ticketPushed) {
                return null;
            }
            entry.ticketPushed = true;
            return new Delivery(
                    waiter.streamName(),
                    waiter.sessionRid(),
                    Delivery.TICKET,
                    null,
                    null,
                    entry.ticket,
                    (int) (entry.ticketExpiresAtMs - now));
        }
        long estimate = estimate(position, now);
        if (position == entry.lastPushedPosition
                && estimate == entry.lastPushedEstimate
                && now - entry.lastPushedAtMs < POSITION_PUSH_MS) {
            return null;
        }
        entry.lastPushedPosition = position;
        entry.lastPushedEstimate = estimate;
        entry.lastPushedAtMs = now;
        return new Delivery(
                waiter.streamName(),
                waiter.sessionRid(),
                Delivery.POSITION,
                (int) position,
                (int) estimate,
                null,
                null);
    }

    private long estimate(long position, long now) {
        while (!issuedAtMs.isEmpty() && now - issuedAtMs.peekFirst() > ESTIMATE_WINDOW_MS) {
            issuedAtMs.removeFirst();
        }
        return estimatedWaitMs(position, issuedAtMs.size());
    }

    private void sweepExpired(long now) {
        for (Iterator<Entry> it = waiters.values().iterator(); it.hasNext(); ) {
            Entry entry = it.next();
            if (entry.ticket != null && now > entry.ticketExpiresAtMs) {
                it.remove();
            }
        }
    }

    private void reclaimMissingPermits(NodeKey key, NodeReport report) {
        Set<Long> held = new HashSet<>(report.heldPermits());
        permits.entrySet()
                .removeIf(
                        entry ->
                                entry.getValue().node().equals(key)
                                        && entry.getValue().requestSeq()
                                                <= report.resolvedRequestSeq()
                                        && !held.contains(entry.getKey()));
    }

    private static Connection waiter(NodeKey node, ConnectionRef ref) {
        return new Connection(
                node.nodeRid(), node.generation(), ref.streamName(), ref.sessionRid());
    }

    static ZLinkAdmissionTickets.TicketConnection connection(Connection waiter) {
        return new ZLinkAdmissionTickets.TicketConnection(
                DECODER.decode(waiter.nodeRid()),
                waiter.nodeLifecycleGeneration(),
                waiter.streamName(),
                DECODER.decode(waiter.sessionRid()));
    }
}
