namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

internal sealed class ZLinkBackendRouterSocketWrapper(IRouterSocket nativeSocket) : IZLinkBackendRouterSocket
{
    public string GetLastEndpoint() => nativeSocket.Options.LastEndpoint;

    public void ApplySocketConfig(IZLinkSocketConfig config) =>
        ZLinkBackendSocketOptionsMapper.Apply(nativeSocket.Options, config);
    private readonly object _gate = new();

    internal IRouterSocket NativeSocket => nativeSocket;

    public IZLinkBackendSocketPoller CreateReceivePoller() =>
        ZLinkBackendSocketPoller.Create(nativeSocket);

    public void Bind(string endpoint)
    {
        nativeSocket.Bind(endpoint);
    }

    public void SetChannelName(string channelName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(channelName);
    }

    public void SetMaxMessageSize(long value)
    {
        nativeSocket.Options.MaxMessageSize = value;
    }

    public void SetSendHighWaterMark(ulong value)
    {
        nativeSocket.Options.SendHighWaterMark = value;
    }

    public void SetReceiveHighWaterMark(ulong value)
    {
        nativeSocket.Options.ReceiveHighWaterMark = value;
    }

    public void Connect(string endpoint)
    {
        nativeSocket.Connect(endpoint);
    }

    public void Disconnect(string endpoint)
    {
        nativeSocket.Disconnect(endpoint);
    }

    public void OnSendReady(Action handler)
    {
        nativeSocket.OnSendReady(handler);
    }

    public void SetRoutingId(RoutingId routingId)
    {
        nativeSocket.SetRoutingId(routingId);
    }

    public void SetConnectRoutingId(RoutingId routingId)
    {
        nativeSocket.Options.SetConnectRoutingId(routingId);
    }

    public void SetProbe(bool enabled)
    {
        nativeSocket.Options.Probe = enabled;
    }

    public void SetMandatory(bool mandatory)
    {
        nativeSocket.Options.Mandatory = mandatory;
    }

    public void SetHandover(bool enabled)
    {
        nativeSocket.Options.Handover = enabled;
    }

    public void DisconnectPeer(RoutingId routingId)
    {
        nativeSocket.DisconnectRid(routingId);
    }

    public void SetPeerWeight(int weight)
    {
        nativeSocket.Options.PeerWeight = weight;
    }

    public int GetPeerWeight()
    {
        return nativeSocket.Options.PeerWeight;
    }

    public Received? Recv(RecvFlags flags = RecvFlags.None)
    {
        var result = Received.Create();
        lock (_gate)
        {
            if (nativeSocket.Recv(result, flags))
                return result;
        }

        result.Dispose();
        return null;
    }

    public bool Send(
        RoutingId routingId,
        Message message,
        SendFlags flags)
    {
        lock (_gate)
        {
            return nativeSocket.Send(routingId)
                .Message(message)
                .Flags(flags)
                .Submit();
        }
    }

    public bool Send(
        RoutingId routingId,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        lock (_gate)
        {
            return nativeSocket.Send(routingId)
                .Messages(parts)
                .Flags(flags)
                .Submit();
        }
    }

    public bool Request(
        RoutingId routingId,
        Message message,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout)
    {
        var operation = nativeSocket.Request(routingId)
            .Message(message)
            .Flags(flags);
        if (timeout is { } value) operation = operation.Timeout(value);

        lock (_gate)
        {
            return operation.Submit(callback);
        }
    }

    public bool Request(
        RoutingId routingId,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout)
    {
        var operation = nativeSocket.Request(routingId).Messages(parts);

        if (timeout is { } value) operation = operation.Timeout(value);

        lock (_gate)
        {
            return operation.Flags(flags).Submit(callback);
        }
    }

    public void Reply(
        RoutingId routingId,
        ulong requestSeq,
        Message message)
    {
        lock (_gate)
        {
            nativeSocket.Reply(routingId, requestSeq)
                .Message(message)
                .Submit();
        }
    }

    public void Reply(
        RoutingId routingId,
        ulong requestSeq,
        IReadOnlyList<Message> parts)
    {
        lock (_gate)
        {
            nativeSocket.Reply(routingId, requestSeq)
                .Messages(parts)
                .Submit();
        }
    }

    public ValueTask DisposeAsync()
    {
        lock (_gate)
        {
            return nativeSocket.DisposeAsync();
        }
    }
}
