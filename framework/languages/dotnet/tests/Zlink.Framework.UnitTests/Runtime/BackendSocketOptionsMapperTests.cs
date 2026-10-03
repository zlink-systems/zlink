using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Configuration;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class BackendSocketOptionsMapperTests
{
    [Fact]
    public void Apply_LeavesExistingValues_WhenBooleanOptionsAreNotSet()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateDealerSocket();
        socket.Options.IPv6 = true;
        socket.Options.TcpNoDelay = true;
        socket.Options.Immediate = true;

        ZLinkBackendSocketOptionsMapper.Apply(socket.Options, new ZLinkSocketConfig());

        Assert.True(socket.Options.IPv6);
        Assert.True(socket.Options.TcpNoDelay);
        Assert.True(socket.Options.Immediate);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void Apply_AppliesExplicitBooleanOptions(bool value)
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateDealerSocket();

        ZLinkBackendSocketOptionsMapper.Apply(
            socket.Options,
            new ZLinkSocketConfig
            {
                IPv6 = value,
                TcpNoDelay = value,
                Immediate = value,
            }
        );

        Assert.Equal(value, socket.Options.IPv6);
        Assert.Equal(value, socket.Options.TcpNoDelay);
        Assert.Equal(value, socket.Options.Immediate);
    }
}
