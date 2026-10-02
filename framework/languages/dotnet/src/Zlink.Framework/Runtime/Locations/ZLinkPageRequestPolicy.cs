namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkPageRequestPolicy
{
    internal const int DiscoveryPageSize = 256;
    public const int MaximumPageSize = Internal.ZLinkLocationStoreLimits.MaximumPageItems;

    public static ZLinkPageRequest Normalize(ZLinkPageRequest request)
    {
        if (request.PageSize is < 0 or > MaximumPageSize)
            throw new ArgumentOutOfRangeException(nameof(request));

        return request.PageSize == 0
            ? new ZLinkPageRequest(ContinuationToken: request.ContinuationToken)
            : request;
    }
}
