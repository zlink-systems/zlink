namespace Zlink.Framework.Runtime.Backend.Contracts;

// STREAM keeps a semantic adapter because Framework coordinates actor binding,
// packet receive ownership, and mesh completions through one lifecycle boundary.
// Ordinary DEALER, ROUTER, PUB, and SUB channels use the binding interfaces
// directly; duplicating those interfaces here would add no domain meaning.
internal interface IZLinkBackendStreamSocket : IAsyncDisposable
{
    void Bind(string endpoint);

    IZLinkBackendSocketPoller CreateReceivePoller() =>
        throw new NotSupportedException(
            "The backend stream socket does not provide a receive poller.");

    void ApplySocketConfig(IZLinkSocketConfig config) { }

    string GetLastEndpoint() => string.Empty;

    void SetTlsServer(string certPath, string keyPath, bool requireClientCert);

    bool RecvPacket(
        out ZLinkBackendStreamReceive? received,
        RecvFlags flags = RecvFlags.None);

    bool Send(
        RoutingId routingId,
        Message payload,
        SendFlags flags);

    ValueTask SendAsync(
        RoutingId routingId,
        Message payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            if (!Send(routingId, payload, SendFlags.DontWait))
                throw new ZlinkSubmitException(
                    ZlinkSubmitException.ErrorCode.Backpressured);
            return ValueTask.CompletedTask;
        }
        finally
        {
            payload.Dispose();
        }
    }

    bool Send(
        RoutingId routingId,
        IReadOnlyList<Message> parts,
        SendFlags flags);

    void DisconnectPeer(RoutingId routingId);

    ValueTask BindActorAsync(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        TimeSpan timeout,
        CancellationToken cancellationToken);

    ValueTask UnbindActorAsync(
        RoutingId sessionRid,
        string actorId,
        TimeSpan timeout,
        CancellationToken cancellationToken);

    bool SendBoundActor(
        RoutingId sessionRid,
        string actorId,
        IReadOnlyList<Message> parts,
        SendFlags flags);
}

internal sealed class ZLinkBackendStreamReceive(
    RoutingId? sourceRoutingId,
    Message? header,
    Message? payload) : IDisposable
{
    private Message? _header = header;
    private Message? _payload = payload;

    internal RoutingId? SourceRoutingId { get; } = sourceRoutingId;

    internal bool HasPacket => _header is not null && _payload is not null;

    internal long ByteLength => checked(
        (long)(_header?.Size ?? 0) + (_payload?.Size ?? 0));

    internal (Message Header, Message Payload) TakePacket()
    {
        if (_header is null || _payload is null)
            throw new InvalidDataException(
                "A Core STREAM packet must contain one header and one payload.");
        var packet = (_header, _payload);
        _header = null;
        _payload = null;
        return packet;
    }

    public void Dispose()
    {
        Interlocked.Exchange(ref _header, null)?.Dispose();
        Interlocked.Exchange(ref _payload, null)?.Dispose();
    }
}

internal interface IZLinkBackendSocketMonitor : IAsyncDisposable
{
    bool Wait(TimeSpan timeout) =>
        throw new NotSupportedException(
            "The backend socket monitor does not provide a poll wait.");

    bool TryRecv(out ZLinkBackendSocketMonitorEvent monitorEvent);
}
