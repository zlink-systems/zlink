namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

internal sealed class ZLinkBackendPublisherSocketWrapper(IPubSocket nativeSocket) : IZLinkBackendPublisherSocket
{
    public string GetLastEndpoint() => nativeSocket.Options.LastEndpoint;

    public void ApplySocketConfig(IZLinkSocketConfig config) =>
        ZLinkBackendSocketOptionsMapper.Apply(nativeSocket.Options, config);
    internal IPubSocket NativeSocket => nativeSocket;

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

    public void OnSendReady(Action handler)
    {
        nativeSocket.OnSendReady(handler);
    }

    public bool Publish(
        string topic,
        Message message,
        SendFlags flags)
    {
        return nativeSocket.Publish(topic)
            .Message(message)
            .Flags(flags)
            .Submit();
    }

    public bool Publish(
        string topic,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        return nativeSocket.Publish(topic)
            .Messages(parts)
            .Flags(flags)
            .Submit();
    }

    public ValueTask DisposeAsync()
    {
        return nativeSocket.DisposeAsync();
    }
}
