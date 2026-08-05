using System.Text.Json;

namespace Systems.Zlink.Stream.Connector.Contracts;

public static class ZlinkStreamJsonCodec
{
    private static JsonSerializerOptions _serializerOptions = new(JsonSerializerDefaults.Web);

    public static JsonSerializerOptions SerializerOptions
    {
        get => new(Volatile.Read(ref _serializerOptions));
        private set => Volatile.Write(ref _serializerOptions, new JsonSerializerOptions(value));
    }

    internal static JsonSerializerOptions Snapshot => Volatile.Read(ref _serializerOptions);

    public static void Configure(JsonSerializerOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        SerializerOptions = options;
    }

}

public static class ZlinkStreamJsonExtensions
{
    public static T FromJson<T>(this ZlinkStreamEncodedPayload payload)
    {
        EnsureJson(payload);
        return JsonSerializer.Deserialize<T>(payload.Payload.Span, ZlinkStreamJsonCodec.Snapshot)!;
    }

    public static ZlinkStreamEncodedPayload ToJson<T>(this T value)
    {
        return new ZlinkStreamEncodedPayload(
            ZlinkStreamCodec.Json,
            JsonSerializer.SerializeToUtf8Bytes(value, ZlinkStreamJsonCodec.Snapshot),
            typeof(T));
    }

    private static void EnsureJson(ZlinkStreamEncodedPayload payload)
    {
        if (payload.Codec != ZlinkStreamCodec.Json)
            throw new InvalidOperationException($"Stream payload codec is {payload.Codec}, not Json.");
    }
}

public static class ZlinkStreamTypedConnectorExtensions
{
    public static IZlinkStreamConnector WithInboundObserver(
        this IZlinkStreamConnector connector,
        Func<ZlinkStreamInboundObservation, CancellationToken, ValueTask> observer)
    {
        ArgumentNullException.ThrowIfNull(connector);
        connector.ObserveInbound(observer);
        return connector;
    }

    public static ZlinkStreamTypedSendBuilder Send<TPayload>(
        this IZlinkStreamConnector connector,
        TPayload payload)
    {
        ArgumentNullException.ThrowIfNull(connector);
        return new ZlinkStreamTypedSendBuilder(
            connector.Send(EncodePayload(connector.Options.PayloadCodec, payload)));
    }

    public static ZlinkStreamTypedRequestBuilder Request<TPayload>(
        this IZlinkStreamConnector connector,
        TPayload payload)
    {
        ArgumentNullException.ThrowIfNull(connector);
        return new ZlinkStreamTypedRequestBuilder(
            connector.Request(EncodePayload(connector.Options.PayloadCodec, payload)),
            connector.Options.PayloadCodec);
    }

    internal static ZlinkStreamEncodedPayload EncodePayload<TPayload>(
        IZlinkStreamPayloadCodec? codec,
        TPayload payload)
    {
        return codec is null ? payload.ToJson() : codec.Encode(payload);
    }

    internal static TPayload DecodePayload<TPayload>(
        IZlinkStreamPayloadCodec? codec,
        ZlinkStreamEncodedPayload payload)
    {
        return codec is null ? payload.FromJson<TPayload>() : codec.Decode<TPayload>(payload);
    }

    public static IDisposable On<TPayload>(
        this IZlinkStreamConnector connector,
        Func<ZlinkStreamMessage<TPayload>, CancellationToken, ValueTask> handler)
    {
        ArgumentNullException.ThrowIfNull(connector);
        return connector.On(connector.Options.NameResolver.Resolve(typeof(TPayload)), handler);
    }

    public static IDisposable On<TPayload>(
        this IZlinkStreamConnector connector,
        string name,
        Func<ZlinkStreamMessage<TPayload>, CancellationToken, ValueTask> handler)
    {
        ArgumentNullException.ThrowIfNull(connector);
        ArgumentNullException.ThrowIfNull(handler);
        return connector.On(name, (message, cancellationToken) =>
        {
            var payload = DecodePayload<TPayload>(connector.Options.PayloadCodec, message.Payload);
            return handler(new ZlinkStreamMessage<TPayload>(message.Name, message.Metadata, payload),
                cancellationToken);
        });
    }

    public static ZlinkStreamTypedWaitBuilder<TPayload> WaitFor<TPayload>(
        this IZlinkStreamConnector connector,
        string name)
    {
        ArgumentNullException.ThrowIfNull(connector);
        return new ZlinkStreamTypedWaitBuilder<TPayload>(connector.WaitFor(name), connector.Options.PayloadCodec);
    }

