using System.Diagnostics;

namespace Systems.Zlink.Stream.Connector.Runtime.Calls;

internal sealed class ZlinkStreamSequenceBuilder : IZlinkStreamSequenceCall
{
    private readonly IZlinkStreamConnectorInternal _connector;
    private readonly List<Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>> _expectations = [];
    private readonly ZlinkStreamCallBuilderState _state;

    internal ZlinkStreamSequenceBuilder(
        IZlinkStreamConnectorInternal connector,
        string name)
    {
        _connector = connector;
        _state = new ZlinkStreamCallBuilderState(name);
    }

    public IZlinkStreamSequenceCall Expect(
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate)
    {
        ArgumentNullException.ThrowIfNull(predicate);
        _expectations.Add(predicate);
        return this;
    }

    public IZlinkStreamSequenceCall Timeout(TimeSpan timeout)
    {
        if (timeout <= TimeSpan.Zero)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "WaitForSequence timeout must be greater than zero.");
        _state.SetTimeout(timeout);
        return this;
    }

    public async ValueTask<IReadOnlyList<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>> Async(
        CancellationToken cancellationToken = default)
    {
        _state.EnsureNotExecuted();
        if (_expectations.Count == 0)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "WaitForSequence requires at least one expectation.");

        var name = _state.ResolveMessageName();
        var timeout = _state.Timeout ?? _connector.Options.WaitTimeout;
        var elapsed = Stopwatch.StartNew();
        var messages = new List<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>(_expectations.Count);
        for (var index = 0; index < _expectations.Count; index++)
        {
            var remaining = timeout - elapsed.Elapsed;
            // Both a timeout and an out-of-order arrival are violations of this
            // observation, so both are ValidationFailed (stream-connector spec §10.1).
            if (remaining <= TimeSpan.Zero)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    $"Timed out after {timeout} waiting for the '{name}' stream message sequence.");

            var message = await _connector.WaitForEncodedAsync(name, null, remaining, cancellationToken)
                .ConfigureAwait(false);
            if (message is null)
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    $"Timed out after {timeout} waiting for the '{name}' stream message sequence.");

            if (!_expectations[index](message))
                throw ZlinkStreamConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    $"Stream message '{name}' arrived out of the expected sequence at index {index}.");
            messages.Add(message);
        }

        return messages;
    }
}
