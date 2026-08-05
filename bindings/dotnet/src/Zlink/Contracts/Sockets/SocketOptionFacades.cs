// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Typed facade over the socket options shared by every socket type.
/// </summary>
public partial class CommonSocketOptions
{
    /// <summary>
    ///     Gets or sets the maximum inbound message size in bytes; -1 means no
    ///     limit.
    /// </summary>
    public long MaxMessageSize
    {
        get => Socket.GetOption(SocketOptions.MaxMsgSize);
        set => Socket.SetOption(SocketOptions.MaxMsgSize, value);
    }

    /// <summary>
    ///     Gets or sets the accounted byte limit for outbound messages queued
    ///     before the socket applies back-pressure; 0 means no limit.
    /// </summary>
    public ulong SendHighWaterMark
    {
        get => Socket.GetOption(SocketOptions.SndHwm);
        set => Socket.SetOption(SocketOptions.SndHwm, value);
    }

    /// <summary>
    ///     Gets or sets the accounted byte limit for inbound messages queued
    ///     before the socket applies back-pressure; 0 means no limit.
    /// </summary>
    public ulong ReceiveHighWaterMark
    {
        get => Socket.GetOption(SocketOptions.RcvHwm);
        set => Socket.SetOption(SocketOptions.RcvHwm, value);
    }

    /// <summary>
    ///     Gets or sets the underlying OS send buffer size in bytes; -1 keeps the
    ///     OS default.
    /// </summary>
    public int SendBufferSize
    {
        get => Socket.GetOption(SocketOptions.SndBuf);
        set => Socket.SetOption(SocketOptions.SndBuf, value);
    }

    /// <summary>
    ///     Gets or sets the underlying OS receive buffer size in bytes; -1 keeps the
    ///     OS default.
    /// </summary>
    public int ReceiveBufferSize
    {
        get => Socket.GetOption(SocketOptions.RcvBuf);
        set => Socket.SetOption(SocketOptions.RcvBuf, value);
    }

    /// <summary>
    ///     Gets or sets how long close waits to deliver still-queued messages;
    ///     null waits indefinitely.
    /// </summary>
    public TimeSpan? Linger
    {
        get => DecodeDuration(Socket.GetOption(SocketOptions.Linger));
        set => Socket.SetOption(SocketOptions.Linger,
            EncodeDuration(value, nameof(value)));
    }

    /// <summary>
    ///     Gets or sets the base delay before reconnecting a broken connection;
    ///     null disables reconnection.
    /// </summary>
    public TimeSpan? ReconnectInterval
    {
        get => DecodeDuration(Socket.GetOption(SocketOptions.ReconnectIvl));
        set => Socket.SetOption(SocketOptions.ReconnectIvl,
            EncodeDuration(value, nameof(value)));
    }

    /// <summary>
    ///     Gets or sets the maximum delay between reconnection attempts when the
    ///     reconnect interval backs off exponentially; null leaves it uncapped.
    /// </summary>
    public TimeSpan? ReconnectIntervalMax
    {
        get => DecodeDuration(Socket.GetOption(SocketOptions.ReconnectIvlMax));
        set => Socket.SetOption(SocketOptions.ReconnectIvlMax,
            EncodeDuration(value, nameof(value)));
    }

    /// <summary>
    ///     Gets or sets the maximum length of the queue of pending inbound
    ///     connections on a bound endpoint.
    /// </summary>
    public int Backlog
    {
        get => Socket.GetOption(SocketOptions.Backlog);
        set => Socket.SetOption(SocketOptions.Backlog, value);
    }

    /// <summary>
    ///     Gets or sets how long a blocking receive waits for a message before
    ///     failing; null blocks indefinitely.
    /// </summary>
    public TimeSpan? ReceiveTimeout
    {
        get => DecodeDuration(Socket.GetOption(SocketOptions.RcvTimeo));
        set => Socket.SetOption(SocketOptions.RcvTimeo,
            EncodeDuration(value, nameof(value)));
    }

    /// <summary>
    ///     Gets or sets how long a blocking send waits to enqueue a message before
    ///     failing; null blocks indefinitely.
    /// </summary>
    public TimeSpan? SendTimeout
    {
        get => DecodeDuration(Socket.GetOption(SocketOptions.SndTimeo));
        set => Socket.SetOption(SocketOptions.SndTimeo,
            EncodeDuration(value, nameof(value)));
    }

