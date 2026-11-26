package systems.zlink.framework.runtime.internal.locations;

/** Command 40's prepared attempt and resolved membership for a single Actor, if applicable. */
public record ZLinkAggregateCommitIntent(
        String representativeKey, long targetAttemptGeneration, ActorMembership actorMembership) {
    public record ActorMembership(String spotId, long spotGeneration, int spotKind) {}
}
