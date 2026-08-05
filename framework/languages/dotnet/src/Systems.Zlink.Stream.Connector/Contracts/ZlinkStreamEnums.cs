namespace Systems.Zlink.Stream.Connector.Contracts;

public enum ZlinkStreamTransport
{
    Tcp,
    Tls,
    WebSocket,
    WebSocketSecure
}

public enum ZlinkStreamCodec : byte
{
    Raw = 0,
    Json = 1,
    MessagePack = 2,
    Protobuf = 3
}

public enum ZlinkStreamCompression
{
    None,
    Lz4
}

public enum ZlinkStreamDispatchMode
{
    Manual,
    Immediate
}

public enum ZlinkStreamMessageKind : byte
{
    Send = 1,
    Request = 2,
    Response = 3,
    Error = 4,
    Control = 5
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
    ObserverFailed,
    ObserverDropped,
    RemoteError,
    ReceivedMessageDropped
}

public enum ZlinkStreamConnectionState
{
    Created,
    Connecting,
    Connected,
    Reconnecting,
    Disconnected,
    Closed
}

public enum ZlinkStreamCloseReason
{
    ClientClose = 0,
    IdleTimeout = 1,
    HeartbeatTimeout = 2,
    ServerDrain = 3,
    ProtocolError = 4,
    TransportError = 5
}
