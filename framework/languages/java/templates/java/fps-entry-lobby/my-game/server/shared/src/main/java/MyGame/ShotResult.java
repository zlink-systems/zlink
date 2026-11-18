package MyGame;

import org.jspecify.annotations.Nullable;

public record ShotResult(String outcome, @Nullable String targetId, long sequence) {}
