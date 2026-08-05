package systems.zlink.samples.supportchat.server.support.domain;

import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class Conversation {
    public enum Status { WaitingForAgent, Active, WaitingForClose, Closed }
    public enum Role { Customer, Agent }
    public enum EventKind { ParticipantJoined, MessageAppended, TypingChanged, Idle, Closed }

    public record Policy(Duration idleTimeout, Duration closeGraceTimeout, int maxMessageLength) {
    }

    public record Participant(
        String actorId,
        Role role,
        String displayName,
        long joinedAtUnixMs,
        boolean typing) {
    }

    public record Message(
        String conversationId,
        long messageSeq,
        String senderActorId,
        String text,
        long sentAtUnixMs) {
    }

    public record Snapshot(
        String conversationId,
        String subject,
        Status status,
        String customerActorId,
        String agentActorId,
        long lastMessageSeq,
        Long lastMessageAtUnixMs,
        Long idleDeadlineUnixMs) {
    }

    public record Event(
        EventKind kind,
        Snapshot state,
        String actorId,
        Role role,
        Message message,
        Boolean typing) {
    }

    public record Change(Snapshot state, List<Event> events) {
    }

    private final String conversationId;
    private final String subject;
    private final String customerActorId;
    private final Policy policy;
    private final Map<String, Participant> participants = new LinkedHashMap<>();
    private Status status = Status.WaitingForAgent;
    private String agentActorId;
    private long lastMessageSeq;
    private Long lastMessageAtUnixMs;
    private Long idleDeadlineUnixMs;
    private Long closeDeadlineUnixMs;

    public Conversation(
        String conversationId,
        String subject,
        String customerActorId,
        String customerDisplayName,
        long createdAtUnixMs,
        Policy policy) {
        if (subject == null || subject.isBlank()) {
            throw new IllegalArgumentException("Conversation subject is required");
        }
        this.conversationId = conversationId;
        this.subject = subject;
        this.customerActorId = customerActorId;
        this.policy = policy;
        participants.put(customerActorId, new Participant(
            customerActorId, Role.Customer, customerDisplayName, createdAtUnixMs, false));
    }

    public synchronized Snapshot snapshot() {
        return new Snapshot(
            conversationId, subject, status, customerActorId, agentActorId,
            lastMessageSeq, lastMessageAtUnixMs, idleDeadlineUnixMs);
    }

    public synchronized Change joinAgent(String actorId, String displayName, long now) {
        ensureOpen("join an agent");
        if (agentActorId != null && !agentActorId.equals(actorId)) {
            throw new IllegalStateException("Conversation already has an assigned agent");
        }
        agentActorId = actorId;
        status = Status.Active;
        participants.put(actorId, new Participant(actorId, Role.Agent, displayName, now, false));
        Snapshot state = snapshot();
        return change(new Event(EventKind.ParticipantJoined, state, actorId, Role.Agent, null, null));
    }

    public synchronized Change sendMessage(String senderActorId, String text, long now) {
        requireParticipant(senderActorId);
        if (status == Status.Closed) {
            throw new IllegalStateException("Closed conversation cannot accept messages");
        }
        if (status == Status.WaitingForAgent) {
            throw new IllegalStateException("Conversation is waiting for an agent");
        }
        if (text == null || text.isBlank()) {
            throw new IllegalArgumentException("Message text is required");
        }
        if (text.length() > policy.maxMessageLength()) {
            throw new IllegalArgumentException("Message text is too long");
        }
        status = Status.Active;
        lastMessageSeq += 1;
        lastMessageAtUnixMs = now;
        idleDeadlineUnixMs = now + policy.idleTimeout().toMillis();
        closeDeadlineUnixMs = null;
        Message message = new Message(conversationId, lastMessageSeq, senderActorId, text, now);
        Snapshot state = snapshot();
        return change(new Event(EventKind.MessageAppended, state, senderActorId, null, message, null));
    }

    public synchronized Change setTyping(String actorId, boolean typing) {
        Participant participant = participants.get(actorId);
        if (status == Status.Closed || participant == null) {
            return new Change(snapshot(), List.of());
        }
        participants.put(actorId, new Participant(
            participant.actorId(), participant.role(), participant.displayName(),
            participant.joinedAtUnixMs(), typing));
        Snapshot state = snapshot();
        return change(new Event(
            EventKind.TypingChanged, state, actorId, participant.role(), null, typing));
    }

    public synchronized Change markIdle(long now) {
        if (status == Status.Active && idleDeadlineUnixMs != null && now >= idleDeadlineUnixMs) {
            status = Status.WaitingForClose;
            closeDeadlineUnixMs = now + policy.closeGraceTimeout().toMillis();
            Snapshot state = snapshot();
            return change(new Event(EventKind.Idle, state, null, null, null, null));
        }
        if (status == Status.WaitingForClose
            && closeDeadlineUnixMs != null
            && now >= closeDeadlineUnixMs) {
            status = Status.Closed;
            Snapshot state = snapshot();
            return change(new Event(EventKind.Closed, state, null, null, null, null));
        }
        return new Change(snapshot(), List.of());
    }

    public synchronized Change close(String actorId, String reason) {
        requireParticipant(actorId);
        if (status == Status.Closed) {
            throw new IllegalStateException("Conversation is already closed");
        }
        status = Status.Closed;
        Snapshot state = snapshot();
        return change(new Event(EventKind.Closed, state, actorId, null, null, null));
    }

    private Participant requireParticipant(String actorId) {
        Participant participant = participants.get(actorId);
        if (participant == null) {
            throw new IllegalStateException("Actor is not a conversation participant");
        }
        return participant;
    }

    private void ensureOpen(String action) {
        if (status == Status.Closed) {
            throw new IllegalStateException("Closed conversation cannot " + action);
        }
    }

    private Change change(Event event) {
        return new Change(event.state(), List.of(event));
    }
}
