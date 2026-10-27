package MyGame;

public record PrepareDungeon(
        String runId,
        String partyId,
        String ownerSpotId,
        java.util.List<String> members) {}
