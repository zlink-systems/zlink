namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

internal static class ZLinkBackendSocketOptionsMapper
{
    public static void Apply(CommonSocketOptions options, IZLinkSocketConfig config)
    {
        if (config.MaxMessageSize > 0)
            options.MaxMessageSize = config.MaxMessageSize;
        if (config.SendHighWaterMark > 0)
            options.SendHighWaterMark = config.SendHighWaterMark;
        if (config.ReceiveHighWaterMark > 0)
            options.ReceiveHighWaterMark = config.ReceiveHighWaterMark;
        if (config.SendBufferSize > 0)
            options.SendBufferSize = config.SendBufferSize;
        if (config.ReceiveBufferSize > 0)
            options.ReceiveBufferSize = config.ReceiveBufferSize;
        options.Linger = config.Linger;
        if (config.ReceiveTimeout is not null)
            options.ReceiveTimeout = config.ReceiveTimeout;
        if (config.SendTimeout is not null)
            options.SendTimeout = config.SendTimeout;
        if (config.ConnectTimeout is not null)
            options.ConnectTimeout = config.ConnectTimeout;
        if (config.HandshakeInterval is not null)
            options.HandshakeInterval = config.HandshakeInterval;
        options.IPv6 = config.IPv6;
        options.TcpNoDelay = config.TcpNoDelay;
        options.Immediate = config.Immediate;
    }
}
