package systems.zlink.framework.runtime.internal.locations;

/**
 * Test projection of the relocation state stored in a Location authority
 * payload. Tests consume the resulting authority effects rather than driving
 * the production state machine through its internal phase methods.
 */
public final class ZLinkCanonicalRelocationObservation {
    private ZLinkCanonicalRelocationObservation() {
    }

    public static State observe(byte[] authorityPayload) {
        var published = ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            authorityPayload);
        return published == null
            ? new State(false, false, false)
            : new State(
                true, false, published.sourceCleanupCompleted());
    }

    public static State observe(
        byte[] authorityPayload,
        ZLinkRelocationStore relocationStore) {
        var published = ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            authorityPayload);
        if (published == null) {
            return new State(false, false, false);
        }
        ZLinkRelocationTreeStore.Read stored = ZLinkRelocationTreeStore.read(
                relocationStore,
                published.reference(),
                published.checksumCrc32c(),
                () -> false)
            .toCompletableFuture().join();
        var root = ZLinkServiceRelocationEnvelopeCodec.decode(
            stored.logicalRoot());
        boolean cleanupMarker = root.terminalCompletions().stream()
            .anyMatch(value -> value.payload() != null
                && "ZLinkActorSourceCleanupCompleted".equals(
                    value.payload().packetName()));
        return new State(
            true, cleanupMarker, published.sourceCleanupCompleted());
    }

    public record State(
        boolean relocationPublished,
        boolean sourceCleanupMarked,
        boolean authorityCompleted) {
        public boolean sourceCleanupCompleted() {
            return authorityCompleted;
        }
    }
}