    public static ZlinkStreamTypedWaitBuilder<TPayload> WaitFor<TPayload>(
        this IZlinkStreamConnector connector)
    {
        ArgumentNullException.ThrowIfNull(connector);
        return connector.WaitFor<TPayload>(connector.Options.NameResolver.Resolve(typeof(TPayload)));
    }

    public static ZlinkStreamTypedExpectNoneBuilder<TPayload> ExpectNone<TPayload>(
        this IZlinkStreamConnector connector,
        string name)
    {
        ArgumentNullException.ThrowIfNull(connector);
        return new ZlinkStreamTypedExpectNoneBuilder<TPayload>(connector.ExpectNone(name));
    }

    public static ZlinkStreamTypedExpectNoneBuilder<TPayload> ExpectNone<TPayload>(
        this IZlinkStreamConnector connector)
    {
        ArgumentNullException.ThrowIfNull(connector);
        return connector.ExpectNone<TPayload>(connector.Options.NameResolver.Resolve(typeof(TPayload)));
    }

    public static ZlinkStreamTypedSequenceBuilder<TPayload> WaitForSequence<TPayload>(
        this IZlinkStreamConnector connector,
        string name)
    {
        ArgumentNullException.ThrowIfNull(connector);
        return new ZlinkStreamTypedSequenceBuilder<TPayload>(
            connector.WaitForSequence(name),
            connector.Options.PayloadCodec);
    }

    public static ZlinkStreamTypedSequenceBuilder<TPayload> WaitForSequence<TPayload>(
        this IZlinkStreamConnector connector)
    {
        ArgumentNullException.ThrowIfNull(connector);
        return connector.WaitForSequence<TPayload>(
            connector.Options.NameResolver.Resolve(typeof(TPayload)));
    }
}

/// <summary>
///     Configures a typed negative observation for one packet name.
/// </summary>
public sealed class ZlinkStreamTypedExpectNoneBuilder<TPayload>
{
    private readonly IZlinkStreamExpectNoneCall _inner;

    internal ZlinkStreamTypedExpectNoneBuilder(IZlinkStreamExpectNoneCall inner)
    {
        _inner = inner;
    }

    /// <summary>
    ///     Sets the interval during which the packet must not arrive.
    /// </summary>
    public ZlinkStreamTypedExpectNoneBuilder<TPayload> Within(TimeSpan window)
    {
        _inner.Within(window);
        return this;
    }

    /// <summary>
    ///     Executes the negative observation.
    /// </summary>
    public ValueTask Async(CancellationToken cancellationToken = default)
    {
        return _inner.Async(cancellationToken);
    }
}

/// <summary>
///     Configures typed predicates that must match messages in arrival order.
/// </summary>
public sealed class ZlinkStreamTypedSequenceBuilder<TPayload>
{
    private readonly IZlinkStreamPayloadCodec? _codec;
    private readonly IZlinkStreamSequenceCall _inner;

    internal ZlinkStreamTypedSequenceBuilder(
        IZlinkStreamSequenceCall inner,
        IZlinkStreamPayloadCodec? codec)
    {
        _inner = inner;
        _codec = codec;
    }

    /// <summary>
    ///     Appends the next expected typed message predicate.
    /// </summary>
    public ZlinkStreamTypedSequenceBuilder<TPayload> Expect(
        Func<ZlinkStreamMessage<TPayload>, bool> predicate)
    {
        ArgumentNullException.ThrowIfNull(predicate);
        _inner.Expect(message => predicate(Decode(message)));
        return this;
    }

    /// <summary>
    ///     Sets the total timeout for the complete sequence.
    /// </summary>
    public ZlinkStreamTypedSequenceBuilder<TPayload> Timeout(TimeSpan timeout)
    {
        _inner.Timeout(timeout);
        return this;
    }

    /// <summary>
    ///     Verifies the sequence and returns typed messages in arrival order.
    /// </summary>
    public async ValueTask<IReadOnlyList<ZlinkStreamMessage<TPayload>>> Async(
        CancellationToken cancellationToken = default)
    {
        var messages = await _inner.Async(cancellationToken).ConfigureAwait(false);
        return messages.Select(Decode).ToArray();
    }

    private ZlinkStreamMessage<TPayload> Decode(
        ZlinkStreamMessage<ZlinkStreamEncodedPayload> message)
    {
        return new ZlinkStreamMessage<TPayload>(
            message.Name,
            message.Metadata,
            ZlinkStreamTypedConnectorExtensions.DecodePayload<TPayload>(_codec, message.Payload));
    }
}

