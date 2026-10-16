package MyGame;

public record FieldView(
        String fieldId,
        String nodeId,
        java.util.List<String> players,
        java.util.List<PartyView> parties) {}
