namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkDirectRelocationInventory
{
    internal static ZLinkAggregateParticipant Participant(
        ZLinkAuthorityKey key,
        string originalStoreVersion,
        ReadOnlyMemory<byte> sourcePayload
    ) =>
        new(
            key,
            originalStoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            sourcePayload,
            ZLinkAggregateInventory.NoMembershipMutation
        );

    internal static string Digest(
        ZLinkAuthorityKey key,
        string originalStoreVersion,
        ReadOnlyMemory<byte> sourcePayload
    ) =>
        ZLinkAggregateInventory
            .Plan([Participant(key, originalStoreVersion, sourcePayload)])
            .Root.DigestSha256;
}
