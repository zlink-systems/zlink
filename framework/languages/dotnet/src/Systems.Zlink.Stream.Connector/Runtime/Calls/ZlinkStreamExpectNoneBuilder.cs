namespace Systems.Zlink.Stream.Connector.Runtime.Calls;

internal sealed class ZlinkStreamExpectNoneBuilder : IZlinkStreamExpectNoneCall
{
    private readonly IZlinkStreamConnectorInternal _connector;
    private readonly ZlinkStreamCallBuilderState _state;
    private TimeSpan? _window;

    internal ZlinkStreamExpectNoneBuilder(IZlinkStreamConnectorInternal connector, string name)
    {
        _connector = connector;
        _state = new ZlinkStreamCallBuilderState(name);
    }

    public IZlinkStreamExpectNoneCall Within(TimeSpan window)
    {
        if (window <= TimeSpan.Zero)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "ExpectNone observation window must be greater than zero."
            );
        _window = window;
        return this;
    }

    public async ValueTask Async(CancellationToken cancellationToken = default)
    {
        _state.EnsureNotExecuted();
        var name = _state.ResolveMessageName();
        var window =
            _window
            ?? throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "ExpectNone requires Within(window)."
            );

        var message = await _connector
            .WaitForEncodedAsync(name, null, window, cancellationToken)
            .ConfigureAwait(false);

        // The negative observation holds exactly when nothing arrived; an arrival is the
        // violation and violations are ValidationFailed (stream-connector spec §10.1).
        if (message is null)
            return;

        throw ZlinkStreamConnector.Error(
            ZlinkStreamErrorCode.ValidationFailed,
            $"Expected no '{name}' stream message within {window}."
        );
    }
}
