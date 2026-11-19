package MyGame;

import org.jspecify.annotations.Nullable;

public record PartyCommandResult(@Nullable PartyView view, @Nullable ActionRejected rejection) {}
