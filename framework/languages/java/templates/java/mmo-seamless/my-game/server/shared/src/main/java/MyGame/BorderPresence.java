package MyGame;

import org.jspecify.annotations.Nullable;

public record BorderPresence(
        String sourceZoneId,
        String playerId,
        Position position,
        long sequence,
        BorderPresenceKind kind,
        @Nullable PlayerLeftReason reason) {}