    /// <summary>
    ///     Gets or sets the time limit for a single connection attempt; null uses
    ///     the OS default.
    /// </summary>
    public TimeSpan? ConnectTimeout
    {
        get => DecodeDuration(Socket.GetOption(SocketOptions.ConnectTimeout));
        set => Socket.SetOption(SocketOptions.ConnectTimeout,
            EncodeDuration(value, nameof(value)));
    }

    /// <summary>
    ///     Gets or sets the maximum time allowed for protocol handshaking; null
    ///     keeps the native default.
    /// </summary>
    public TimeSpan? HandshakeInterval
    {
        get => DecodeDuration(Socket.GetOption(SocketOptions.HandshakeIvl));
        set => Socket.SetOption(SocketOptions.HandshakeIvl,
            EncodeDuration(value, nameof(value)));
    }

    /// <summary>
    ///     Gets or sets TCP keep-alive: -1 uses the OS default, 0 disables it, 1
    ///     enables it.
    /// </summary>
    public int TcpKeepAlive
    {
        get => Socket.GetOption(SocketOptions.TcpKeepalive);
        set => Socket.SetOption(SocketOptions.TcpKeepalive, value);
    }

    /// <summary>
    ///     Gets or sets whether IPv6 connections are enabled.
    /// </summary>
    public bool IPv6
    {
        get => Socket.GetOption(SocketOptions.Ipv6) != 0;
        set => Socket.SetOption(SocketOptions.Ipv6, value ? 1 : 0);
    }

    /// <summary>
    ///     Gets or sets whether Nagle's algorithm is disabled (TCP_NODELAY) so
    ///     small messages are sent without buffering delay.
    /// </summary>
    public bool TcpNoDelay
    {
        get => Socket.GetOption(SocketOptions.TcpNoDelay) != 0;
        set => Socket.SetOption(SocketOptions.TcpNoDelay, value ? 1 : 0);
    }

    /// <summary>
    ///     Gets or sets whether messages are queued only to fully established
    ///     connections; when false they may also queue to pending connections.
    /// </summary>
    public bool Immediate
    {
        get => Socket.GetOption(SocketOptions.Immediate) != 0;
        set => Socket.SetOption(SocketOptions.Immediate, value ? 1 : 0);
    }

    /// <summary>
    ///     Gets or sets whether a submit that hits a local failure (such as
    ///     back-pressure) is retried; see <see cref="Systems.Zlink.SubmitRetryMode" />.
    /// </summary>
    public SubmitRetryMode SubmitRetryMode
    {
        get => (SubmitRetryMode)Socket.GetOption(SocketOptions.SubmitRetryMode);
        set => Socket.SetOption(SocketOptions.SubmitRetryMode, (int)value);
    }

    /// <summary>
    ///     Gets or sets the total time budget, in milliseconds, for submit retries
    ///     when <see cref="SubmitRetryMode" /> is enabled.
    /// </summary>
    public int SubmitRetryTimeoutMilliseconds
    {
        get => Socket.GetOption(SocketOptions.SubmitRetryTimeout);
        set => Socket.SetOption(SocketOptions.SubmitRetryTimeout, value);
    }

    /// <summary>
    ///     Gets or sets the maximum number of submit retry attempts when
    ///     <see cref="SubmitRetryMode" /> is enabled.
    /// </summary>
    public int SubmitRetryAttempts
    {
        get => Socket.GetOption(SocketOptions.SubmitRetryAttempts);
        set => Socket.SetOption(SocketOptions.SubmitRetryAttempts, value);
    }

    /// <summary>
    ///     Gets or sets how the socket reacts when a connecting peer presents a
    ///     routing id already in use; see <see cref="RidDuplicatePolicy" />.
    /// </summary>
    public RidDuplicatePolicy RoutingIdDuplicatePolicy
    {
        get => (RidDuplicatePolicy)Socket.GetOption(
            SocketOptions.RidDuplicatePolicy);
        set => Socket.SetOption(SocketOptions.RidDuplicatePolicy, (int)value);
    }

    /// <summary>
    ///     Gets the concrete endpoint the socket last bound to, for example the
    ///     resolved address and port after binding to a wildcard.
    /// </summary>
    public string LastEndpoint => Socket.GetOption(SocketOptions.LastEndpoint);
}
