namespace Systems.Zlink.Stream.Connector.Runtime.Calls;

internal sealed class ZlinkStreamExpectNoneBuilder : IZlinkStreamExpectNoneCall
{
    private readonly IZlinkStreamConnectorInternal _connector;
    private readonly ZlinkStreamCallBuilderState _state;
    private TimeSpan? _window;

    internal ZlinkStreamExpectNoneBuilder(
        IZlinkStreamConnectorInternal connector,
        string name)
    {
        _connector = connector;
        _state = new ZlinkStreamCallBuilderState(name);
    }

    public IZlinkStreamExpectNoneCall Within(TimeSpan window)
    {
        if (window <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(window), "Observation window must be greater than zero.");
        _window = window;
        return this;
    }

    public async ValueTask Async(CancellationToken cancellationToken = default)
    {
        _state.EnsureNotExecuted();
        var name = _state.ResolveMessageName();
        var window = _window
                     ?? throw ZlinkStreamConnector.Error(
                         ZlinkStreamErrorCode.ValidationFailed,
                         "ExpectNone requires Within(window).");
        try
        {
            await _connector.WaitForEncodedAsync(name, null, window, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            return;
        }

        throw new InvalidOperationException(
            $"Expected no '{name}' stream message within {window}.");
    }
}
