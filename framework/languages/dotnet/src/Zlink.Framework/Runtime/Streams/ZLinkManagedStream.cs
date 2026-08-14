namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkManagedStream : IZLinkStream
{
    private readonly ZLinkCodecRegistryBuilder _codecs;
    private readonly RoutingId _routingId;
    private readonly IZLinkBackendStreamSocket _socket;

    public ZLinkManagedStream(
        IZLinkBackendStreamSocket socket,
        RoutingId routingId,
        ZLinkCodecRegistryBuilder codecs,
        string transport)
    {
        _socket = socket;
        _routingId = routingId;
        _codecs = codecs;
        Transport = transport;
        SessionId = _routingId.ToHex();
    }

    public string SessionId { get; }

    internal string Transport { get; }

    public RoutingId? RoutingId => _routingId;

    public string? LocalAddr { get; private set; }

    public string? RemoteAddr { get; private set; }

    public bool Write(
        ZLinkMessage payload,
        SendFlags flags = SendFlags.None)
    {
        ArgumentNullException.ThrowIfNull(payload);
        using var raw = payload.ToRawMessage(_codecs);
        return WriteRaw(raw, flags);
    }

    public ValueTask CloseAsync()
    {
        _socket.DisconnectPeer(_routingId);
        return ValueTask.CompletedTask;
    }

    internal bool WriteRaw(
        Message payload,
        SendFlags flags = SendFlags.None)
    {
        return _socket.Send(_routingId, payload, flags);
    }

    internal ValueTask SubmitRawAsync(
        Message payload,
        CancellationToken cancellationToken)
    {
        return _socket.SendAsync(_routingId, payload, cancellationToken);
    }

    public void UpdateAddresses(string? localAddr, string? remoteAddr)
    {
        LocalAddr = localAddr;
        RemoteAddr = remoteAddr;
    }

    internal async ValueTask BindActorAsync(
        ZLinkBackendActorRef actor,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        await _socket.BindActorAsync(_routingId, actor, timeout, cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask UnbindActorAsync(
        string actorId,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        await _socket.UnbindActorAsync(_routingId, actorId, timeout, cancellationToken)
            .ConfigureAwait(false);
    }

    internal bool SendBoundActor(
        string actorId,
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        return _socket.SendBoundActor(_routingId, actorId, parts, flags);
    }
}
