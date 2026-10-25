package MyGame;

public record PartyView(
        String partyId,
        java.util.List<String> members,
        String leaderId,
        RunPhase phase,
        String runId) {}
