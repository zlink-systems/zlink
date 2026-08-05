namespace Zlink.Framework.Runtime.Channels;

// The classic dealer/fanout plane has no application-metadata slot in its
// envelope; only the RouteMesh service wire carries the canonical frame
// (05-route-mesh §6). Rejecting loudly beats dropping the caller's metadata.
internal static class ZLinkClassicCallSupport
{
    public static NotSupportedException MetadataNotSupported()
    {
        return new NotSupportedException(
            "Application metadata rides the RouteMesh service wire only; the "
            + "classic channel plane does not carry it. Use the RouteMesh "
            + "route/spot clients for metadata-bearing calls.");
    }
}
