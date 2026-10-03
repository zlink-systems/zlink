using System.Net.Security;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal static class ZlinkStreamTransportFactory
{
    public static void ValidateTransport(ZlinkStreamConnectorOptions options)
    {
        _ = ResolveTransport(options);
    }

    public static async ValueTask<IZlinkStreamConnection> ConnectAsync(
        ZlinkStreamConnectorOptions options,
        CancellationToken cancellationToken
    )
    {
        var transport = ResolveTransport(options);
        return transport is ZlinkStreamTransport.WebSocket or ZlinkStreamTransport.WebSocketSecure
            ? await ConnectWebSocketAsync(options, cancellationToken).ConfigureAwait(false)
            : await ConnectStreamAsync(options, transport, cancellationToken).ConfigureAwait(false);
    }

    private static async ValueTask<IZlinkStreamConnection> ConnectWebSocketAsync(
        ZlinkStreamConnectorOptions options,
        CancellationToken cancellationToken
    )
    {
        var webSocket = new ClientWebSocket();
        try
        {
            webSocket.Options.RemoteCertificateValidationCallback = CertificateValidationCallback(
                options.SkipServerCertificateValidation
            );

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
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var tcp = new TcpClient();
        try
        {
            tcp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
            await tcp.ConnectAsync(options.Endpoint.Host, options.Endpoint.Port)
                .WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            System.IO.Stream stream = tcp.GetStream();
            if (transport == ZlinkStreamTransport.Tls)
            {
                var ssl = new SslStream(
                    stream,
                    false,
                    CertificateValidationCallback(options.SkipServerCertificateValidation)
                );
                try
                {
                    await ssl.AuthenticateAsClientAsync(options.Endpoint.Host)
                        .WaitAsync(cancellationToken)
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

    private const string TcpScheme = "tcp";
    private const string TlsScheme = "tls";

    private static RemoteCertificateValidationCallback CertificateValidationCallback(bool skip) =>
        skip ? static (_, _, _, _) => true : ValidateServerCertificate;

    private static bool ValidateServerCertificate(
        object sender,
        X509Certificate? certificate,
        X509Chain? chain,
        SslPolicyErrors errors
    )
    {
        if (errors != SslPolicyErrors.None)
            throw new CertificateValidationException(errors);
        return true;
    }

    internal sealed class CertificateValidationException(SslPolicyErrors errors)
        : AuthenticationException($"TLS server certificate validation failed ({errors}).");

    private static ZlinkStreamTransport ResolveTransport(ZlinkStreamConnectorOptions options)
    {
        var inferred = options.Endpoint.Scheme.ToLowerInvariant() switch
        {
            TcpScheme => ZlinkStreamTransport.Tcp,
            TlsScheme => ZlinkStreamTransport.Tls,
            var scheme when scheme == Uri.UriSchemeWs => ZlinkStreamTransport.WebSocket,
            var scheme when scheme == Uri.UriSchemeWss => ZlinkStreamTransport.WebSocketSecure,
            _ => throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ConfigurationError,
                "Endpoint scheme is not supported."
            ),
        };

        if (options.Transport is { } configured && configured != inferred)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ConfigurationError,
                "Configured transport conflicts with endpoint scheme."
            );

        return inferred;
    }
}
