namespace Zlink.Framework.Contracts.Locations;

/// <summary>
/// Page request shared by store and operational list queries. A default
/// value uses the contract page size of 100.
/// </summary>
public readonly record struct ZLinkPageRequest(
    int PageSize = 100,
    string? ContinuationToken = null);

public sealed record ZLinkLocationPage<T>(
    IReadOnlyList<T> Items,
    string? ContinuationToken);
