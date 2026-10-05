using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;

namespace Zlink.Framework.ContractTests.Channels;

public sealed class OneWaySendContracts
{
    [Theory]
    [InlineData(typeof(IZLinkSendCall))]
    [InlineData(typeof(IZLinkSpotSendCall))]
    [InlineData(typeof(IZLinkActorSendCall))]
    [InlineData(typeof(IZLinkBoundSessionSendCall))]
    [InlineData(typeof(IZLinkSessionSendCall))]
    [InlineData(typeof(IZLinkSessionReplyCall))]
    public void SendAndReply_HaveNoTimeoutOrCancellationInput(Type contract)
    {
        Assert.DoesNotContain(contract.GetMethods(), method => method.Name == "Timeout");
        Assert.Empty(contract.GetMethod("Async")!.GetParameters());
    }

    [Fact]
    public void SessionActorRelay_HasNoCancellationInput()
    {
        Assert.All(
            typeof(IZLinkSessionActor).GetMethods().Where(method => method.Name == "RelayAsync"),
            method =>
                Assert.DoesNotContain(
                    method.GetParameters(),
                    parameter => parameter.ParameterType == typeof(CancellationToken)
                )
        );
    }

    [Theory]
    [InlineData(typeof(IZLinkSocketConfig))]
    [InlineData(typeof(IZLinkStreamSocketConfig))]
    [InlineData(typeof(IZLinkMeshNodeSocketConfig))]
    public void OneWaySocket_HasNoSendTimeout(Type contract)
    {
        Assert.Null(contract.GetProperty("SendTimeout"));
    }

    [Fact]
    public void ClassicFanoutPublisher_KeepsSendTimeout()
    {
        Assert.NotNull(typeof(IZLinkSpotPublisherConfig).GetProperty("SendTimeout"));
        Assert.NotNull(typeof(IZLinkFrameworkOptions).GetProperty("DefaultSocketSendTimeout"));
    }
}
