namespace Zlink.Framework.Runtime.Locations;

/// <summary>Separates caller validation and settled relocation failures from uncertain Store calls.</summary>
internal static class ZLinkLocationStoreFailure
{
    // A failed provider call may have applied a CAS. Read its authority to settle it.
    internal static bool IsIndeterminate(Exception error, CancellationToken cancellationToken) =>
        !cancellationToken.IsCancellationRequested
        && error
            is not (
                ArgumentException
                or ZLinkRelocationTargetSettledException
                or ZLinkRelocationReplyBatchLimitException
                or ZLinkRelocationDataLostException
                or ZLinkAuthorityGenerationExhaustedException
            );

    // A failed read, blob operation or relay can be retried after unavailability.
    internal static bool IsUnavailable(Exception error, CancellationToken cancellationToken) =>
        !cancellationToken.IsCancellationRequested
        && error is not ZLinkRelocationDataLostException
        && error
            is IOException
                or TimeoutException
                or ZLinkFrameworkException
                {
                    Kind: ZLinkFrameworkErrorKind.Unavailable
                        or ZLinkFrameworkErrorKind.Rejected
                        or ZLinkFrameworkErrorKind.DeadlineExceeded,
                };
}
