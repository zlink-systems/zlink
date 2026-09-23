namespace Systems.Zlink.Stream.Connector.Contracts;

public sealed record ZlinkStreamDisconnected(ZlinkStreamCloseReason CloseReason);

public sealed record ZlinkStreamEncodedPayload(
    ZlinkStreamCodec Codec,
    ReadOnlyMemory<byte> Payload,
    Type? MessageType = null
);

public sealed record ZlinkStreamMessage(string Name, ZlinkStreamMetadata Metadata, object? Payload);

/// <summary>A received stream message.</summary>
/// <param name="Name">Packet name the frame carried.</param>
/// <param name="Metadata">Metadata key-value pairs the frame carried.</param>
/// <param name="Payload">Decoded payload.</param>
/// <param name="ActorId">The bound Actor on the server side, or <see langword="null" />.</param>
public sealed record ZlinkStreamMessage<TPayload>(
    string Name,
    ZlinkStreamMetadata Metadata,
    TPayload Payload,
    string? ActorId = null
);

public sealed class ZlinkStreamRequestSendingContext
{
    internal ZlinkStreamRequestSendingContext(
        string requestPacketName,
        string? actorId,
        ZlinkStreamMetadata metadata
    )
    {
        RequestPacketName = requestPacketName;
        ActorId = actorId;
        Metadata = metadata;
    }

    public string RequestPacketName { get; }
    public string? ActorId { get; }
    internal ZlinkStreamMetadata Metadata { get; private set; }

    public void SetMetadata(string key, string value) => Metadata = Metadata.With(key, value);
}

public sealed class ZlinkStreamReplyReceivedContext
{
    internal ZlinkStreamReplyReceivedContext(
        string requestPacketName,
        string? actorId,
        bool succeeded,
        ZlinkStreamMessage<ZlinkStreamEncodedPayload>? reply,
        ZlinkStreamError? error,
        TimeSpan elapsed
    )
    {
        RequestPacketName = requestPacketName;
        ActorId = actorId;
        Succeeded = succeeded;
        Reply = reply;
        Error = error;
        Elapsed = elapsed;
    }

    public string RequestPacketName { get; }
    public string? ActorId { get; }
    public bool Succeeded { get; }
    public ZlinkStreamMessage<ZlinkStreamEncodedPayload>? Reply { get; }
    public ZlinkStreamError? Error { get; }
    public TimeSpan Elapsed { get; }
}

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
