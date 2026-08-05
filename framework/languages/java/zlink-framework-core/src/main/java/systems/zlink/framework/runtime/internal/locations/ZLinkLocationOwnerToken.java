package systems.zlink.framework.runtime.internal.locations;

public record ZLinkLocationOwnerToken(
    String ownerId,
    long leaseGeneration) {
}
