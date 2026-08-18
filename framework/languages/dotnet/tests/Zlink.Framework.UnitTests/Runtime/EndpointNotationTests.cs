using Zlink.Framework.Runtime.Configuration;

namespace Zlink.Framework.UnitTests;

public sealed class EndpointNotationTests
{
    [Theory]
    [InlineData("TCP://Host:0080", "tcp://host:80")]
    [InlineData("tcp://HOST.EXAMPLE.COM:443", "tcp://host.example.com:443")]
    [InlineData("TLS://Host:0080", "tls://host:80")]
    [InlineData("tcp://Host:00080/a/b/", "tcp://host:80/a/b")]
    [InlineData("tcp://Host:0080/", "tcp://host:80")]
    [InlineData("  tcp://host:80  ", "tcp://host:80")]
    [InlineData("tcp://127.0.0.1:80/", "tcp://127.0.0.1:80")]
    [InlineData("tcp://[::1]:80", "tcp://[::1]:80")]
    [InlineData("tcp://[FE80::1]:80", "tcp://[fe80::1]:80")]
    public void Normalize_ProducesDeterministicCanonicalForm(string input, string expected)
    {
        Assert.Equal(expected, ZLinkEndpointNotation.Normalize(input));
    }

    [Fact]
    public void Normalize_IsIdempotent()
    {
        var once = ZLinkEndpointNotation.Normalize("TCP://Host:0080/a/b/");
        var twice = ZLinkEndpointNotation.Normalize(once);
        Assert.Equal(once, twice);
    }

    [Fact]
    public void Normalize_PreservesIPv6ZoneId_CanonicalPercent25Form()
    {
        Assert.Equal(
            "tcp://[fe80::1%25eth0]:80",
            ZLinkEndpointNotation.Normalize("tcp://[fe80::1%25eth0]:80"));
    }

    [Fact]
    public void Normalize_PreservesIPv6ZoneId_EvenWhenGivenUnescaped()
    {
        // RFC 6874 requires the zone id delimiter to be escaped as %25, but a
        // caller that passed the raw '%' must still keep the zone id value:
        // normalization only re-encodes it into canonical form, it never
        // drops it.
        Assert.Equal(
            "tcp://[fe80::1%25eth0]:80",
            ZLinkEndpointNotation.Normalize("tcp://[fe80::1%eth0]:80"));
    }

    [Fact]
    public void Normalize_PreservesUserInfoQueryAndFragment_Losslessly()
    {
        Assert.Equal(
            "tcp://user:pass@host.example.com:443/path?q=1#frag",
            ZLinkEndpointNotation.Normalize(
                "tcp://user:pass@Host.Example.com:0443/path/?q=1#frag"));
    }

    [Fact]
    public void Normalize_DoesNotResolveDns_LocalhostAndLoopbackStayDistinct()
    {
        var localhost = ZLinkEndpointNotation.Normalize("tcp://localhost:80");
        var loopback = ZLinkEndpointNotation.Normalize("tcp://127.0.0.1:80");

        Assert.NotEqual(localhost, loopback);
        Assert.Equal("tcp://localhost:80", localhost);
        Assert.Equal("tcp://127.0.0.1:80", loopback);
    }

    [Fact]
    public void Normalize_UnparsableInput_FailsOpenAndReturnsTrimmedValue()
    {
        Assert.Equal("not-a-uri", ZLinkEndpointNotation.Normalize("  not-a-uri  "));
        Assert.Equal(string.Empty, ZLinkEndpointNotation.Normalize(null));
        Assert.Equal(string.Empty, ZLinkEndpointNotation.Normalize("   "));
    }

    // doc/plan/endpoint-notation-policy.ko.md §2.2: host/port/slash rules
    // apply only to schemes with a real network authority (tcp/tls/ws/wss).
    // A scheme like ipc:// is followed by a filesystem path, not an
    // authority -- only the scheme token is lowercased, every other byte
    // (path casing, slash count, trailing slash) is preserved exactly.
    // Matches the C++ (endpoint_notation.hpp, dfcb2177c9) and Node
    // (7a8c4b7945) implementations.
    [Theory]
    [InlineData("ipc:///var/run/Zlink/Socket.sock", "ipc:///var/run/Zlink/Socket.sock")]
    [InlineData("IPC:///var/run/Zlink/Socket.sock", "ipc:///var/run/Zlink/Socket.sock")]
    [InlineData("ipc://./Relative/Path/", "ipc://./Relative/Path/")]
    [InlineData("  IPC:///Trailing/Slash/  ", "ipc:///Trailing/Slash/")]
    [InlineData("INPROC://Some-Node-Identity", "inproc://Some-Node-Identity")]
    public void Normalize_OpaqueScheme_OnlyLowercasesSchemeAndKeepsRemainderByteIdentical(
        string input,
        string expected)
    {
        Assert.Equal(expected, ZLinkEndpointNotation.Normalize(input));
    }

    [Fact]
    public void IsAuthorityScheme_ClassifiesTcpTlsWsWssAsAuthorityAndIpcAsOpaque()
    {
        Assert.True(ZLinkEndpointNotation.IsAuthorityScheme("tcp"));
        Assert.True(ZLinkEndpointNotation.IsAuthorityScheme("tls"));
        Assert.True(ZLinkEndpointNotation.IsAuthorityScheme("ws"));
        Assert.True(ZLinkEndpointNotation.IsAuthorityScheme("wss"));
        Assert.False(ZLinkEndpointNotation.IsAuthorityScheme("ipc"));
        Assert.False(ZLinkEndpointNotation.IsAuthorityScheme("inproc"));
    }
}