public sealed class ZlinkStreamTypedWaitBuilder<TPayload>
{
    private readonly IZlinkStreamPayloadCodec? _codec;
    private readonly IZlinkStreamWaitCall _inner;

    internal ZlinkStreamTypedWaitBuilder(IZlinkStreamWaitCall inner, IZlinkStreamPayloadCodec? codec)
    {
        _inner = inner;
        _codec = codec;
    }

    public ZlinkStreamTypedWaitBuilder<TPayload> Timeout(TimeSpan timeout)
    {
        _inner.Timeout(timeout);
        return this;
    }

    public ZlinkStreamTypedWaitBuilder<TPayload> Where(Func<ZlinkStreamMessage<TPayload>, bool> predicate)
    {
        ArgumentNullException.ThrowIfNull(predicate);
        _inner.Where(message => predicate(new ZlinkStreamMessage<TPayload>(
            message.Name,
            message.Metadata,
            ZlinkStreamTypedConnectorExtensions.DecodePayload<TPayload>(_codec, message.Payload))));
        return this;
    }

    public async ValueTask<ZlinkStreamMessage<TPayload>> Async(
        CancellationToken cancellationToken = default)
    {
        var message = await _inner.Async(cancellationToken).ConfigureAwait(false);
        return new ZlinkStreamMessage<TPayload>(
            message.Name,
            message.Metadata,
            ZlinkStreamTypedConnectorExtensions.DecodePayload<TPayload>(_codec, message.Payload));
    }
}

public sealed class ZlinkStreamTypedSendBuilder
{
    private readonly IZlinkStreamSendCall _inner;

    internal ZlinkStreamTypedSendBuilder(IZlinkStreamSendCall inner)
    {
        _inner = inner;
    }

    public ZlinkStreamTypedSendBuilder PacketName(string name)
    {
        _inner.PacketName(name);
        return this;
    }

    public ZlinkStreamTypedSendBuilder Metadata(string key, string value)
    {
        _inner.Metadata(key, value);
        return this;
    }

    public ZlinkStreamTypedSendBuilder Metadata(ZlinkStreamMetadata metadata)
    {
        _inner.Metadata(metadata);
        return this;
    }

    public ZlinkStreamTypedSendBuilder Compress()
    {
        _inner.Compress();
        return this;
    }

    public ValueTask Async(CancellationToken cancellationToken = default)
    {
        return _inner.Async(cancellationToken);
    }
}

public sealed class ZlinkStreamTypedRequestBuilder
{
    private readonly IZlinkStreamPayloadCodec? _codec;
    private readonly IZlinkStreamRequestCall _inner;

    internal ZlinkStreamTypedRequestBuilder(IZlinkStreamRequestCall inner, IZlinkStreamPayloadCodec? codec)
    {
        _inner = inner;
        _codec = codec;
    }

    public ZlinkStreamTypedRequestBuilder PacketName(string name)
    {
        _inner.PacketName(name);
        return this;
    }

    public ZlinkStreamTypedRequestBuilder Metadata(string key, string value)
    {
        _inner.Metadata(key, value);
        return this;
    }

    public ZlinkStreamTypedRequestBuilder Metadata(ZlinkStreamMetadata metadata)
    {
        _inner.Metadata(metadata);
        return this;
    }

    public ZlinkStreamTypedRequestBuilder Timeout(TimeSpan timeout)
    {
        _inner.Timeout(timeout);
        return this;
    }

    public ZlinkStreamTypedRequestBuilder Compress()
    {
        _inner.Compress();
        return this;
    }

    public async ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default)
    {
        var reply = await _inner.Async(cancellationToken).ConfigureAwait(false);
        return ZlinkStreamTypedConnectorExtensions.DecodePayload<TReply>(_codec, reply);
    }

    public void Submit(Action<ZlinkStreamResult> callback)
    {
        _inner.Submit(callback);
    }

    public void Submit<TReply>(Action<ZlinkStreamResult<TReply>> callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        _inner.Submit(result =>
        {
            if (!result.IsSuccess)
            {
                callback(ZlinkStreamResult<TReply>.Failure(result.Error!));
                return;
            }

            try
            {
                callback(ZlinkStreamResult<TReply>.Success(
                    ZlinkStreamTypedConnectorExtensions.DecodePayload<TReply>(_codec, result.Value!)));
            }
            catch (Exception ex)
            {
                callback(ZlinkStreamResult<TReply>.Failure(new ZlinkStreamError(
                    ZlinkStreamErrorCode.UserCallbackFailed,
                    "Stream reply decode failed.",
                    ex)));
            }
        });
    }
}
