namespace Systems.Zlink.Stream.Connector.Runtime.Calls;

internal sealed class ZlinkStreamWaitBuilder : IZlinkStreamWaitCall
{
    private readonly IZlinkStreamConnectorInternal _connector;
    private readonly ZlinkStreamCallBuilderState _state;
    private Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? _predicate;

    internal ZlinkStreamWaitBuilder(IZlinkStreamConnectorInternal connector, string name)
    {
        _connector = connector;
        _state = new ZlinkStreamCallBuilderState(name);
    }

    public IZlinkStreamWaitCall Timeout(TimeSpan timeout)
    {
        _state.SetTimeout(timeout);
        return this;
    }

    public IZlinkStreamWaitCall Where(
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate
    )
    {
        ArgumentNullException.ThrowIfNull(predicate);
        var previous = _predicate;
        _predicate = previous is null
            ? predicate
            : message => previous(message) && predicate(message);
        return this;
    }

    public async ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> Async(
        CancellationToken cancellationToken = default
    )
    {
        _state.EnsureNotExecuted();
        var name = _state.ResolveMessageName();
        var timeout = _state.Timeout ?? _connector.Options.WaitTimeout;
        var message = await _connector
            .WaitForEncodedAsync(name, _predicate, timeout, cancellationToken)
            .ConfigureAwait(false);

        // A wait surface reports every violation as ValidationFailed carried by
        // ZlinkStreamException (stream-connector spec §10.1, §9.2).
        return message
            ?? throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                $"Timed out after {timeout} waiting for '{name}' stream message."
            );
    }
}
