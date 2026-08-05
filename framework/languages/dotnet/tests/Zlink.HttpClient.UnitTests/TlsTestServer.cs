/* SPDX-License-Identifier: Apache-2.0 */

using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Zlink.HttpClient.UnitTests;

/// <summary>Generates ephemeral self-signed certificates for TLS tests.</summary>
internal static class TestCertificates
{
    public static X509Certificate2 CreateSelfSigned(string commonName)
    {
        using var rsa = RSA.Create(2048);
        var request = new CertificateRequest(
            $"CN={commonName}", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
        var san = new SubjectAlternativeNameBuilder();
        san.AddIpAddress(IPAddress.Loopback);
        san.AddDnsName("localhost");
        request.CertificateExtensions.Add(san.Build());
        return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
    }

    /// <summary>Writes the certificate (PEM) and its private key (PKCS#8 PEM) to two temp files.</summary>
    public static (string CertPath, string KeyPath) WritePem(X509Certificate2 certificate)
    {
        var certPath = Path.GetTempFileName();
        var keyPath = Path.GetTempFileName();
        File.WriteAllText(certPath, certificate.ExportCertificatePem());
        using var rsa = certificate.GetRSAPrivateKey()!;
        File.WriteAllText(keyPath, rsa.ExportPkcs8PrivateKeyPem());
        return (certPath, keyPath);
    }
}

/// <summary>
///     Minimal HTTPS server over <see cref="SslStream" /> for TLS contract tests. The managed Linux
///     <see cref="HttpListener" /> cannot bind HTTPS, so this server drives the handshake directly,
///     optionally requiring a client certificate (mTLS), and records whether one was presented.
/// </summary>
internal sealed class TlsTestServer : IDisposable
{
    private readonly TaskCompletionSource<X509Certificate2?> _clientCertificate =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private readonly CancellationTokenSource _cts = new();
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly bool _requireClientCertificate;
    private readonly X509Certificate2 _serverCertificate;

    public TlsTestServer(X509Certificate2 serverCertificate, bool requireClientCertificate = false)
    {
        _serverCertificate = serverCertificate;
        _requireClientCertificate = requireClientCertificate;
        _listener.Start();
        var port = ((IPEndPoint)_listener.LocalEndpoint).Port;
        BaseUrl = $"https://127.0.0.1:{port}";
        _ = ServeAsync();
    }

    public string BaseUrl { get; }

    /// <summary>Completes with the client certificate the server saw (null if none).</summary>
    public Task<X509Certificate2?> PresentedClientCertificate => _clientCertificate.Task;

    public void Dispose()
    {
        _cts.Cancel();
        try
        {
            _listener.Stop();
        }
        catch
        {
            // already stopped
        }

        _cts.Dispose();
    }

    private async Task ServeAsync()
    {
        try
        {
            using var connection = await _listener.AcceptTcpClientAsync(_cts.Token).ConfigureAwait(false);
            await using var ssl = new SslStream(connection.GetStream(), false);
            var options = new SslServerAuthenticationOptions
            {
                ServerCertificate = _serverCertificate,
                ClientCertificateRequired = _requireClientCertificate,
                RemoteCertificateValidationCallback = (_, _, _, _) => true
            };

            await ssl.AuthenticateAsServerAsync(options, _cts.Token).ConfigureAwait(false);
            _clientCertificate.TrySetResult(ssl.RemoteCertificate is { } c ? new X509Certificate2(c) : null);

            // Drain request headers.
            var buffer = new byte[4096];
            await ssl.ReadAsync(buffer, _cts.Token).ConfigureAwait(false);

            var response = Encoding.ASCII.GetBytes(
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
            await ssl.WriteAsync(response, _cts.Token).ConfigureAwait(false);
            await ssl.FlushAsync(_cts.Token).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            _clientCertificate.TrySetException(ex);
        }
    }
}