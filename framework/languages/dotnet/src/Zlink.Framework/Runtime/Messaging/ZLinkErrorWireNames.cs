namespace Zlink.Framework.Runtime.Messaging;

/// <summary>
/// Cross-language errorCode wire names for <see cref="ZLinkFrameworkErrorKind"/>.
/// Canonical table: C++ channel_reply_writer.cpp error_code_name(). Every
/// language encodes the snake_case name on the wire; enum member names are a
/// process-local representation and never cross the wire.
/// </summary>
internal static class ZLinkErrorWireNames
{
    public static string Name(ZLinkFrameworkErrorKind kind) => kind switch
    {
        ZLinkFrameworkErrorKind.NotFound => "not_found",
        ZLinkFrameworkErrorKind.AlreadyExists => "already_exists",
        ZLinkFrameworkErrorKind.TypeMismatch => "type_mismatch",
        ZLinkFrameworkErrorKind.NotConfigured => "not_configured",
        ZLinkFrameworkErrorKind.Rejected => "rejected",
        ZLinkFrameworkErrorKind.Unavailable => "unavailable",
        ZLinkFrameworkErrorKind.CapacityExceeded => "capacity_exceeded",
        ZLinkFrameworkErrorKind.DeadlineExceeded => "deadline_exceeded",
        ZLinkFrameworkErrorKind.ShuttingDown => "shutting_down",
        ZLinkFrameworkErrorKind.ProtocolError => "protocol_error",
        ZLinkFrameworkErrorKind.InvalidOperation => "invalid_operation",
        ZLinkFrameworkErrorKind.DataLost => "data_lost",
        ZLinkFrameworkErrorKind.InternalFailure => "internal_failure",
        _ => "internal_failure"
    };

    public static bool TryParse(string? name, out ZLinkFrameworkErrorKind kind)
    {
        switch (name)
        {
            case "not_found": kind = ZLinkFrameworkErrorKind.NotFound; return true;
            case "already_exists": kind = ZLinkFrameworkErrorKind.AlreadyExists; return true;
            case "type_mismatch": kind = ZLinkFrameworkErrorKind.TypeMismatch; return true;
            case "not_configured": kind = ZLinkFrameworkErrorKind.NotConfigured; return true;
            case "rejected": kind = ZLinkFrameworkErrorKind.Rejected; return true;
            case "unavailable": kind = ZLinkFrameworkErrorKind.Unavailable; return true;
            case "capacity_exceeded": kind = ZLinkFrameworkErrorKind.CapacityExceeded; return true;
            case "deadline_exceeded": kind = ZLinkFrameworkErrorKind.DeadlineExceeded; return true;
            case "shutting_down": kind = ZLinkFrameworkErrorKind.ShuttingDown; return true;
            case "protocol_error": kind = ZLinkFrameworkErrorKind.ProtocolError; return true;
            case "invalid_operation": kind = ZLinkFrameworkErrorKind.InvalidOperation; return true;
            case "data_lost": kind = ZLinkFrameworkErrorKind.DataLost; return true;
            case "internal_failure": kind = ZLinkFrameworkErrorKind.InternalFailure; return true;
            default: kind = default; return false;
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
        bool forceFrameworkOrigin = false)
    {
        if (!forceFrameworkOrigin
            && exception is not ZLinkFrameworkException { Origin: ZLinkErrorOrigin.Framework })
            return null;
        return FrameworkOriginMetadata();
    }

    public static Dictionary<string, string> FrameworkOriginMetadata() => new(1)
    {
        [FrameworkOriginMetadataKey] = FrameworkOriginMetadataValue
    };

    public static bool HasFrameworkOrigin(IReadOnlyDictionary<string, string>? metadata) =>
        metadata is not null
        && metadata.TryGetValue(FrameworkOriginMetadataKey, out var value)
        && value == FrameworkOriginMetadataValue;

    /// <summary>Origin classification for an error decoded from a remote reply
    /// envelope: framework when the marker is present, otherwise the remote
    /// application handler produced it (stale-route contract).</summary>
    public static ZLinkErrorOrigin RemoteReplyOrigin(
        IReadOnlyDictionary<string, string>? metadata) =>
        HasFrameworkOrigin(metadata)
            ? ZLinkErrorOrigin.Framework
            : ZLinkErrorOrigin.Application;
}
