package MyGame;

public record DungeonView(
        String dungeonId,
        String nodeId,
        String partyId,
        java.util.List<String> members,
        int stage,
        String runId,
        RunPhase phase) {}
