namespace Zlink.Framework.Contracts.Locations;

/// <summary>
/// Page request shared by store and operational list queries. A default
/// value uses the contract page size of 100.
/// </summary>
public readonly record struct ZLinkPageRequest(
    int PageSize = ZLinkPageRequest.DefaultPageSize,
    string? ContinuationToken = null
)
{
    internal const int DefaultPageSize = 100;
}

public sealed record ZLinkLocationPage<T>(IReadOnlyList<T> Items, string? ContinuationToken);
