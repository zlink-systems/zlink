namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkPageRequestPolicy
{
    public const int DefaultPageSize = 100;
    public const int MaximumPageSize = 1000;

    public static ZLinkPageRequest Normalize(ZLinkPageRequest request)
    {
        if (request.PageSize is < 0 or > MaximumPageSize)
            throw new ArgumentOutOfRangeException(nameof(request));

        return request.PageSize == 0
            ? request with { PageSize = DefaultPageSize }
            : request;
    }
}
