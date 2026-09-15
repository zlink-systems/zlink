namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamReceivedMessages
{
    private readonly object _gate = new();
    private readonly Dictionary<string, List<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>> _messages = new(StringComparer.Ordinal);
    private TaskCompletionSource _arrived = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public int Count(string name)
    {
        lock (_gate)
        {
            return _messages.TryGetValue(name, out var messages) ? messages.Count : 0;
        }
    }

    public void Record(ZlinkStreamMessage<ZlinkStreamEncodedPayload> message)
    {
        TaskCompletionSource arrived;
        lock (_gate)
        {
            if (!_messages.TryGetValue(message.Name, out var messages))
            {
                messages = [];
                _messages.Add(message.Name, messages);
            }

            messages.Add(message);
            arrived = _arrived;
            _arrived = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        }

        arrived.TrySetResult();
    }

    public async ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> WaitForAsync(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        using var timeoutSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutSource.CancelAfter(timeout);

        while (!timeoutSource.IsCancellationRequested)
        {
            var pending = TryTakeOrWait(name, predicate, timeoutSource.Token);
            if (pending.Message is not null) return pending.Message;

            try
            {
                await pending.WaitTask!.ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (timeoutSource.IsCancellationRequested
                                                     && !cancellationToken.IsCancellationRequested)
            {
                break;
            }
        }

        cancellationToken.ThrowIfCancellationRequested();
        throw new TimeoutException($"Timed out waiting for '{name}' stream message.");
    }

    private PendingMessage TryTakeOrWait(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate,
        CancellationToken cancellationToken)
    {
        lock (_gate)
        {
            var message = TryTakeLocked(name, predicate);
            return message is not null
                ? new PendingMessage(message, null)
                : new PendingMessage(null, _arrived.Task.WaitAsync(cancellationToken));
        }
    }

    private ZlinkStreamMessage<ZlinkStreamEncodedPayload>? TryTakeLocked(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate)
    {
        if (!_messages.TryGetValue(name, out var messages)) return null;

        for (var index = 0; index < messages.Count; index++)
        {
            var message = messages[index];
            if (predicate is not null && !predicate(message)) continue;

            messages.RemoveAt(index);
            if (messages.Count == 0) _messages.Remove(name);
            return message;
        }

        return null;
    }

    private readonly record struct PendingMessage(
        ZlinkStreamMessage<ZlinkStreamEncodedPayload>? Message,
        Task? WaitTask);
}
