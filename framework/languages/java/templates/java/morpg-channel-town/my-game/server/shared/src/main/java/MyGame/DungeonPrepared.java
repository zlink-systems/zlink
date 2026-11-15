package MyGame;

import org.jspecify.annotations.Nullable;

public record DungeonPrepared(String dungeonId, String runId, @Nullable String reason) {}
