package MyGame;

public record DungeonView(String dungeonId, String nodeId, String partyId, java.util.List<String> members, long stage, String runId, RunPhase phase) {}
