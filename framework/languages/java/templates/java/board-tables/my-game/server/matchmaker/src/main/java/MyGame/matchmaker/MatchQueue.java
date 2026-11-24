package MyGame.matchmaker;

import java.time.Duration;
import java.util.*;

final class MatchQueue {
    private static final long WAIT_LIMIT = Duration.ofSeconds(120).toNanos();
    private static final long ENTER_LIMIT = Duration.ofSeconds(10).toNanos();

    record Ticket(String id, String playerId, long expires) {}

    static final class Assignment {
        final String id;
        final List<Ticket> members;
        final long deadline;
        final Set<String> entered = new HashSet<>();

        Assignment(String id, List<Ticket> members, long deadline) {
            this.id = id;
            this.members = members;
            this.deadline = deadline;
        }
    }

    private final MatchRule rule = new MatchRule();
    private final Map<String, Ticket> players = new HashMap<>();
    private final Map<String, Ticket> tickets = new HashMap<>();
    private final LinkedHashSet<Ticket> waiting = new LinkedHashSet<>();
    private final Map<String, Assignment> assignments = new LinkedHashMap<>();
    private final Map<String, Assignment> reserved = new HashMap<>();

    Ticket enqueue(String requestKey, String playerId, long now) {
        var active = players.get(playerId);
        if (active != null) return active;
        var ticket = new Ticket(requestKey, playerId, now + WAIT_LIMIT);
        tickets.put(ticket.id(), ticket);
        players.put(playerId, ticket);
        waiting.addLast(ticket);
        return ticket;
    }

    Assignment pair(String assignmentId, long now) {
        var members = rule.choose(waiting.stream());
        if (members.isEmpty()) return null;
        var assignment = new Assignment(assignmentId, members, now + ENTER_LIMIT);
        assignments.put(assignment.id, assignment);
        for (var ticket : members) {
            waiting.remove(ticket);
            reserved.put(ticket.id(), assignment);
        }
        return assignment;
    }

    List<Ticket> expireWaiting(long now) {
        var expired = new ArrayList<Ticket>();
        waiting.removeIf(
                ticket -> {
                    if (now - ticket.expires() < 0) return false;
                    finish(ticket);
                    expired.add(ticket);
                    return true;
                });
        return expired;
    }

    List<Assignment> expireAssignments(long now) {
        return assignments.values().stream().filter(a -> now - a.deadline >= 0).toList();
    }

    Assignment assignment(String ticketId) {
        return reserved.get(ticketId);
    }

    Ticket cancelWaiting(String ticketId) {
        var ticket = tickets.get(ticketId);
        if (ticket == null || !waiting.remove(ticket)) return null;
        finish(ticket);
        return ticket;
    }

    void settle(Assignment assignment, Set<String> survivors) {
        assignments.remove(assignment.id);
        for (var ticket : assignment.members.reversed()) {
            reserved.remove(ticket.id());
            if (survivors.contains(ticket.id())) waiting.addFirst(ticket);
            else finish(ticket);
        }
    }

    private void finish(Ticket ticket) {
        players.remove(ticket.playerId());
        tickets.remove(ticket.id());
    }
}
