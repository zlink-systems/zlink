using System.Net.Security;
using System.Net.Sockets;
using System.Net.WebSockets;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal static class ZlinkStreamTransportFactory
{
    public static void ValidateTransport(ZlinkStreamConnectorOptions options)
    {
        _ = ResolveTransport(options);
    }

    public static async ValueTask<IZlinkStreamConnection> ConnectAsync(
        ZlinkStreamConnectorOptions options,
        CancellationToken cancellationToken)
    {
        var transport = ResolveTransport(options);
        return transport is ZlinkStreamTransport.WebSocket or ZlinkStreamTransport.WebSocketSecure
            ? await ConnectWebSocketAsync(options, cancellationToken).ConfigureAwait(false)
            : await ConnectStreamAsync(options, transport, cancellationToken).ConfigureAwait(false);
    }

    private static async ValueTask<IZlinkStreamConnection> ConnectWebSocketAsync(
        ZlinkStreamConnectorOptions options,
        CancellationToken cancellationToken)
    {
        var webSocket = new ClientWebSocket();
        try
        {
            if (options.SkipServerCertificateValidation)
                webSocket.Options.RemoteCertificateValidationCallback = (_, _, _, _) => true;

            await webSocket.ConnectAsync(options.Endpoint, cancellationToken).ConfigureAwait(false);
            return new WebSocketConnection(webSocket, options.MaxReceivePayloadSize);
        }
        catch
        {
            webSocket.Dispose();
            throw;
        }
    }

    private static async ValueTask<IZlinkStreamConnection> ConnectStreamAsync(
        ZlinkStreamConnectorOptions options,
        ZlinkStreamTransport transport,
        CancellationToken cancellationToken)
    {
        var tcp = new TcpClient();
        try
        {
            tcp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
            await tcp.ConnectAsync(options.Endpoint.Host, options.Endpoint.Port, cancellationToken)
                .ConfigureAwait(false);
            System.IO.Stream stream = tcp.GetStream();
            if (transport == ZlinkStreamTransport.Tls)
            {
                var ssl = new SslStream(
                    stream,
                    false,
                    options.SkipServerCertificateValidation
                        ? (_, _, _, _) => true
                        : null);
                try
                {
                    await ssl.AuthenticateAsClientAsync(options.Endpoint.Host).WaitAsync(cancellationToken)
                        .ConfigureAwait(false);
                    stream = ssl;
                }
                catch
                {
                    await ssl.DisposeAsync().ConfigureAwait(false);
                    throw;
                }
            }

            return new StreamConnection(tcp, stream);
        }
        catch
        {
            tcp.Dispose();
            throw;
        }
    }

    private static ZlinkStreamTransport ResolveTransport(ZlinkStreamConnectorOptions options)
    {
        var inferred = options.Endpoint.Scheme.ToLowerInvariant() switch
        {
            "tcp" => ZlinkStreamTransport.Tcp,
            "tls" => ZlinkStreamTransport.Tls,
            "ws" => ZlinkStreamTransport.WebSocket,
            "wss" => ZlinkStreamTransport.WebSocketSecure,
            _ => throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ConfigurationError,
                "Endpoint scheme is not supported.")
        };

        if (options.Transport is { } configured && configured != inferred)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ConfigurationError,
                "Configured transport conflicts with endpoint scheme.");

        return inferred;
    }
}
