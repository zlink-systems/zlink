namespace Zlink.Framework.Contracts.Errors;

public sealed class ZLinkFrameworkException : Exception
{
    internal ZLinkFrameworkException(
        ZLinkFrameworkErrorKind kind,
        string message,
        ZLinkRetryAdvice? retryAdvice = null,
        Exception? innerException = null)
        : base(message, innerException)
    {
        Kind = kind;
        RetryAdvice = retryAdvice ?? DefaultRetryAdvice(kind);
    }

    public ZLinkFrameworkErrorKind Kind { get; }

    internal ZLinkRetryAdvice RetryAdvice { get; }

    /// <summary>
    /// Who produced the failure. <see cref="ZLinkErrorOrigin.Framework"/>
    /// marks errors the framework runtime generated on its own (route
    /// resolution failure, sealed admission, dispatch rejection); their error
    /// replies carry the <c>zlink.origin=framework</c> metadata marker.
    /// <see cref="ZLinkErrorOrigin.Application"/> marks errors decoded from a
    /// peer error reply that did not carry the marker — the remote application
    /// handler produced them, so route caches must not read them as a
    /// stale-route signal. Mirrors the C++ <c>error_origin_t</c> contract.
    /// </summary>
    internal ZLinkErrorOrigin Origin { get; init; }

    private static ZLinkRetryAdvice DefaultRetryAdvice(
        ZLinkFrameworkErrorKind kind) =>
        kind switch
        {
            ZLinkFrameworkErrorKind.Unavailable
                or ZLinkFrameworkErrorKind.CapacityExceeded
                or ZLinkFrameworkErrorKind.DeadlineExceeded =>
                ZLinkRetryAdvice.RetryAfterBackoff,
            ZLinkFrameworkErrorKind.ShuttingDown =>
                ZLinkRetryAdvice.RetryAfterStateChange,
            _ => ZLinkRetryAdvice.DoNotRetry
        };
}

public enum ZLinkFrameworkErrorKind
{
    NotFound = 0,
    AlreadyExists = 1,
    TypeMismatch = 2,
    NotConfigured = 3,
    Rejected = 4,
    Unavailable = 5,
    CapacityExceeded = 6,
    DeadlineExceeded = 7,
    ShuttingDown = 8,
    ProtocolError = 9,
    InvalidOperation = 10,
    DataLost = 11,
    InternalFailure = 12
}

internal enum ZLinkRetryAdvice
{
    DoNotRetry = 0,
    RetryAfterBackoff = 1,
    RetryAfterStateChange = 2
}

internal enum ZLinkErrorOrigin
{
    Unspecified = 0,
    Framework = 1,
    Application = 2
}
