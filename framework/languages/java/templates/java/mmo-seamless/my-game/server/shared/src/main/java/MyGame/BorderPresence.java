package MyGame;

public record BorderPresence(
        String sourceZoneId,
        String playerId,
        Position position,
        long sequence,
        BorderPresenceKind kind,
        PlayerLeftReason reason) {}
