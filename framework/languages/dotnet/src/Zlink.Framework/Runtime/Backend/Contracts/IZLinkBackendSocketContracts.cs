namespace Zlink.Framework.Runtime.Backend.Contracts;

internal interface IZLinkBackendSocket : IAsyncDisposable
{
    void Bind(string endpoint);

    void SetChannelName(string channelName);
}

internal interface IZLinkBackendSocketOptions : IZLinkBackendSocket
{
    void ApplySocketConfig(IZLinkSocketConfig config);

    void SetMaxMessageSize(long value);

    void SetSendHighWaterMark(ulong value);

    void SetReceiveHighWaterMark(ulong value);
}

internal interface IZLinkBackendConnectableSocket : IZLinkBackendSocket
{
    void Connect(string endpoint);

    void Disconnect(string endpoint);
}

// ROUTER/DEALER serving socket 의 advertised peer weight 를 런타임에 읽고/쓴다(core PEER_WEIGHT).
internal interface IZLinkBackendWeightedSocket : IZLinkBackendSocket
{
    void SetPeerWeight(int weight);

    int GetPeerWeight();
}

internal interface IZLinkBackendDealerSocket : IZLinkBackendConnectableSocket, IZLinkBackendWeightedSocket,
    IZLinkBackendSocketOptions
{
    IZLinkBackendSocketPoller CreateReceivePoller() =>
        throw new NotSupportedException(
            "The backend dealer socket does not provide a receive poller.");

    void SetRoutingId(RoutingId routingId);

    void SetProbe(bool enabled);

    void OnSendReady(Action handler);

    bool Send(Message message, SendFlags flags);

    bool Send(IReadOnlyList<Message> parts, SendFlags flags);

    bool Request(
        Message message,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout);

    bool Request(
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout);

    Task<IReadOnlyList<Message>> RequestAsync(
        Message message,
        TimeSpan timeout,
        CancellationToken cancellationToken);

    Received? Recv(RecvFlags flags = RecvFlags.None);

    bool Reply(
        Received received,
        Message message);
}

internal interface IZLinkBackendRouterSocket : IZLinkBackendConnectableSocket, IZLinkBackendWeightedSocket,
    IZLinkBackendSocketOptions
{
    IZLinkBackendSocketPoller CreateReceivePoller() =>
        throw new NotSupportedException(
            "The backend router socket does not provide a receive poller.");

    string GetLastEndpoint() => string.Empty;

    void OnSendReady(Action handler);

    void SetRoutingId(RoutingId routingId);

    /// <summary>Assigns the routing id of the peer the next outbound
    /// connect reaches, so rid-addressed sends work toward dialed peers.</summary>
    void SetConnectRoutingId(RoutingId routingId);

    void SetProbe(bool enabled);

    void SetMandatory(bool mandatory);

    void SetHandover(bool enabled);

    void DisconnectPeer(RoutingId routingId);

    Received? Recv(RecvFlags flags = RecvFlags.None);

    bool Send(
        RoutingId routingId,
        Message message,
        SendFlags flags);

    bool Send(
        RoutingId routingId,
        IReadOnlyList<Message> parts,
        SendFlags flags);

    bool Request(
        RoutingId routingId,
        Message message,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout);

    bool Request(
        RoutingId routingId,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout);

    void Reply(
        RoutingId routingId,
        ulong requestSeq,
        Message message);

    void Reply(
        RoutingId routingId,
        ulong requestSeq,
        IReadOnlyList<Message> parts);
}

internal interface IZLinkBackendPublisherSocket : IZLinkBackendSocket, IZLinkBackendSocketOptions
{
    string GetLastEndpoint() => string.Empty;

    void OnSendReady(Action handler);

    bool Publish(
        string topic,
        Message message,
        SendFlags flags);

    bool Publish(
        string topic,
        IReadOnlyList<Message> parts,
        SendFlags flags);
}

internal interface IZLinkBackendSubscriberSocket : IZLinkBackendConnectableSocket, IZLinkBackendSocketOptions
{
    IZLinkBackendSocketPoller CreateReceivePoller() =>
        throw new NotSupportedException(
            "The backend subscriber socket does not provide a receive poller.");

    void SetRoutingId(RoutingId routingId);

    void SetSubscription(string topic);

    bool Subscribe(TopicMessage result, RecvFlags flags = RecvFlags.None);
}

internal interface IZLinkBackendStreamSocket : IZLinkBackendSocket
{
    IZLinkBackendSocketPoller CreateReceivePoller() =>
        throw new NotSupportedException(
            "The backend stream socket does not provide a receive poller.");

    void ApplySocketConfig(IZLinkSocketConfig config) { }

    string GetLastEndpoint() => string.Empty;

    void SetTlsServer(string certPath, string keyPath, bool requireClientCert);

    void OnSendReady(Action handler);

    bool RecvPart(
        out RoutingId? sourceRoutingId,
        out Message? part,
        out bool hasMore,
        RecvFlags flags = RecvFlags.None);

    bool Send(
        RoutingId routingId,
        Message payload,
        SendFlags flags);

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

internal interface IZLinkBackendSocketMonitor : IAsyncDisposable
{
    bool Wait(TimeSpan timeout) =>
        throw new NotSupportedException(
            "The backend socket monitor does not provide a poll wait.");

    void OnEvent(Action<ZLinkBackendSocketMonitorEvent> handler);

    bool TryRecv(out ZLinkBackendSocketMonitorEvent monitorEvent);
}
