using Microsoft.Extensions.Logging;

namespace Zlink.Framework.Runtime.Diagnostics;

internal static class ZLinkMessageFlowLogger
{
    private static readonly EventId HandlerMissingEvent = new(1001, "ZLinkHandlerMissing");
    private static readonly EventId PayloadDecodeFailedEvent = new(1002, "ZLinkPayloadDecodeFailed");
    private static readonly EventId MessageDroppedEvent = new(1003, "ZLinkMessageDropped");
    private static readonly EventId MessageRejectedEvent = new(1004, "ZLinkMessageRejected");

    public static void HandlerMissing(
        ILogger logger,
        LogLevel level,
        ZLinkMessageFlowEvent flow,
        string action,
        string reason,
        string surfaceName,
        string kindName,
        string? actorType = null,
        bool writeLog = true)
    {
        ZLinkTelemetry.RecordHandlerMissing(surfaceName, kindName, action, reason);
        ZLinkTelemetry.TraceFlowEvent(
            "handler-missing",
            flow,
            action,
            reason,
            surfaceName,
            kindName,
            actorType);

        if (!writeLog || !logger.IsEnabled(level)) return;

        logger.Log(
            level,
            HandlerMissingEvent,
            "ZLink message flow {Event} {Surface} {Kind} {PacketName} {Action} {Reason} {ChannelName} {ActorId} {ActorType} {SpotId}",
            "handler-missing",
            surfaceName,
            kindName,
            flow.PacketName,
            action,
            reason,
            flow.ChannelName,
            flow.ActorId,
            actorType,
            flow.SpotId);
    }

    public static void PayloadDecodeFailed(
        ILogger logger,
        ZLinkMessageFlowEvent flow,
        string action,
        string reason,
        Exception exception,
        string surfaceName,
        string kindName,
        string? actorType = null,
        bool writeLog = true)
    {
        if (string.Equals(action, "reply-error", StringComparison.Ordinal))
            ZLinkTelemetry.RecordReplyError(surfaceName, kindName, reason);
        else if (string.Equals(action, "drop", StringComparison.Ordinal))
            ZLinkTelemetry.RecordDropped(surfaceName, kindName, reason);
        ZLinkTelemetry.TraceFlowEvent(
            "failed",
            flow,
            action,
            reason,
            surfaceName,
            kindName,
            actorType);

        if (!writeLog || !logger.IsEnabled(LogLevel.Warning)) return;

        logger.LogWarning(
            PayloadDecodeFailedEvent,
            exception,
            "ZLink message flow {Event} {Surface} {Kind} {PacketName} {Action} {Reason} {ChannelName} {ActorId} {ActorType} {SpotId}",
            "failed",
            surfaceName,
            kindName,
            flow.PacketName,
            action,
            reason,
            flow.ChannelName,
            flow.ActorId,
            actorType,
            flow.SpotId);
    }

    public static void Dropped(
        ILogger logger,
        LogLevel level,
        ZLinkMessageFlowEvent flow,
        string reason,
        string surfaceName,
        string kindName,
        string? actorType = null,
        bool writeLog = true)
    {
        ZLinkTelemetry.RecordDropped(surfaceName, kindName, reason);
        ZLinkTelemetry.TraceFlowEvent(
            "dropped",
            flow,
            "drop",
            reason,
            surfaceName,
            kindName,
            actorType);

        if (!writeLog || !logger.IsEnabled(level)) return;

        logger.Log(
            level,
            MessageDroppedEvent,
            "ZLink message flow {Event} {Surface} {Kind} {PacketName} {Action} {Reason} {ChannelName} {ActorId} {ActorType} {SpotId}",
            "dropped",
            surfaceName,
            kindName,
            flow.PacketName,
            "drop",
            reason,
            flow.ChannelName,
            flow.ActorId,
            actorType,
            flow.SpotId);
    }

    public static void Rejected(
        ILogger logger,
        LogLevel level,
        ZLinkMessageFlowEvent flow,
        string reason,
        string surfaceName,
        string kindName,
        Exception? exception = null,
        string? actorType = null,
        bool writeLog = true)
    {
        if (string.Equals(reason, "no-join-handler", StringComparison.Ordinal))
            ZLinkTelemetry.RecordHandlerMissing(surfaceName, kindName, "rejected", reason);
        ZLinkTelemetry.TraceFlowEvent(
            "rejected",
            flow,
            "rejected",
            reason,
            surfaceName,
            kindName,
            actorType);

        if (!writeLog || !logger.IsEnabled(level)) return;

        logger.Log(
            level,
            MessageRejectedEvent,
            exception,
            "ZLink message flow {Event} {Surface} {Kind} {PacketName} {Action} {Reason} {ChannelName} {ActorId} {ActorType} {SpotId}",
            "rejected",
            surfaceName,
            kindName,
            flow.PacketName,
            "rejected",
            reason,
            flow.ChannelName,
            flow.ActorId,
            actorType,
            flow.SpotId);
    }
}
