using Zlink.Framework.Runtime.Configuration;

namespace Zlink.Framework.UnitTests;

public sealed class NetworkEndpointResolverTests
{
    [Fact]
    public void Bind_NormalizesExplicitEndpoint()
    {
        var network = new ZLinkNetworkOptionsModel();

        var bound = ZLinkNetworkEndpointResolver.Bind(
            "TCP://Host:0080",
            port: null,
            listenerBindHost: null,
            network);

        Assert.Equal("tcp://host:80", bound);
    }

    [Fact]
    public void Bind_NormalizesGeneratedEndpoint()
    {
        var network = new ZLinkNetworkOptionsModel();

        var bound = ZLinkNetworkEndpointResolver.Bind(
            explicitEndpoint: null,
            port: 7101,
            listenerBindHost: "127.0.0.1",
            network);

        Assert.Equal("tcp://127.0.0.1:7101", bound);
    }

    [Fact]
    public void Advertise_NormalizesResultForTcpScheme()
    {
        var network = new ZLinkNetworkOptionsModel();

        var advertised = ZLinkNetworkEndpointResolver.Advertise(
            "TCP://0.0.0.0:0080",
            listenerAdvertiseHost: "Host.Example.com",
            listenerBindHost: "0.0.0.0",
            network);

        Assert.Equal("tcp://host.example.com:80", advertised);
    }

    [Fact]
    public void Advertise_AppliesAdvertiseHostAndNormalizationForNonTcpSchemes()
    {
        // The tcp-only early return that used to leave non-tcp schemes
        // completely unnormalized (and without the advertise-host
        // substitution applied) must be gone.
        var network = new ZLinkNetworkOptionsModel();

        var advertised = ZLinkNetworkEndpointResolver.Advertise(
            "TLS://0.0.0.0:0080",
            listenerAdvertiseHost: "Host.Example.com",
            listenerBindHost: "0.0.0.0",
            network);

        Assert.Equal("tls://host.example.com:80", advertised);
    }

    [Fact]
    public void Advertise_NormalizesButDoesNotSubstituteHost_ForOpaqueLocalSchemes()
    {
        // inproc/ipc endpoints carry a process-local identity in the "host"
        // segment, not a dialable network address. AdvertiseHost must never
        // overwrite it, even though every scheme is now normalized the same
        // way (doc/plan/endpoint-notation-policy.ko.md §2.2/§2.3).
        var network = new ZLinkNetworkOptionsModel { AdvertiseHost = "Host.Example.com" };

        var advertised = ZLinkNetworkEndpointResolver.Advertise(
            "INPROC://Some-Node-Identity",
            listenerAdvertiseHost: null,
            listenerBindHost: null,
            network);

        // Per §2.2, inproc's remainder is opaque (a process-local identity,
        // not a network authority): only the scheme is lowercased, the
        // identity's own casing is preserved byte-for-byte.
        Assert.Equal("inproc://Some-Node-Identity", advertised);
    }

    [Fact]
    public void Advertise_ThrowsWhenWildcardBindHasNoAdvertiseHost()
    {
        var network = new ZLinkNetworkOptionsModel();

        Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkNetworkEndpointResolver.Advertise(
                "tcp://0.0.0.0:7101",
                listenerAdvertiseHost: null,
                listenerBindHost: "0.0.0.0",
                network));
    }
}
