// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Marker interface for zlink endpoints that can participate in poll/proxy APIs.
/// </summary>
public interface IZlinkSocket
{
}

/// <summary>
///     Base contract shared by every socket type: lifetime, binding, TLS, and
///     monitoring.
/// </summary>
public interface ISocket : IZlinkSocket, IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Gets the typed options facade common to all socket types.
    /// </summary>
    CommonSocketOptions Options { get; }

    /// <summary>
    ///     Binds the socket to a local transport address (for example
    ///     <c>tcp://*:5555</c>, <c>ipc:///tmp/s</c>, or <c>inproc://name</c>) to
    ///     start accepting connections there.
    /// </summary>
    void Bind(string address);

    /// <summary>
    ///     Stops accepting connections at <paramref name="address" />, a previously
    ///     bound address.
    /// </summary>
    void Unbind(string address);

    /// <summary>
    ///     Opens a monitor that reports the selected socket lifecycle events. The
    ///     caller owns the returned monitor and must dispose it.
    /// </summary>
    ISocketMonitor MonitorOpen(SocketEvent events = SocketEvent.All);

    /// <summary>
    ///     Configures this socket as a TLS server. Apply before binding.
    /// </summary>
    /// <param name="certPath">Path to the server certificate (PEM).</param>
    /// <param name="keyPath">Path to the certificate's private key (PEM).</param>
    /// <param name="requireClientCert">
    ///     When true, require and verify a client certificate (mutual TLS).
    /// </param>
    void SetTlsServer(string certPath, string keyPath,
        bool requireClientCert = false);

    /// <summary>
    ///     Configures this socket as a TLS client. Apply before connecting.
    /// </summary>
    /// <param name="caCertPath">
    ///     Path to the CA bundle (PEM) used to verify the server certificate.
    /// </param>
    /// <param name="hostname">Expected server hostname to verify against.</param>
    /// <param name="trustSystem">
    ///     When true, also trust the host system's CA store in addition to
    ///     <paramref name="caCertPath" />.
    /// </param>
    void SetTlsClient(string caCertPath, string hostname,
        bool trustSystem = false);

    /// <summary>
    ///     Closes the socket and releases its native resources; further operations
    ///     throw. Unlike <see cref="IDisposable.Dispose" />, this closes the native
    ///     socket immediately.
    /// </summary>
    void Close();
}

/// <summary>
///     A socket that can also initiate outbound connections, not just bind.
/// </summary>
public interface IConnectableSocket : ISocket
{
    /// <summary>
    ///     Connects to a remote transport address (for example
    ///     <c>tcp://host:5555</c>). Connection is asynchronous; this returns before
    ///     the peer is reachable.
    /// </summary>
    void Connect(string address);

    /// <summary>
    ///     Disconnects the connection previously established to
    ///     <paramref name="address" />.
    /// </summary>
    void Disconnect(string address);

    /// <summary>
    ///     Disconnects the peer identified by <paramref name="peerRid" /> rather than
    ///     by address.
    /// </summary>
    void DisconnectRid(RoutingId peerRid);
}
