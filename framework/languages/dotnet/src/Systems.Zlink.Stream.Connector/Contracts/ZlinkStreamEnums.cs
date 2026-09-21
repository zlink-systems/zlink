namespace Systems.Zlink.Stream.Connector.Contracts;

public enum ZlinkStreamTransport
{
    Tcp,
    Tls,
    WebSocket,
    WebSocketSecure,
}

public enum ZlinkStreamCodec : byte
{
    Raw = 0,
    Json = 1,
    MessagePack = 2,
    Protobuf = 3,
}

public enum ZlinkStreamCompression
{
    None,
    Lz4,
}

public enum ZlinkStreamDispatchMode
{
    Manual,
    Immediate,
}

/// <summary>
///     Diagnostics level of the connector. Mirrors the server-side message-flow
///     tracing levels: <see cref="Off" /> disables every trace-only operation,
///     including outbound flow_id/flow_origin stamping and inbound flow capture.
/// </summary>
public enum ZlinkStreamDiagnosticsLevel
{
    Off = 0,
    Errors = 1,
    Normal = 2,
    Detailed = 3,
}

public enum ZlinkStreamMessageKind : byte
{
    Send = 1,
    Request = 2,
    Response = 3,
    Error = 4,
    Control = 5,
}

public enum ZlinkStreamErrorCode
{
    Disconnected,
    ConfigurationError,
    ValidationFailed,
    RequestTimeout,
    ConnectTimeout,
    FrameDecodeFailed,
    FrameTooLarge,
    SendFailed,
    CompressionFailed,
    TlsValidationFailed,
    DecompressionFailed,
    UserCallbackFailed,
    RemoteError,
}

public enum ZlinkStreamConnectionState
{
    Created,
    Connecting,
    Connected,
    Reconnecting,
    Disconnected,
    Closed,
}

/// <summary>
///     Origin of the flow a received message belongs to (stream-connector .NET spec §11).
/// </summary>
/// <remarks>
///     The <c>flow_origin</c> wire values are 1..4 while these ordinals are 0..3. The
///     header codec converts between the two explicitly; never cast this enum to an
///     integer to obtain a wire value.
/// </remarks>
public enum ZlinkStreamFlowOrigin
{
    Inbound,
    Timer,
    Application,
    Lifecycle,
}

public enum ZlinkStreamCloseReason
{
    ClientClose = 0,
    IdleTimeout = 1,
    HeartbeatTimeout = 2,
    ServerDrain = 3,
    ProtocolError = 4,
    TransportError = 5,
}
