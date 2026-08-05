/* SPDX-License-Identifier: Apache-2.0 */

using System.Net;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Builds the underlying <see cref="SocketsHttpHandler" /> for the wrapper: native auto-redirect,
///     auto-decompression, and the cookie container are disabled (the wrapper enforces those), while
///     connection pooling, proxy, and TLS (trust certificate + mTLS client certificate) are configured
///     on the native handler. Separated from <see cref="HttpClientRuntime" /> so handler/TLS construction
///     is independent of request orchestration.
/// </summary>
internal static class HttpTransportFactory
{
    public static HttpTransport Create(HttpClientOptions options)
    {
        var handler = new SocketsHttpHandler
        {
            AllowAutoRedirect = false,
            AutomaticDecompression = DecompressionMethods.None,
            UseCookies = false
        };

        if (options.Proxy is not null)
        {
            handler.UseProxy = true;
            var proxy = new WebProxy(options.Proxy);
            if (options.ProxyCredentials is { } credentials)
                proxy.Credentials = new NetworkCredential(credentials.User, credentials.Password);
            handler.Proxy = proxy;
        }
        else
        {
            handler.UseProxy = false;
        }

        var ownedCertificates = ConfigureTls(handler, options);
        return new HttpTransport(handler, ownedCertificates);
    }

    private static IReadOnlyList<X509Certificate2> ConfigureTls(SocketsHttpHandler handler, HttpClientOptions options)
    {
        if (options.TrustCertificateFile is null && options.ClientCertificate is null) return [];

        var sslOptions = new SslClientAuthenticationOptions();
        var ownedCertificates = new List<X509Certificate2>();

        if (options.ClientCertificate is { } clientCertificate)
        {
            var certificate = X509Certificate2.CreateFromPemFile(
                clientCertificate.CertificatePath, clientCertificate.KeyPath);
            sslOptions.ClientCertificates = new X509CertificateCollection { certificate };
            ownedCertificates.Add(certificate);
        }

        if (options.TrustCertificateFile is { } trustPath)
        {
            // The trust certificate is a certificate only (no private key); CreateFromPemFile
            // would try to extract a matching key and fail.
            var trusted = X509Certificate2.CreateFromPem(File.ReadAllText(trustPath));
            ownedCertificates.Add(trusted);
            sslOptions.RemoteCertificateValidationCallback = (_, presented, suppliedChain, errors) =>
            {
                if (errors == SslPolicyErrors.None) return true;

                if (presented is null) return false;

                // Hostname verification stays enabled: a name mismatch (or missing cert) is never
                // accepted, even when the thumbprint matches the pinned trust certificate.
                if ((errors & (SslPolicyErrors.RemoteCertificateNameMismatch |
                               SslPolicyErrors.RemoteCertificateNotAvailable)) != 0) return false;

                using var presentedCertificate = new X509Certificate2(presented);

                // Pinned trust: the server presents exactly the trusted certificate (the common
                // case for a self-signed test certificate, which cannot act as a CA root).
                if (string.Equals(presentedCertificate.Thumbprint, trusted.Thumbprint,
                        StringComparison.OrdinalIgnoreCase)) return true;

                // CA trust: the presented certificate chains to the trusted certificate as root.
                using var chain = new X509Chain();
                chain.ChainPolicy.TrustMode = X509ChainTrustMode.CustomRootTrust;
                chain.ChainPolicy.CustomTrustStore.Add(trusted);
                chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;

                // Include server-sent intermediates so a certificate that chains to the trusted root
                // through intermediates still validates.
                if (suppliedChain is not null)
                    foreach (var element in suppliedChain.ChainElements)
                        chain.ChainPolicy.ExtraStore.Add(element.Certificate);

                return chain.Build(presentedCertificate);
            };
        }

        handler.SslOptions = sslOptions;
        return ownedCertificates;
    }
}
