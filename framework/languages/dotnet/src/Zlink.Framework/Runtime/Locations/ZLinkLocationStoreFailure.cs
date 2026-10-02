namespace Zlink.Framework.Runtime.Locations;

/// <summary>Separates caller validation and settled relocation failures from uncertain Store calls.</summary>
internal static class ZLinkLocationStoreFailure
{
    internal static bool IsIndeterminate(Exception error, CancellationToken cancellationToken) =>
        !cancellationToken.IsCancellationRequested
        && error
            is not (
                ArgumentException
                or ZLinkRelocationTargetSettledException
                or ZLinkRelocationDataLostException
                or ZLinkAuthorityGenerationExhaustedException
            );
}
