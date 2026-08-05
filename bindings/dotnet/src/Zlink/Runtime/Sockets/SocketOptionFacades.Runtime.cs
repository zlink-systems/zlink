// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

public partial class CommonSocketOptions
{
    private protected readonly ISocketOptionEndpoint Socket;

    internal CommonSocketOptions(ISocketOptionEndpoint socket)
    {
        Socket = socket;
    }

    internal ulong Affinity
    {
        get => Socket.GetOption(SocketOptions.Affinity);
        set => Socket.SetOption(SocketOptions.Affinity, value);
    }

    internal int Rate
    {
        get => Socket.GetOption(SocketOptions.Rate);
        set => Socket.SetOption(SocketOptions.Rate, value);
    }

    internal TimeSpan? RecoveryInterval
    {
        get => DecodeDuration(Socket.GetOption(SocketOptions.RecoveryIvl));
        set => Socket.SetOption(SocketOptions.RecoveryIvl,
            EncodeDuration(value, nameof(value)));
    }

    internal int MulticastHops
    {
        get => Socket.GetOption(SocketOptions.MulticastHops);
        set => Socket.SetOption(SocketOptions.MulticastHops, value);
    }

    internal int TcpKeepAliveCount
    {
        get => Socket.GetOption(SocketOptions.TcpKeepaliveCnt);
        set => Socket.SetOption(SocketOptions.TcpKeepaliveCnt, value);
    }

    internal int TcpKeepAliveIdleSeconds
    {
        get => Socket.GetOption(SocketOptions.TcpKeepaliveIdle);
        set => Socket.SetOption(SocketOptions.TcpKeepaliveIdle, value);
    }

    internal int TcpKeepAliveIntervalSeconds
    {
        get => Socket.GetOption(SocketOptions.TcpKeepaliveIntvl);
        set => Socket.SetOption(SocketOptions.TcpKeepaliveIntvl, value);
    }

    internal int TcpMaxRetransmitTimeout
    {
        get => Socket.GetOption(SocketOptions.TcpMaxRt);
        set => Socket.SetOption(SocketOptions.TcpMaxRt, value);
    }

    internal int TypeOfService
    {
        get => Socket.GetOption(SocketOptions.Tos);
        set => Socket.SetOption(SocketOptions.Tos, value);
    }

    internal int MulticastMaxTransportDataUnit
    {
        get => Socket.GetOption(SocketOptions.MulticastMaxTpdu);
        set => Socket.SetOption(SocketOptions.MulticastMaxTpdu, value);
    }

    internal string BindToDevice
    {
        get => Socket.GetOption(SocketOptions.BindToDevice);
        set => Socket.SetOption(SocketOptions.BindToDevice, value);
    }

    internal bool Conflate
    {
        get => Socket.GetOption(SocketOptions.Conflate) != 0;
        set => Socket.SetOption(SocketOptions.Conflate, value ? 1 : 0);
    }

    internal bool Blocky
    {
        get => Socket.GetOption(SocketOptions.Blocky) != 0;
        set => Socket.SetOption(SocketOptions.Blocky, value ? 1 : 0);
    }

    internal bool InvertMatching
    {
        get => Socket.GetOption(SocketOptions.InvertMatching) != 0;
        set => Socket.SetOption(SocketOptions.InvertMatching, value ? 1 : 0);
    }

    internal bool ZmpMetadata
    {
        get => Socket.GetOption(SocketOptions.ZmpMetadata) != 0;
        set => Socket.SetOption(SocketOptions.ZmpMetadata, value ? 1 : 0);
    }

    internal string TlsCertificatePath
    {
        get => Socket.GetOption(SocketOptions.TlsCert);
        set => Socket.SetOption(SocketOptions.TlsCert, value);
    }

    internal string TlsKeyPath
    {
        get => Socket.GetOption(SocketOptions.TlsKey);
        set => Socket.SetOption(SocketOptions.TlsKey, value);
    }

    internal string TlsCaCertificatePath
    {
        get => Socket.GetOption(SocketOptions.TlsCa);
        set => Socket.SetOption(SocketOptions.TlsCa, value);
    }

    internal bool TlsVerify
    {
        get => Socket.GetOption(SocketOptions.TlsVerify) != 0;
        set => Socket.SetOption(SocketOptions.TlsVerify, value ? 1 : 0);
    }

    internal bool TlsRequireClientCertificate
    {
        get => Socket.GetOption(SocketOptions.TlsRequireClientCert) != 0;
        set => Socket.SetOption(SocketOptions.TlsRequireClientCert,
            value ? 1 : 0);
    }

    internal string TlsHostname
    {
        get => Socket.GetOption(SocketOptions.TlsHostname);
        set => Socket.SetOption(SocketOptions.TlsHostname, value);
    }

    internal bool TlsTrustSystem
    {
        get => Socket.GetOption(SocketOptions.TlsTrustSystem) != 0;
        set => Socket.SetOption(SocketOptions.TlsTrustSystem, value ? 1 : 0);
    }

    internal string TlsPassword
    {
        get => Socket.GetOption(SocketOptions.TlsPassword);
        set => Socket.SetOption(SocketOptions.TlsPassword, value);
    }

    internal int FileDescriptor => Socket.GetOption(SocketOptions.Fd);

    internal SocketType SocketType => Socket.SocketType;

    internal PollEventFlags Events => (PollEventFlags)Socket.GetOption(SocketOptions.Events);

    internal static TimeSpan? DecodeDuration(int millis)
    {
        return millis < 0 ? null : TimeSpan.FromMilliseconds(millis);
    }

    internal static int EncodeDuration(TimeSpan? duration, string paramName)
    {
        if (duration is null)
            return -1;

        var millis = duration.Value.TotalMilliseconds;
        if (double.IsNaN(millis) || double.IsInfinity(millis)
                                 || millis < 0 || millis > int.MaxValue)
            throw new ArgumentOutOfRangeException(paramName);

        return (int)Math.Ceiling(millis);
    }

    internal static int EncodePeerWeight(int value, string paramName)
    {
        if (value < 0 || value > 100)
            throw new ArgumentOutOfRangeException(paramName);
        return value;
    }
}

internal interface ISocketOptionEndpoint
{
    SocketType SocketType { get; }
    void SetOption(SocketOptionKey<int> option, int value);
    void SetOption(SocketOptionKey<long> option, long value);
    void SetOption(SocketOptionKey<ulong> option, ulong value);
    void SetOption(SocketOptionKey<byte[]> option, byte[] value);
    void SetOption(SocketOptionKey<byte[]> option, ReadOnlySpan<byte> value);
    void SetOption(SocketOptionKey<string> option, string value);
    int GetOption(SocketOptionKey<int> option);
    long GetOption(SocketOptionKey<long> option);
    ulong GetOption(SocketOptionKey<ulong> option);
    byte[] GetOption(SocketOptionKey<byte[]> option, int initialSize = 256);
    int GetOption(SocketOptionKey<byte[]> option, Span<byte> destination);
    string GetOption(SocketOptionKey<string> option, int initialSize = 256);
}
