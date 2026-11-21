package systems.zlink.framework.runtime.internal.locations;

public final class ZLinkCanonicalRelocationAuthorityStateCodecTestAccess {
    private ZLinkCanonicalRelocationAuthorityStateCodecTestAccess() {}

    public static byte[] publish(
            byte[] authorityPayload,
            ZLinkAggregateRelocationCoordinator.Request request,
            ZLinkAuthorityGenerationTransition transition) {
        return ZLinkLegacyRelocationAuthorityFixture.publish(authorityPayload, request, transition);
    }
}
