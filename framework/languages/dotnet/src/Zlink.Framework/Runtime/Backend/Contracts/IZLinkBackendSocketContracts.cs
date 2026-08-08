namespace Zlink.Framework.Runtime.Backend.Contracts;

// STREAM keeps a semantic adapter because Framework coordinates actor binding,
// multipart receive state, and mesh completions through one lifecycle boundary.
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
