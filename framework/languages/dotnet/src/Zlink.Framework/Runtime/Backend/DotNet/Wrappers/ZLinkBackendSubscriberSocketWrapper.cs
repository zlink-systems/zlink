namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

internal sealed class ZLinkBackendSubscriberSocketWrapper(ISubSocket nativeSocket) : IZLinkBackendSubscriberSocket
{
    public void ApplySocketConfig(IZLinkSocketConfig config) =>
        ZLinkBackendSocketOptionsMapper.Apply(nativeSocket.Options, config);
    internal ISubSocket NativeSocket => nativeSocket;

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

    public void SetRoutingId(RoutingId routingId)
    {
        nativeSocket.SetRoutingId(routingId);
    }

    public void SetSubscription(string topic)
    {
        nativeSocket.SetSubscription(topic);
    }

    public bool Subscribe(TopicMessage result, RecvFlags flags = RecvFlags.None)
    {
        return nativeSocket.Subscribe(result, flags);
    }

    public ValueTask DisposeAsync()
    {
        return nativeSocket.DisposeAsync();
    }
}
