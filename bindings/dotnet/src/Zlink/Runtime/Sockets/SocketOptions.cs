// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal readonly struct SocketOptionKey<T>
{
    internal SocketOptionKey(SocketOption option)
    {
        Option = option;
    }

    public SocketOption Option { get; }

    public override string ToString()
    {
        return Option.ToString();
    }
}

internal static class SocketOptions
{
    public static SocketOptionKey<ulong> Affinity { get; } =
        UInt64(SocketOption.Affinity);

    public static SocketOptionKey<byte[]> RoutingId { get; } =
        Bytes(SocketOption.RoutingId);

    public static SocketOptionKey<int> Rate { get; } = Int(SocketOption.Rate);

    public static SocketOptionKey<int> RecoveryIvl { get; } =
        Int(SocketOption.RecoveryIvl);

    public static SocketOptionKey<int> SndBuf { get; } = Int(SocketOption.SndBuf);
    public static SocketOptionKey<int> RcvBuf { get; } = Int(SocketOption.RcvBuf);
    public static SocketOptionKey<int> RcvMore { get; } = Int(SocketOption.RcvMore);
    public static SocketOptionKey<int> Fd { get; } = Int(SocketOption.Fd);
    public static SocketOptionKey<int> Events { get; } = Int(SocketOption.Events);
    public static SocketOptionKey<int> Type { get; } = Int(SocketOption.Type);
    public static SocketOptionKey<int> Linger { get; } = Int(SocketOption.Linger);

    public static SocketOptionKey<int> ReconnectIvl { get; } =
        Int(SocketOption.ReconnectIvl);

    public static SocketOptionKey<int> Backlog { get; } =
        Int(SocketOption.Backlog);

    public static SocketOptionKey<int> ReconnectIvlMax { get; } =
        Int(SocketOption.ReconnectIvlMax);

    public static SocketOptionKey<long> MaxMsgSize { get; } =
        Int64(SocketOption.MaxMsgSize);

    public static SocketOptionKey<ulong> SndHwm { get; } =
        UInt64(SocketOption.SndHwm);
    public static SocketOptionKey<ulong> RcvHwm { get; } =
        UInt64(SocketOption.RcvHwm);

    public static SocketOptionKey<int> MulticastHops { get; } =
        Int(SocketOption.MulticastHops);

    public static SocketOptionKey<int> RcvTimeo { get; } =
        Int(SocketOption.RcvTimeo);

    public static SocketOptionKey<int> SndTimeo { get; } =
        Int(SocketOption.SndTimeo);

    public static SocketOptionKey<string> LastEndpoint { get; } =
        String(SocketOption.LastEndpoint);

    public static SocketOptionKey<int> RouterMandatory { get; } =
        Int(SocketOption.RouterMandatory);

    public static SocketOptionKey<int> TcpKeepalive { get; } =
        Int(SocketOption.TcpKeepalive);

    public static SocketOptionKey<int> TcpKeepaliveCnt { get; } =
        Int(SocketOption.TcpKeepaliveCnt);

    public static SocketOptionKey<int> TcpKeepaliveIdle { get; } =
        Int(SocketOption.TcpKeepaliveIdle);

    public static SocketOptionKey<int> TcpKeepaliveIntvl { get; } =
        Int(SocketOption.TcpKeepaliveIntvl);

    public static SocketOptionKey<int> TcpNoDelay { get; } =
        Int(SocketOption.TcpNoDelay);

    public static SocketOptionKey<int> Immediate { get; } =
        Int(SocketOption.Immediate);

    public static SocketOptionKey<int> RidDuplicatePolicy { get; } =
        Int(SocketOption.RidDuplicatePolicy);

    public static SocketOptionKey<int> RouteValueMaxSize { get; } =
        Int(SocketOption.RouteValueMaxSize);

    public static SocketOptionKey<int> SubmitRetryMode { get; } =
        Int(SocketOption.SubmitRetryMode);

    public static SocketOptionKey<int> SubmitRetryTimeout { get; } =
        Int(SocketOption.SubmitRetryTimeout);

    public static SocketOptionKey<int> SubmitRetryAttempts { get; } =
        Int(SocketOption.SubmitRetryAttempts);

    public static SocketOptionKey<int> XPubVerbose { get; } =
        Int(SocketOption.XPubVerbose);

    public static SocketOptionKey<int> Ipv6 { get; } = Int(SocketOption.Ipv6);

    public static SocketOptionKey<int> ProbeRouter { get; } =
        Int(SocketOption.ProbeRouter);

