namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

internal static class ZLinkBackendSocketOptionsMapper
{
    public static void Apply(CommonSocketOptions options, IZLinkSocketConfig config)
    {
        if (config.MaxMessageSize > 0)
            options.MaxMessageSize = config.MaxMessageSize;
        ApplyHighWaterMarks(options, config);
        if (config.SendBufferSize > 0)
            options.SendBufferSize = config.SendBufferSize;
        if (config.ReceiveBufferSize > 0)
            options.ReceiveBufferSize = config.ReceiveBufferSize;
        options.Linger = config.Linger;
        if (config.ReceiveTimeout is not null)
            options.ReceiveTimeout = config.ReceiveTimeout;
        if (config.ConnectTimeout is not null)
            options.ConnectTimeout = config.ConnectTimeout;
        if (config.HandshakeInterval is not null)
            options.HandshakeInterval = config.HandshakeInterval;
        if (config.IPv6 is { } ipv6)
            options.IPv6 = ipv6;
        if (config.TcpNoDelay is { } tcpNoDelay)
            options.TcpNoDelay = tcpNoDelay;
        if (config.Immediate is { } immediate)
            options.Immediate = immediate;
    }

    internal static void ApplyHighWaterMarks(CommonSocketOptions options, IZLinkSocketConfig config)
    {
        var send = config is ZLinkSocketConfig configuredSend
            ? configuredSend.ConfiguredSendHighWaterMark
            : config.SendHighWaterMark;
        var receive = config is ZLinkSocketConfig configuredReceive
            ? configuredReceive.ConfiguredReceiveHighWaterMark
            : config.ReceiveHighWaterMark;
        if (send is { } sendValue)
            options.SendHighWaterMark = sendValue;
        if (receive is { } receiveValue)
            options.ReceiveHighWaterMark = receiveValue;
    }
}
