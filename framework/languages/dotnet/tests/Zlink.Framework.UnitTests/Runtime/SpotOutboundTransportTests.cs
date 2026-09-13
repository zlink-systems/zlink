using System.Reflection;

using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class SpotOutboundTransportTests
{
    [Fact]
    public async Task NodeControlRouteWithoutSpotGeneration_DoesNotObserveUserSpotAuthority()
    {
        var spot = DispatchProxy.Create<
            IAuthorityAwareBackendSpot,
            AuthorityAwareBackendSpotProxy>();
        var proxy = (AuthorityAwareBackendSpotProxy)(object)spot;

        await using var transport = new ZLinkSpotOutboundTransport(
            spot,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);
        using var payload = Message.From("node-control");

        var result = transport.TrySendToSpotOnce(
            RoutingId.From("target-node"),
            "__zlink.node.control",
            targetSpotGeneration: 0,
            targetNodeGeneration: 11,
            authorityOwnerGeneration: 13,
            ownerLeaseGeneration: 17,
            [payload]);

        Assert.True(result);
        Assert.Equal(0, proxy.ObservedSpotAuthorityCount);
        Assert.Equal(1, proxy.SpotSendCount);
    }

    [Fact]
    public async Task UserSpotRouteWithGeneration_ObservesExactAuthorityFence()
    {
        var spot = DispatchProxy.Create<
            IAuthorityAwareBackendSpot,
            AuthorityAwareBackendSpotProxy>();
        var proxy = (AuthorityAwareBackendSpotProxy)(object)spot;

        await using var transport = new ZLinkSpotOutboundTransport(
            spot,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);
        using var payload = Message.From("user-spot");

        var result = transport.TrySendToSpotOnce(
            RoutingId.From("target-node"),
            "user-spot-1",
            targetSpotGeneration: 7,
            targetNodeGeneration: 11,
            authorityOwnerGeneration: 13,
            ownerLeaseGeneration: 17,
            [payload]);

        Assert.True(result);
        Assert.Equal(1, proxy.ObservedSpotAuthorityCount);
        Assert.Equal(7UL, proxy.ObservedObjectGeneration);
        Assert.Equal(1, proxy.SpotSendCount);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task DirectSpotCallsReleaseBorrowedPayloadOnSuccessAndAuthorityFailure(
        bool request, bool authorityFailure)
    {
        var spot = DispatchProxy.Create<IAuthorityAwareBackendSpot, AuthorityAwareBackendSpotProxy>();
        var proxy = (AuthorityAwareBackendSpotProxy)(object)spot;
        proxy.AuthorityFailure = authorityFailure ? new InvalidOperationException("authority rejected") : null;
        await using var transport = new ZLinkSpotOutboundTransport(spot, null, CancellationToken.None);
        var payload = Message.Allocate(4096);
        using var witness = payload.Copy();
        async Task Submit()
        {
            if (request)
            {
                using var reply = await transport.RequestToSpotAsync(
                    RoutingId.From("remote"), "spot", 1, 1, 1, 1, [payload],
                    TimeSpan.FromSeconds(1), CancellationToken.None);
                Assert.Equal("reply", reply.Parts[0].GetString());
            }
            else
            {
                var result = await transport.SendToSpotAsync(
                    RoutingId.From("remote"), "spot", 1, 1, 1, 1, [payload], CancellationToken.None);
                Assert.Equal(ZLinkOneWaySubmitStatus.Submitted, result.Status);
            }
        }
        if (authorityFailure) await Assert.ThrowsAsync<InvalidOperationException>(Submit);
        else await Submit();
        Assert.Equal(1, witness.RefCount);
    }

    private interface IAuthorityAwareBackendSpot :
        IZLinkBackendSpot,
        IZLinkBackendAuthorityObserver;

    private class AuthorityAwareBackendSpotProxy : DispatchProxy
    {
        internal int ObservedSpotAuthorityCount { get; private set; }

        internal ulong ObservedObjectGeneration { get; private set; }

        internal int SpotSendCount { get; private set; }
        internal Exception? AuthorityFailure { get; set; }

        protected override object? Invoke(
            MethodInfo? targetMethod,
            object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            return targetMethod.Name switch
            {
                nameof(IZLinkBackendAuthorityObserver.ObserveSpotAuthority) =>
                    ObserveSpotAuthority(args),
                nameof(IZLinkBackendSpot.SendToSpot) => SendToSpot(),
                nameof(IZLinkBackendSpot.SendToSpotAsync) => ValueTask.CompletedTask,
                nameof(IZLinkBackendSpot.RequestToSpotAsync) => ValueTask.FromResult(
                    new ZLinkBackendRouteReceived([Message.From("reply")], null, null, null, null)),
                nameof(IAsyncDisposable.DisposeAsync) => ValueTask.CompletedTask,
                _ => throw new NotSupportedException(targetMethod.Name)
            };
        }

        private object? ObserveSpotAuthority(object?[]? args)
        {
            if (AuthorityFailure is not null) throw AuthorityFailure;
            ObservedSpotAuthorityCount++;
            ObservedObjectGeneration = (ulong)args![2]!;
            return null;
        }

        private object SendToSpot()
        {
            SpotSendCount++;
            return SubmitResult.Ok;
        }
    }
}
