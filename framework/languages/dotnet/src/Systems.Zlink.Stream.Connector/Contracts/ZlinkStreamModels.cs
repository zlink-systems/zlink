namespace Systems.Zlink.Stream.Connector.Contracts;

public sealed record ZlinkStreamDisconnected(ZlinkStreamCloseReason CloseReason);

public sealed record ZlinkStreamEncodedPayload(
    ZlinkStreamCodec Codec,
    ReadOnlyMemory<byte> Payload,
    Type? MessageType = null
);

public sealed record ZlinkStreamMessage(string Name, ZlinkStreamMetadata Metadata, object? Payload);

/// <summary>
///     A received stream message: payload, packet name, metadata and the flow pair the
///     frame carried (stream-connector spec §5.5, .NET spec §11).
/// </summary>
/// <param name="Name">Packet name the frame carried.</param>
/// <param name="Metadata">Metadata key-value pairs the frame carried.</param>
/// <param name="Payload">Decoded payload.</param>
/// <param name="FlowId">
///     Flow identifier the frame carried, or <see langword="null" /> when the connector's
///     diagnostics level is <see cref="ZlinkStreamDiagnosticsLevel.Off" /> or the frame
///     carried no flow pair.
/// </param>
/// <param name="FlowOrigin">
///     Origin of <paramref name="FlowId" />, or <see langword="null" /> under the same
///     conditions.
/// </param>
public sealed record ZlinkStreamMessage<TPayload>(
    string Name,
    ZlinkStreamMetadata Metadata,
    TPayload Payload,
    string? FlowId = null,
    ZlinkStreamFlowOrigin? FlowOrigin = null
);

public sealed record ZlinkStreamError(
    ZlinkStreamErrorCode Code,
    string Message,
    Exception? Exception = null
);

public sealed record ZlinkStreamConnectionStateChanged(
    ZlinkStreamConnectionState Previous,
    ZlinkStreamConnectionState Current,
    ZlinkStreamError? Error = null
);

public sealed class ZlinkStreamException(ZlinkStreamError error)
    : Exception(error.Message, error.Exception)
{
    public ZlinkStreamError Error { get; } = error;
}

public readonly struct ZlinkStreamResult
{
    private ZlinkStreamResult(ZlinkStreamError? error)
    {
        Error = error;
    }

    public bool IsSuccess => Error is null;

    public ZlinkStreamError? Error { get; }

    public static ZlinkStreamResult Success()
    {
        return new ZlinkStreamResult(null);
    }

    public static ZlinkStreamResult Failure(ZlinkStreamError error)
    {
        return new ZlinkStreamResult(error);
    }
}

public readonly struct ZlinkStreamResult<T>
{
    private ZlinkStreamResult(T? value, ZlinkStreamError? error)
    {
        Value = value;
        Error = error;
    }

    public bool IsSuccess => Error is null;

    public T? Value { get; }

    public ZlinkStreamError? Error { get; }

    public static ZlinkStreamResult<T> Success(T value)
    {
        return new ZlinkStreamResult<T>(value, null);
    }

    public static ZlinkStreamResult<T> Failure(ZlinkStreamError error)
    {
        return new ZlinkStreamResult<T>(default, error);
    }
}
