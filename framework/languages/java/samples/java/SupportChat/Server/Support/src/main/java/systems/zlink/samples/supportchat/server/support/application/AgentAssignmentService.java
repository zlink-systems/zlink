package systems.zlink.samples.supportchat.server.support.application;

import java.util.LinkedHashMap;
import java.util.Map;

public final class AgentAssignmentService {
    public record AvailableAgent(String rosterActorId, String displayName) {
    }

    private static final class AgentSlot {
        private final String actorId;
        private String displayName;
        private int active;

        private AgentSlot(String actorId, String displayName, int active) {
            this.actorId = actorId;
            this.displayName = displayName;
            this.active = active;
        }
    }

    private final int capacity;
    private final Map<String, AgentSlot> agents = new LinkedHashMap<>();
    private final Map<String, String> reservations = new LinkedHashMap<>();

    public AgentAssignmentService(int capacity) {
        this.capacity = capacity;
    }

    public synchronized void setAvailable(
        String rosterActorId,
        String displayName,
        boolean available) {
        if (!available) {
            agents.remove(rosterActorId);
            return;
        }
        int active = (int) reservations.values().stream()
            .filter(rosterActorId::equals)
            .count();
        AgentSlot slot = agents.computeIfAbsent(
            rosterActorId,
            ignored -> new AgentSlot(rosterActorId, displayName, active));
        slot.displayName = displayName;
        slot.active = active;
    }

    public synchronized AvailableAgent assignForConversation(String conversationId) {
        if (reservations.containsKey(conversationId)) {
            return null;
        }
        AgentSlot slot = agents.values().stream()
            .filter(candidate -> candidate.active < capacity)
            .findFirst()
            .orElse(null);
        if (slot == null) {
            return null;
        }
        slot.active += 1;
        reservations.put(conversationId, slot.actorId);
        return new AvailableAgent(slot.actorId, slot.displayName);
    }

    public synchronized void releaseConversation(String conversationId) {
        String actorId = reservations.remove(conversationId);
        AgentSlot slot = actorId == null ? null : agents.get(actorId);
        if (slot != null && slot.active > 0) {
            slot.active -= 1;
        }
    }
}
