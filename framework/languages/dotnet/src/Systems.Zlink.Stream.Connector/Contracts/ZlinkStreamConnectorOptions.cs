namespace Systems.Zlink.Stream.Connector.Contracts;

public sealed class ZlinkStreamConnectorOptions
{
    private int _diagnosticsLevel = (int)ZlinkStreamDiagnosticsLevel.Errors;

    public required Uri Endpoint { get; init; }

    public ZlinkStreamTransport? Transport { get; init; }

    public TimeSpan ConnectTimeout { get; init; } = TimeSpan.FromSeconds(5);

    public TimeSpan RequestTimeout { get; init; } = TimeSpan.FromSeconds(30);

    public TimeSpan WaitTimeout { get; init; } = TimeSpan.FromSeconds(5);

    public ZlinkStreamHeartbeatOptions Heartbeat { get; init; } = new();

    public ZlinkStreamReconnectOptions Reconnect { get; init; } = new();

    public int MaxSendPayloadSize { get; init; } = 64 * 1024;

    public int MaxReceivePayloadSize { get; init; } = 64 * 1024;

    public int MaxReceivedMessages { get; init; } = 1024;

    public int MaxPendingDispatchCallbacks { get; init; } = 1024;

    public int MaxInboundObserverNotifications { get; init; } = 1024;

    public int MaxInboundObserverPayloadPreviewBytes { get; init; }

    public bool SkipServerCertificateValidation { get; init; }

    public ZlinkStreamDispatchMode DispatchMode { get; init; } = ZlinkStreamDispatchMode.Manual;

    public ZlinkStreamCompression Compression { get; init; } = ZlinkStreamCompression.Lz4;

    /// <summary>
    ///     Diagnostics level applied for the connector lifetime. The default,
    ///     <see cref="ZlinkStreamDiagnosticsLevel.Errors" />, keeps the current wire
    ///     behavior (outbound frames carry flow_id/flow_origin). At
    ///     <see cref="ZlinkStreamDiagnosticsLevel.Off" /> the connector neither creates
    ///     nor attaches flow fields on outbound frames, skips inbound flow validation
    ///     and flow-context installation (structural length checks remain), and performs
    ///     no other trace-only work. Correlation ids for requests are protocol
    ///     information and are kept at every level.
    /// </summary>
    /// <remarks>
    ///     Backed by an atomic cell so the level can change while the connector is
    ///     running (see <see cref="IZlinkStreamConnector.SetDiagnosticsLevel" />).
    ///     Every read observes the most recently written value; nothing in this type
    ///     caches it across a single processing point on its own, so callers that must
    ///     act consistently within one operation should read it once and reuse that
    ///     value.
    /// </remarks>
    public ZlinkStreamDiagnosticsLevel DiagnosticsLevel
    {
        get => (ZlinkStreamDiagnosticsLevel)Volatile.Read(ref _diagnosticsLevel);
        init => _diagnosticsLevel = (int)value;
    }

    /// <summary>
    ///     Atomically updates <see cref="DiagnosticsLevel" /> for the connector that
    ///     owns this options instance. Internal because live updates must go through
    ///     <see cref="IZlinkStreamConnector.SetDiagnosticsLevel" />, which validates the
    ///     new value first.
    /// </summary>
    internal void SetDiagnosticsLevelLive(ZlinkStreamDiagnosticsLevel level)
    {
        Volatile.Write(ref _diagnosticsLevel, (int)level);
    }

    /// <summary>
    ///     Optional compression codec for frames marked with <see cref="ZlinkStreamHeaderFlags.PayloadCompressed" />.
    ///     When this value is set, it replaces the built-in compression selected by <see cref="Compression" />.
    ///     Set <see cref="Compression" /> to <see cref="ZlinkStreamCompression.None" /> only when this connector must
    ///     reject compressed frames and fail calls that opt into compression.
    /// </summary>
    public IZlinkStreamCompressionCodec? CompressionCodec { get; init; }

    public IZlinkStreamPacketNameResolver NameResolver { get; init; } =
        new ZlinkStreamPacketNameResolver();

    /// <summary>
    ///     Optional custom payload codec for the typed connector API. When set, the
    ///     connector encodes and decodes typed payloads with this codec instead of JSON.
    /// </summary>
    public IZlinkStreamPayloadCodec? PayloadCodec { get; init; }
}

public sealed class ZlinkStreamHeartbeatOptions
{
    public bool Enabled { get; init; } = true;

    public TimeSpan Interval { get; init; } = TimeSpan.FromSeconds(1);

    public TimeSpan Timeout { get; init; } = TimeSpan.FromSeconds(5);
}

public sealed class ZlinkStreamReconnectOptions
{
    public bool Enabled { get; init; } = true;

    public TimeSpan InitialDelay { get; init; } = TimeSpan.FromMilliseconds(250);

    public TimeSpan MaxDelay { get; init; } = TimeSpan.FromSeconds(5);

    public double BackoffFactor { get; init; } = 2.0;

    public int? MaxAttempts { get; init; } = 3;
}