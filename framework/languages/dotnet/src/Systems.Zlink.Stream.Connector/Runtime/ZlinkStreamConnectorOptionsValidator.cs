namespace Systems.Zlink.Stream.Connector.Runtime;

/// <summary>
///     Checks every connector option before a connection is attempted
///     (stream-connector spec §6.3, .NET spec §12).
/// </summary>
/// <remarks>
///     Every failure leaves through <see cref="ZlinkStreamException" /> so the caller can
///     read the code off it. A standard .NET exception would not carry one, leaving the
///     caller unable to tell <see cref="ZlinkStreamErrorCode.ValidationFailed" /> from
///     <see cref="ZlinkStreamErrorCode.ConfigurationError" /> (spec §9.2).
/// </remarks>
internal static class ZlinkStreamConnectorOptionsValidator
{
    public static void Validate(ZlinkStreamConnectorOptions options)
    {
        if (options.Endpoint is null)
            throw Validation("Endpoint is required.");

        // Endpoint scheme and transport agreement, plus unsupported schemes.
        ZlinkStreamTransportFactory.ValidateTransport(options);

        if (
            options.Transport is { } transport
            && !Enum.IsDefined(typeof(ZlinkStreamTransport), transport)
        )
            throw Validation("Transport is invalid.");

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
        if (options.MaxPendingDispatchCallbacks <= 0)
            throw Validation("MaxPendingDispatchCallbacks must be positive.");
        if (!Enum.IsDefined(typeof(ZlinkStreamDispatchMode), options.DispatchMode))
            throw Validation("DispatchMode is invalid.");

        ValidateCompression(options);
    }

    private static void ValidateHeartbeat(ZlinkStreamHeartbeatOptions heartbeat)
    {
        if (!heartbeat.Enabled)
            return;
        if (heartbeat.Interval <= TimeSpan.Zero)
            throw Validation("Heartbeat interval must be positive.");
        if (heartbeat.Timeout <= TimeSpan.Zero)
            throw Validation("Heartbeat timeout must be positive.");
        if (heartbeat.Timeout <= heartbeat.Interval)
            throw Validation("Heartbeat timeout must be greater than the heartbeat interval.");
    }

    private static void ValidateReconnect(ZlinkStreamReconnectOptions reconnect)
    {
        if (!reconnect.Enabled)
            return;
        if (reconnect.InitialDelay <= TimeSpan.Zero)
            throw Validation("Reconnect InitialDelay must be positive.");
        if (reconnect.MaxDelay <= TimeSpan.Zero)
            throw Validation("Reconnect MaxDelay must be positive.");
        if (reconnect.BackoffFactor < 1.0)
            throw Validation("Reconnect BackoffFactor must be at least 1.0.");
        if (reconnect.MaxAttempts <= 0)
            throw Validation("Reconnect MaxAttempts must be null or positive.");
    }

    private static void ValidateCompression(ZlinkStreamConnectorOptions options)
    {
        if (!Enum.IsDefined(typeof(ZlinkStreamCompression), options.Compression))
            throw Validation("Compression is invalid.");

        // A codec paired with compression turned off is two options disagreeing, which
        // is a ConfigurationError rather than an out-of-range value (spec §6.3).
        if (
            options.Compression == ZlinkStreamCompression.None
            && options.CompressionCodec is not null
        )
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ConfigurationError,
                "CompressionCodec cannot be set when Compression is None."
            );
    }

    private static ZlinkStreamException Validation(string message) =>
        ZlinkStreamConnector.Error(ZlinkStreamErrorCode.ValidationFailed, message);
}
