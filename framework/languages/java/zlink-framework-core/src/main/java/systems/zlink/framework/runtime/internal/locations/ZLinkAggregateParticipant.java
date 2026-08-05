package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkAggregateParticipant(
    String authorityKey,
    long objectGeneration,
    long sourceAuthorityOwnerGeneration,
    String expectedStoreVersion,
    ZLinkAuthorityGenerationTransition ownerTransition,
    byte[] authorityPayload,
    byte[] membershipMutation) {
    public ZLinkAggregateParticipant {
        Objects.requireNonNull(authorityKey, "authorityKey");
        if (objectGeneration <= 0 || sourceAuthorityOwnerGeneration <= 0) {
            throw new IllegalArgumentException(
                "aggregate participant generations must be positive");
        }
        Objects.requireNonNull(
            expectedStoreVersion,
            "expectedStoreVersion");
        Objects.requireNonNull(ownerTransition, "ownerTransition");
        authorityPayload = Objects.requireNonNull(
            authorityPayload,
            "authorityPayload").clone();
        membershipMutation = Objects.requireNonNull(
            membershipMutation,
            "membershipMutation").clone();
    }

    @Override
    public byte[] authorityPayload() {
        return authorityPayload.clone();
    }

    @Override
    public byte[] membershipMutation() {
        return membershipMutation.clone();
    }
}
