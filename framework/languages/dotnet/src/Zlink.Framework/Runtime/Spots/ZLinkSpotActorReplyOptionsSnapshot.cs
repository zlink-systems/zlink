namespace Zlink.Framework.Runtime.Spots;

internal sealed record ZLinkSpotActorReplyOptionsSnapshot(
    IReadOnlyDictionary<string, string> Metadata,
    bool CompressPayload)
{
    internal static readonly ZLinkSpotActorReplyOptionsSnapshot Default = new(
        new Dictionary<string, string>(StringComparer.Ordinal),
        false);
}