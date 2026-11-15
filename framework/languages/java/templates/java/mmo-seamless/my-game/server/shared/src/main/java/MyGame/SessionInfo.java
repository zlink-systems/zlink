package MyGame;

import org.jspecify.annotations.Nullable;

public record SessionInfo(String sessionId, @Nullable String actorId, long objectGeneration) {}
