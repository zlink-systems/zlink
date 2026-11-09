using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Zlink.Framework.Locations.Etcd.Tests;

public sealed class EtcdTlsTests
{
    [Fact]
    public async Task Custom_trusted_CA_and_client_certificate_chain_complete_mutual_TLS()
    {
        using var certificates = new Certificates();
        string? observedClient = null;
        var observedIntermediate = false;
        await using var server = await FakeEtcdServer.StartAsync(
            certificates.Server,
            (client, suppliedChain, _) =>
            {
                using var chain = new X509Chain();
                chain.ChainPolicy.TrustMode = X509ChainTrustMode.CustomRootTrust;
                chain.ChainPolicy.CustomTrustStore.Add(certificates.Root);
                chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
                chain.ChainPolicy.DisableCertificateDownloads = true;
                chain.ChainPolicy.ApplicationPolicy.Add(new Oid("1.3.6.1.5.5.7.3.2"));
                if (suppliedChain is not null)
                    foreach (var element in suppliedChain.ChainElements)
                        chain.ChainPolicy.ExtraStore.Add(element.Certificate);
                var valid = chain.Build(client);
                observedClient = client.Subject;
                observedIntermediate = chain
                    .ChainElements.Cast<X509ChainElement>()
                    .Any(element =>
                        element.Certificate.Thumbprint == certificates.Intermediate.Thumbprint
                    );
                return valid;
            }
        );
        server.Clock();
        await using var store = CreateStore(
            server.Endpoint,
            certificates.Root.ExportCertificatePem(),
            certificates
        );
        Assert.IsType<ZLinkStoreReadResult.Missing>(await store.ReadAsync(new("key")));
        Assert.Equal(certificates.Client.Subject, observedClient);
        Assert.True(
            observedIntermediate,
            "The server must receive the client intermediate certificate to build the chain."
        );
        Assert.Single(server.Calls);
    }

    [Fact]
    public async Task Untrusted_server_certificate_fails_before_the_RPC_is_received()
    {
        using var certificates = new Certificates();
        using var unrelatedKey = RSA.Create(2048);
        var unrelatedRequest = new CertificateRequest(
            "CN=unrelated-" + Guid.NewGuid().ToString("N"),
            unrelatedKey,
            HashAlgorithmName.SHA256,
            RSASignaturePadding.Pkcs1
        );
        unrelatedRequest.CertificateExtensions.Add(
            new X509BasicConstraintsExtension(true, false, 0, true)
        );
        using var unrelatedRoot = unrelatedRequest.CreateSelfSigned(
            DateTimeOffset.UtcNow.AddMinutes(-5),
            DateTimeOffset.UtcNow.AddDays(1)
        );
        await using var server = await FakeEtcdServer.StartAsync(
            certificates.Server,
            (_, _, _) => true
        );
        server.Clock();
        await using var store = CreateStore(
            server.Endpoint,
            unrelatedRoot.ExportCertificatePem(),
            certificates
        );
        await Assert.ThrowsAnyAsync<IOException>(() => store.ReadAsync(new("key")).AsTask());
        Assert.Empty(server.Calls);
    }

    private static ZLinkEtcdLocationStore CreateStore(
        string endpoint,
        string trustedCa,
        Certificates certificates
    ) =>
        new(options =>
        {
            options.Namespace = "test";
            options.Endpoints.Add(endpoint);
            options.Tls = new()
            {
                TrustedCaPem = trustedCa,
                ClientIdentity = new()
                {
                    CertificatePem =
                        certificates.Client.ExportCertificatePem()
                        + "\n"
                        + certificates.Intermediate.ExportCertificatePem(),
                    KeyPem = certificates.ClientKey.ExportPkcs8PrivateKeyPem(),
                },
            };
        });

    private sealed class Certificates : IDisposable
    {
        private readonly List<RSA> keys = [];
        internal X509Certificate2 Root { get; }
        internal X509Certificate2 Intermediate { get; }
        internal X509Certificate2 Server { get; }
        internal X509Certificate2 Client { get; }
        internal RSA ClientKey { get; }

        internal Certificates()
        {
            var suffix = Guid.NewGuid().ToString("N");
            var rootKey = Key();
            var rootRequest = Request("CN=etcd-root-" + suffix, rootKey, true);
            Root = rootRequest.CreateSelfSigned(
                DateTimeOffset.UtcNow.AddMinutes(-5),
                DateTimeOffset.UtcNow.AddDays(2)
            );
            var intermediateKey = Key();
            Intermediate = Sign(
                Request("CN=etcd-intermediate-" + suffix, intermediateKey, true),
                Root,
                intermediateKey
            );
            var serverKey = Key();
            var serverRequest = Request("CN=localhost", serverKey, false, "1.3.6.1.5.5.7.3.1");
            var names = new SubjectAlternativeNameBuilder();
            names.AddDnsName("localhost");
            names.AddIpAddress(IPAddress.Loopback);
            serverRequest.CertificateExtensions.Add(names.Build());
            using var signedServer = Sign(serverRequest, Root, serverKey);
            // Windows Schannel requires a persisted server private key for TLS.
            Server = new X509Certificate2(signedServer.Export(X509ContentType.Pkcs12));
            ClientKey = Key();
            Client = Sign(
                Request("CN=etcd-client-" + suffix, ClientKey, false, "1.3.6.1.5.5.7.3.2"),
                Intermediate,
                ClientKey
            );
        }

        private RSA Key()
        {
            var key = RSA.Create(2048);
            keys.Add(key);
            return key;
        }

        private static CertificateRequest Request(
            string subject,
            RSA key,
            bool isCa,
            string? purpose = null
        )
        {
            var request = new CertificateRequest(
                subject,
                key,
                HashAlgorithmName.SHA256,
                RSASignaturePadding.Pkcs1
            );
            request.CertificateExtensions.Add(
                new X509BasicConstraintsExtension(isCa, false, 0, true)
            );
            request.CertificateExtensions.Add(
                new X509KeyUsageExtension(
                    isCa
                        ? X509KeyUsageFlags.KeyCertSign | X509KeyUsageFlags.CrlSign
                        : X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment,
                    true
                )
            );
            request.CertificateExtensions.Add(
                new X509SubjectKeyIdentifierExtension(request.PublicKey, false)
            );
            if (purpose is not null)
                request.CertificateExtensions.Add(
                    new X509EnhancedKeyUsageExtension(new OidCollection { new(purpose) }, false)
                );
            return request;
        }

        private static X509Certificate2 Sign(
            CertificateRequest request,
            X509Certificate2 issuer,
            RSA key
        )
        {
            using var certificate = request.Create(
                issuer,
                DateTimeOffset.UtcNow.AddMinutes(-3),
                new DateTimeOffset(issuer.NotAfter.ToUniversalTime().AddMinutes(-1)),
                RandomNumberGenerator.GetBytes(16)
            );
            return certificate.CopyWithPrivateKey(key);
        }

        public void Dispose()
        {
            Client.Dispose();
            Server.Dispose();
            Intermediate.Dispose();
            Root.Dispose();
            foreach (var key in keys)
                key.Dispose();
        }
    }
}
