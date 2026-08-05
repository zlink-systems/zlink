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

    private interface IAuthorityAwareBackendSpot :
        IZLinkBackendSpot,
        IZLinkBackendAuthorityObserver;

    private class AuthorityAwareBackendSpotProxy : DispatchProxy
    {
        internal int ObservedSpotAuthorityCount { get; private set; }

        internal ulong ObservedObjectGeneration { get; private set; }

        internal int SpotSendCount { get; private set; }

        protected override object? Invoke(
            MethodInfo? targetMethod,
            object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            return targetMethod.Name switch
            {
                nameof(IZLinkBackendSpot.OnSendReady) => null,
                nameof(IZLinkBackendAuthorityObserver.ObserveSpotAuthority) =>
                    ObserveSpotAuthority(args),
                nameof(IZLinkBackendSpot.SendToSpot) => SendToSpot(),
                nameof(IAsyncDisposable.DisposeAsync) => ValueTask.CompletedTask,
                _ => throw new NotSupportedException(targetMethod.Name)
            };
        }

        private object? ObserveSpotAuthority(object?[]? args)
        {
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
