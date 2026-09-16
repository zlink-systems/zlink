using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests;

// 06-framework-api "no eligible select-one member": applying eligibility and
// drain can leave a select-one channel with nothing to pick while the send path
// and its connection are still there. That ends as Unavailable, not NotFound,
// and a request and a one-way send agree.
public sealed class ChannelNoEligibleMemberTests
{
    [Fact]
    public void ChannelRequestWithNoEligibleMember_IsUnavailable()
    {
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateChannelCompletionException(
                RequestResult.NotFound,
                "Channel request to 'profile'"));

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
    }

    [Fact]
    public void NodeDirectRequestWithMissingTarget_StaysNotFound()
    {
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateCompletionException(
                RequestResult.NotFound,
                "Node request to 'play-node'"));

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
    }

    [Fact]
    public void ChannelRequestKeepsEveryOtherTerminalResult()
    {
        var notConnected = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateChannelCompletionException(
                RequestResult.NotConnected,
                "Channel request to 'profile'"));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, notConnected.Kind);

        var timedOut = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateChannelCompletionException(
                RequestResult.TimedOut,
                "Channel request to 'profile'"));
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, timedOut.Kind);
    }
}
