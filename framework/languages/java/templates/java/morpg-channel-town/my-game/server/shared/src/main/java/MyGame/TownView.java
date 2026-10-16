package MyGame;

public record TownView(
        String townId,
        String nodeId,
        java.util.List<String> players,
        java.util.List<PartyView> parties) {}
