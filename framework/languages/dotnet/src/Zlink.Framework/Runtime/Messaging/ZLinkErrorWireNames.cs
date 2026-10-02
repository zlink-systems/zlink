namespace Zlink.Framework.Runtime.Messaging;

/// <summary>
/// Cross-language errorCode wire names for <see cref="ZLinkFrameworkErrorKind"/>.
/// Canonical table: C++ channel_reply_writer.cpp error_code_name(). Every
/// language encodes the snake_case name on the wire; enum member names are a
/// process-local representation and never cross the wire.
/// </summary>
internal static class ZLinkErrorWireNames
{
    private const string NotFoundName = "not_found";
    private const string AlreadyExistsName = "already_exists";
    private const string TypeMismatchName = "type_mismatch";
    private const string NotConfiguredName = "not_configured";
    private const string RejectedName = "rejected";
    private const string UnavailableName = "unavailable";
    private const string DeadlineExceededName = "deadline_exceeded";
    private const string ShuttingDownName = "shutting_down";
    private const string ProtocolErrorName = "protocol_error";
    private const string InvalidOperationName = "invalid_operation";
    private const string DataLostName = "data_lost";
    private const string InternalFailureName = "internal_failure";

    public static string Name(ZLinkFrameworkErrorKind kind) =>
        kind switch
        {
            ZLinkFrameworkErrorKind.NotFound => NotFoundName,
            ZLinkFrameworkErrorKind.AlreadyExists => AlreadyExistsName,
            ZLinkFrameworkErrorKind.TypeMismatch => TypeMismatchName,
            ZLinkFrameworkErrorKind.NotConfigured => NotConfiguredName,
            ZLinkFrameworkErrorKind.Rejected => RejectedName,
            ZLinkFrameworkErrorKind.Unavailable => UnavailableName,
            ZLinkFrameworkErrorKind.DeadlineExceeded => DeadlineExceededName,
            ZLinkFrameworkErrorKind.ShuttingDown => ShuttingDownName,
            ZLinkFrameworkErrorKind.ProtocolError => ProtocolErrorName,
            ZLinkFrameworkErrorKind.InvalidOperation => InvalidOperationName,
            ZLinkFrameworkErrorKind.DataLost => DataLostName,
            ZLinkFrameworkErrorKind.InternalFailure => InternalFailureName,
            _ => InternalFailureName,
        };

    public static bool TryParse(string? name, out ZLinkFrameworkErrorKind kind)
    {
        switch (name)
        {
            case NotFoundName:
                kind = ZLinkFrameworkErrorKind.NotFound;
                return true;
            case AlreadyExistsName:
                kind = ZLinkFrameworkErrorKind.AlreadyExists;
                return true;
            case TypeMismatchName:
                kind = ZLinkFrameworkErrorKind.TypeMismatch;
                return true;
            case NotConfiguredName:
                kind = ZLinkFrameworkErrorKind.NotConfigured;
                return true;
            case RejectedName:
                kind = ZLinkFrameworkErrorKind.Rejected;
                return true;
            case UnavailableName:
                kind = ZLinkFrameworkErrorKind.Unavailable;
                return true;
            case DeadlineExceededName:
                kind = ZLinkFrameworkErrorKind.DeadlineExceeded;
                return true;
            case ShuttingDownName:
                kind = ZLinkFrameworkErrorKind.ShuttingDown;
                return true;
            case ProtocolErrorName:
                kind = ZLinkFrameworkErrorKind.ProtocolError;
                return true;
            case InvalidOperationName:
                kind = ZLinkFrameworkErrorKind.InvalidOperation;
                return true;
            case DataLostName:
                kind = ZLinkFrameworkErrorKind.DataLost;
                return true;
            case InternalFailureName:
                kind = ZLinkFrameworkErrorKind.InternalFailure;
                return true;
            default:
                kind = default;
                return false;
        }
    }
}

/// <summary>
/// Reply metadata marker for errors the framework generated itself (never for
/// application handler failures). Lets a requester distinguish a
/// framework-origin NotFound/Unavailable (e.g. stale route / missing handler)
/// from an application error that happens to use the same kind. Mirrors the
/// C++ failure_origin_wire.hpp framework-origin marker.
/// </summary>
internal static class ZLinkErrorOriginWire
{
    public const string FrameworkOriginMetadataKey = "zlink.origin";
    public const string FrameworkOriginMetadataValue = "framework";

    /// <summary>Error-envelope metadata for the reply of <paramref name="exception"/>:
    /// the framework-origin marker when the framework itself produced the error
    /// (or the caller forces a framework surface), otherwise no metadata.</summary>
    public static Dictionary<string, string>? ErrorReplyMetadata(
        Exception exception,
        bool forceFrameworkOrigin = false
    )
    {
        if (
            !forceFrameworkOrigin
            && exception is not ZLinkFrameworkException { Origin: ZLinkErrorOrigin.Framework }
        )
            return null;
        return FrameworkOriginMetadata();
    }

    public static Dictionary<string, string> FrameworkOriginMetadata() =>
        new(1) { [FrameworkOriginMetadataKey] = FrameworkOriginMetadataValue };

    public static bool HasFrameworkOrigin(IReadOnlyDictionary<string, string>? metadata) =>
        metadata is not null
        && metadata.TryGetValue(FrameworkOriginMetadataKey, out var value)
        && value == FrameworkOriginMetadataValue;

    /// <summary>Origin classification for an error decoded from a remote reply
    /// envelope: framework when the marker is present, otherwise the remote
    /// application handler produced it (stale-route contract).</summary>
    public static ZLinkErrorOrigin RemoteReplyOrigin(
        IReadOnlyDictionary<string, string>? metadata
    ) => HasFrameworkOrigin(metadata) ? ZLinkErrorOrigin.Framework : ZLinkErrorOrigin.Application;
}
