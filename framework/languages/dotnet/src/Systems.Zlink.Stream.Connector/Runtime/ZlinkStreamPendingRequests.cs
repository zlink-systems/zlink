using System.Collections.Concurrent;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamPendingRequests
{
    private readonly ConcurrentDictionary<ulong, PendingRequest> _pending = new();
    private long _nextRequestSeq;

    public PendingRequest Create(string packetName)
    {
        var requestSeq = NextRequestSeq();
        var pending = new PendingRequest(requestSeq, packetName);
        if (!_pending.TryAdd(requestSeq.Value, pending))
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Duplicate request sequence.");

        return pending;
    }

    public bool TryComplete(
        ZlinkStreamHeader header,
        ZlinkStreamFrame frame,
        Func<ReadOnlyMemory<byte>, ZlinkStreamError> parseError)
    {
        if (header.RequestSeq is not { } requestSeq
            || (header.Kind != ZlinkStreamMessageKind.Response && header.Kind != ZlinkStreamMessageKind.Error)
            || !_pending.TryRemove(requestSeq.Value, out var pending))
            return false;

        // Stream connector spec 5.2: a pending request is matched by request_seq alone.
        // New replies use an empty name; a legacy peer's non-empty name is ignored here.
        pending.Complete(new ZlinkStreamPendingCompletion(
            header,
            frame,
            header.Kind == ZlinkStreamMessageKind.Error ? parseError(frame.Payload) : null));
        return true;
    }

    public ValueTask<ZlinkStreamPendingCompletion> WaitAsync(
        PendingRequest pending,
        CancellationToken cancellationToken)
    {
        return new ValueTask<ZlinkStreamPendingCompletion>(pending.Task.WaitAsync(cancellationToken));
    }

    public void Remove(ZlinkStreamRequestSeq requestSeq)
    {
        _pending.TryRemove(requestSeq.Value, out _);
    }

    public void FailAll(ZlinkStreamError error)
    {
        foreach (var (requestSeq, pending) in _pending)
            if (_pending.TryRemove(requestSeq, out _))
                pending.Fail(error);
    }

    private ZlinkStreamRequestSeq NextRequestSeq()
    {
        while (true)
        {
            var value = unchecked((ulong)Interlocked.Increment(ref _nextRequestSeq));
            if (value != 0) return new ZlinkStreamRequestSeq(value);
        }
    }

    internal sealed class PendingRequest(ZlinkStreamRequestSeq requestSeq, string packetName)
    {
        private readonly TaskCompletionSource<ZlinkStreamPendingCompletion> _completion =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ZlinkStreamRequestSeq RequestSeq { get; } = requestSeq;

        public string PacketName { get; } = packetName;

        public Task<ZlinkStreamPendingCompletion> Task => _completion.Task;

        public void Complete(ZlinkStreamPendingCompletion completion)
        {
            _completion.TrySetResult(completion);
        }

        public void Fail(ZlinkStreamError error)
        {
            _completion.TrySetException(new ZlinkStreamException(error));
        }
    }
}

internal sealed record ZlinkStreamPendingCompletion(
    ZlinkStreamHeader Header,
    ZlinkStreamFrame Frame,
    ZlinkStreamError? Error);
