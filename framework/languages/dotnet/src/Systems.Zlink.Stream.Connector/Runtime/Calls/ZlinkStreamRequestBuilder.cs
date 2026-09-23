namespace Systems.Zlink.Stream.Connector.Runtime.Calls;

internal sealed class ZlinkStreamRequestBuilder : IZlinkStreamRequestCall
{
    private readonly ZlinkStreamEncodedPayload _body;
    private readonly IZlinkStreamConnectorInternal _connector;
    private readonly ZlinkStreamCallBuilderState _state;
    private readonly Func<ushort?> _actorSlot;
    private readonly string? _actorId;

    internal ZlinkStreamRequestBuilder(
        IZlinkStreamConnectorInternal connector,
        string? name,
        ZlinkStreamEncodedPayload payload,
        Func<ushort?>? actorSlot = null,
        string? actorId = null
    )
    {
        _connector = connector;
        _body = payload;
        _actorSlot = actorSlot ?? (() => null);
        _actorId = actorId;
        _state = new ZlinkStreamCallBuilderState(name);
    }

    public IZlinkStreamRequestCall PacketName(string name)
    {
        _state.SetMessageName(name);
        return this;
    }

    public IZlinkStreamRequestCall Metadata(string key, string value)
    {
        _state.AddMetadata(key, value);
        return this;
    }

    public IZlinkStreamRequestCall Metadata(ZlinkStreamMetadata metadata)
    {
        _state.SetMetadata(metadata);
        return this;
    }

    public IZlinkStreamRequestCall Timeout(TimeSpan timeout)
    {
        _state.SetTimeout(timeout);
        return this;
    }

    public IZlinkStreamRequestCall Compress()
    {
        _state.EnableCompression();
        return this;
    }

    public async ValueTask<ZlinkStreamEncodedPayload> Async(
        CancellationToken cancellationToken = default
    )
    {
        _state.EnsureNotExecuted();
        return await _connector
            .RequestEncodedAsync(
                _state.ResolveMessageName(),
                _body,
                _state.Metadata,
                _state.Compress,
                _state.Timeout ?? _connector.Options.RequestTimeout,
                _actorSlot(),
                _actorId,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    public void Submit(Action<ZlinkStreamResult> callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        _state.EnsureNotExecuted();
        _connector.RequestEncoded(
            _state.ResolveMessageName(),
            _body,
            _state.Metadata,
            _state.Compress,
            _state.Timeout ?? _connector.Options.RequestTimeout,
            _actorSlot(),
            _actorId,
            callback
        );
    }

    public void Submit(Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        _state.EnsureNotExecuted();
        _connector.RequestEncoded(
            _state.ResolveMessageName(),
            _body,
            _state.Metadata,
            _state.Compress,
            _state.Timeout ?? _connector.Options.RequestTimeout,
            _actorSlot(),
            _actorId,
            callback
        );
    }
}