    public static SocketOptionKey<int> Conflate { get; } =
        Int(SocketOption.Conflate);

    public static SocketOptionKey<int> Tos { get; } = Int(SocketOption.Tos);

    public static SocketOptionKey<byte[]> ConnectRoutingId { get; } =
        Bytes(SocketOption.ConnectRoutingId);

    public static SocketOptionKey<int> RouterRequestTimeout { get; } =
        Int(SocketOption.RouterRequestTimeout);

    public static SocketOptionKey<int> RouterWeight { get; } =
        Int(SocketOption.RouterWeight);

    public static SocketOptionKey<int> DealerRequestTimeout { get; } =
        Int(SocketOption.DealerRequestTimeout);

    public static SocketOptionKey<int> DealerWeight { get; } =
        Int(SocketOption.DealerWeight);

    public static SocketOptionKey<int> HandshakeIvl { get; } =
        Int(SocketOption.HandshakeIvl);

    public static SocketOptionKey<int> XPubNoDrop { get; } =
        Int(SocketOption.XPubNoDrop);

    public static SocketOptionKey<int> Blocky { get; } = Int(SocketOption.Blocky);

    public static SocketOptionKey<int> XPubManual { get; } =
        Int(SocketOption.XPubManual);

    public static SocketOptionKey<byte[]> XPubWelcomeMsg { get; } =
        Bytes(SocketOption.XPubWelcomeMsg);

    public static SocketOptionKey<int> StreamNotify { get; } =
        Int(SocketOption.StreamNotify);

    public static SocketOptionKey<int> InvertMatching { get; } =
        Int(SocketOption.InvertMatching);

    public static SocketOptionKey<int> XPubVerboser { get; } =
        Int(SocketOption.XPubVerboser);

    public static SocketOptionKey<int> ConnectTimeout { get; } =
        Int(SocketOption.ConnectTimeout);

    public static SocketOptionKey<int> TcpMaxRt { get; } = Int(SocketOption.TcpMaxRt);

    public static SocketOptionKey<int> MulticastMaxTpdu { get; } =
        Int(SocketOption.MulticastMaxTpdu);

    public static SocketOptionKey<int> UseFd { get; } = Int(SocketOption.UseFd);

    public static SocketOptionKey<string> BindToDevice { get; } =
        String(SocketOption.BindToDevice);

    public static SocketOptionKey<string> TlsCert { get; } =
        String(SocketOption.TlsCert);

    public static SocketOptionKey<string> TlsKey { get; } =
        String(SocketOption.TlsKey);

    public static SocketOptionKey<string> TlsCa { get; } =
        String(SocketOption.TlsCa);

    public static SocketOptionKey<int> TlsVerify { get; } =
        Int(SocketOption.TlsVerify);

    public static SocketOptionKey<int> TlsRequireClientCert { get; } =
        Int(SocketOption.TlsRequireClientCert);

    public static SocketOptionKey<string> TlsHostname { get; } =
        String(SocketOption.TlsHostname);

    public static SocketOptionKey<int> TlsTrustSystem { get; } =
        Int(SocketOption.TlsTrustSystem);

    public static SocketOptionKey<string> TlsPassword { get; } =
        String(SocketOption.TlsPassword);

    public static SocketOptionKey<int> XPubManualLastValue { get; } =
        Int(SocketOption.XPubManualLastValue);

    public static SocketOptionKey<int> TopicsCount { get; } =
        Int(SocketOption.TopicsCount);

    public static SocketOptionKey<string> XPubApproveSubscribe { get; } =
        String(SocketOption.XPubApproveSubscribe);

    public static SocketOptionKey<string> XPubRejectSubscribe { get; } =
        String(SocketOption.XPubRejectSubscribe);

    public static SocketOptionKey<int> SubTopicsCount { get; } =
        Int(SocketOption.SubTopicsCount);

    public static SocketOptionKey<int> ZmpMetadata { get; } =
        Int(SocketOption.ZmpMetadata);

    private static SocketOptionKey<int> Int(SocketOption option)
    {
        return new SocketOptionKey<int>(option);
    }

    private static SocketOptionKey<long> Int64(SocketOption option)
    {
        return new SocketOptionKey<long>(option);
    }

    private static SocketOptionKey<ulong> UInt64(SocketOption option)
    {
        return new SocketOptionKey<ulong>(option);
    }

    private static SocketOptionKey<byte[]> Bytes(SocketOption option)
    {
        return new SocketOptionKey<byte[]>(option);
    }

    private static SocketOptionKey<string> String(SocketOption option)
    {
        return new SocketOptionKey<string>(option);
    }
}
