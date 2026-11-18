package MyGame;

import org.jspecify.annotations.Nullable;

public record PartyView(
        String partyId,
        java.util.List<String> members,
        String leaderId,
        RunPhase phase,
        @Nullable String runId) {}
