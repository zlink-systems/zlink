namespace Systems.Zlink.Stream.Connector.Runtime;

internal static class ZlinkStreamConnectorOptionsValidator
{
    public static void Validate(ZlinkStreamConnectorOptions options)
    {
        if (options.Endpoint is null) throw new ArgumentException("Endpoint is required.", nameof(options));

        ZlinkStreamTransportFactory.ValidateTransport(options);

        if (options.NameResolver is null)
            throw Validation("NameResolver is required.");
        if (options.Heartbeat is null)
            throw Validation("Heartbeat options are required.");
        if (options.Reconnect is null)
            throw Validation("Reconnect options are required.");
        if (options.ConnectTimeout <= TimeSpan.Zero)
            throw Validation("ConnectTimeout must be positive.");
        if (options.RequestTimeout <= TimeSpan.Zero)
            throw Validation("RequestTimeout must be positive.");
        if (options.WaitTimeout <= TimeSpan.Zero)
            throw Validation("WaitTimeout must be positive.");

        ValidateHeartbeat(options.Heartbeat);
        ValidateReconnect(options.Reconnect);

        if (options.MaxSendPayloadSize <= 0)
            throw Validation("MaxSendPayloadSize must be positive.");
        if (options.MaxReceivePayloadSize <= 0)
            throw Validation("MaxReceivePayloadSize must be positive.");
        if (options.MaxReceivedMessages <= 0)
            throw Validation("MaxReceivedMessages must be positive.");
        if (options.MaxPendingDispatchCallbacks <= 0)
            throw Validation("MaxPendingDispatchCallbacks must be positive.");
        if (options.MaxInboundObserverNotifications <= 0)
            throw Validation("MaxInboundObserverNotifications must be positive.");
        if (options.MaxInboundObserverPayloadPreviewBytes < 0)
            throw Validation("MaxInboundObserverPayloadPreviewBytes must not be negative.");
    }

    private static void ValidateHeartbeat(ZlinkStreamHeartbeatOptions heartbeat)
    {
        if (!heartbeat.Enabled) return;
        if (heartbeat.Interval <= TimeSpan.Zero)
            throw Validation("Heartbeat interval must be positive.");
        if (heartbeat.Timeout <= TimeSpan.Zero)
            throw Validation("Heartbeat timeout must be positive.");
        if (heartbeat.Timeout <= heartbeat.Interval)
            throw Validation("Heartbeat timeout must be greater than the heartbeat interval.");
    }

    private static void ValidateReconnect(ZlinkStreamReconnectOptions reconnect)
    {
        if (!reconnect.Enabled) return;
        if (reconnect.InitialDelay <= TimeSpan.Zero)
            throw Validation("Reconnect InitialDelay must be positive.");
        if (reconnect.MaxDelay <= TimeSpan.Zero)
            throw Validation("Reconnect MaxDelay must be positive.");
        if (reconnect.BackoffFactor < 1.0)
            throw Validation("Reconnect BackoffFactor must be at least 1.0.");
        if (reconnect.MaxAttempts <= 0)
            throw Validation("Reconnect MaxAttempts must be null or positive.");
    }

    private static ZlinkStreamException Validation(string message) =>
        ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ValidationFailed, message);
}
