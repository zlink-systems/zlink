namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

internal sealed class ZLinkBackendDealerSocketWrapper(IDealerSocket nativeSocket) : IZLinkBackendDealerSocket
{
    // The binding serializes multipart submission, but native close rejects an
    // in-flight public API with EBUSY. Keep one framework lifecycle gate so
    // DisposeAsync waits for send/request/recv instead of surfacing close failure.
    private readonly object _gate = new();

    internal IDealerSocket NativeSocket => nativeSocket;

    public IZLinkBackendSocketPoller CreateReceivePoller() =>
        ZLinkBackendSocketPoller.Create(nativeSocket, includeRequestCompletion: true);

    public void ApplySocketConfig(IZLinkSocketConfig config) =>
        ZLinkBackendSocketOptionsMapper.Apply(nativeSocket.Options, config);

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

    public void SetRoutingId(RoutingId routingId)
    {
        nativeSocket.SetRoutingId(routingId);
    }

    public void SetProbe(bool enabled)
    {
        nativeSocket.Options.Probe = enabled;
    }

    public void OnSendReady(Action handler)
    {
        nativeSocket.OnSendReady(handler);
    }

    public void SetPeerWeight(int weight)
    {
        nativeSocket.Options.PeerWeight = weight;
    }

    public int GetPeerWeight()
    {
        return nativeSocket.Options.PeerWeight;
    }

    public bool Send(Message message, SendFlags flags)
    {
        lock (_gate)
        {
            return nativeSocket.Send()
                .Message(message)
                .Flags(flags)
                .Submit();
        }
    }

    public bool Send(IReadOnlyList<Message> parts, SendFlags flags)
    {
        lock (_gate)
        {
            return nativeSocket.Send()
                .Messages(parts)
                .Flags(flags)
                .Submit();
        }
    }

    public bool Request(
        Message message,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout)
    {
        lock (_gate)
        {
            var operation = nativeSocket.Request()
                .Message(message)
                .Flags(flags);
            if (timeout is { } value) operation = operation.Timeout(value);

            return operation.Submit(callback);
        }
    }

    public bool Request(
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout)
    {
        lock (_gate)
        {
            var operation = nativeSocket.Request().Messages(parts);

            if (timeout is { } value) operation = operation.Timeout(value);

            return operation.Flags(flags).Submit(callback);
        }
    }

    public Task<IReadOnlyList<Message>> RequestAsync(
        Message message,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        lock (_gate)
        {
            return nativeSocket.Request()
                .Message(message)
                .Timeout(timeout)
                .Async(cancellationToken);
        }
    }

    public Received? Recv(RecvFlags flags = RecvFlags.None)
    {
        var received = Received.Create();
        lock (_gate)
        {
            if (nativeSocket.Recv(received, flags)) return received;
        }

        received.Dispose();
        return null;
    }

    public bool Reply(
        Received received,
        Message message)
    {
        lock (_gate)
        {
            return received.Send()
                .Message(message)
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
