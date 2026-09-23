namespace Systems.Zlink.Stream.Connector.Runtime.Calls;

internal sealed class ZlinkStreamSendBuilder : IZlinkStreamSendCall
{
    private readonly ZlinkStreamEncodedPayload _body;
    private readonly IZlinkStreamConnectorInternal _connector;
    private readonly ZlinkStreamCallBuilderState _state;
    private readonly Func<ushort?> _actorSlot;

    internal ZlinkStreamSendBuilder(
        IZlinkStreamConnectorInternal connector,
        string? name,
        ZlinkStreamEncodedPayload payload,
        Func<ushort?>? actorSlot = null
    )
    {
        _connector = connector;
        _body = payload;
        _actorSlot = actorSlot ?? (() => null);
        _state = new ZlinkStreamCallBuilderState(name);
    }

    public IZlinkStreamSendCall PacketName(string name)
    {
        _state.SetMessageName(name);
        return this;
    }

    public IZlinkStreamSendCall Metadata(string key, string value)
    {
        _state.AddMetadata(key, value);
        return this;
    }

    public IZlinkStreamSendCall Metadata(ZlinkStreamMetadata metadata)
    {
        _state.SetMetadata(metadata);
        return this;
    }

    public IZlinkStreamSendCall Compress()
    {
        _state.EnableCompression();
        return this;
    }

    public ValueTask Async(CancellationToken cancellationToken = default)
    {
        _state.EnsureNotExecuted();
        var name = _state.ResolveMessageName();
        var frame = _connector.BuildSendFrame(
            ZlinkStreamMessageKind.Send,
            name,
            _body,
            _state.Metadata,
            _state.Compress,
            _actorSlot()
        );

        return _connector.SubmitFrameAsync(frame, cancellationToken);
    }
}
