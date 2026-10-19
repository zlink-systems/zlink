using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Theory]
    [InlineData(1, 1, 1, 1, false, true)]
    [InlineData(2, 2, 1, 1, false, true)]
    [InlineData(1, 2, 1, 1, false, false)]
    [InlineData(2, 2, 2, 1, false, false)]
    [InlineData(2, 2, 1, 2, false, false)]
    [InlineData(2, 2, 1, 1, true, false)]
    [InlineData(1, 1, 1, 1, false, false, 2)]
    [InlineData(2, 1, 1, 1, false, false, 1, "previous-owner")]
    [InlineData(1, 1, 1, 1, false, true, 1, "owner", false)]
    [InlineData(2, 2, 1, 1, false, true, 1, "owner", false)]
    public async Task Ready_spot_admission_distinguishes_incarnation_from_owner_change(
        ulong currentGeneration,
        ulong currentAuthority,
        ulong currentLease,
        ulong nodeGenerationOffset,
        bool releaseOwner,
        bool accepted,
        ulong resolvedGeneration = 1,
        string resolvedOwner = "owner",
        bool readyRoute = true
    )
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = NewNode(context, "ready-fence-caller");
        await using var target = NewNode(context, "ready-fence-target");
        target.SetLocalOwnerLeaseGeneration(currentLease);
        target.SetLocalRequestSourceFence(
            new ZLinkServiceWireCodec.RequestSourceFence(
                "owner",
                currentLease,
                target.RoutingId,
                target.Status().LifecycleGeneration
            )
        );
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://ready-fence-caller-{suffix}";
        var targetEndpoint = $"inproc://ready-fence-target-{suffix}";
        caller.SetBind(callerEndpoint);
        target.SetBind(targetEndpoint);
        caller.ConnectPeer(targetEndpoint, target.RoutingId);
        caller.Start();
        target.Start();
        await WaitUntilAsync(() =>
            caller.Status().AdmittedPeerCount == 1 && target.Status().AdmittedPeerCount == 1
        );
        const string spotId = "ready-fence-instance";
        var original = target.GetOrCreateReservedSpot(spotId, 1, 1, out _);
        var snapshot = new ZLinkSpotHandleSnapshot(
            "mesh",
            target.RoutingId,
            spotId,
            resolvedGeneration,
            ZLinkSpotKind.Instance,
            1,
            target.Status().LifecycleGeneration + nodeGenerationOffset - 1,
            1,
            resolvedOwner,
            "resolved-version"
        );
        caller.ObserveSpotAuthority(
            target.RoutingId,
            spotId,
            resolvedGeneration,
            target.Status().LifecycleGeneration,
            1,
            1
        );
        // Resolve precedes Close/recreate. The original route remains immutable.
        if (currentGeneration != 1 || currentAuthority != 1 || releaseOwner)
        {
            original.Dispose();
            if (!releaseOwner)
                target.GetOrCreateReservedSpot(spotId, currentGeneration, currentAuthority, out _);
        }
        DrainAndDispose(target);
        using var payload = Message.From(new byte[] { 42 });
        var request = caller
            .RequestToSpotDirectAsync(
                ((ZLinkManagedSpot)caller.EntrySpot()).SpotId,
                target.RoutingId,
                spotId,
                resolvedGeneration,
                [payload],
                SendFlags.None,
                ReadOnlyMemory<byte>.Empty,
                TimeSpan.FromSeconds(5),
                CancellationToken.None,
                readyRoute
                    ? Assert.IsType<ServiceWireCodec.InstanceRouteV1Case0>(
                        ZLinkServiceWireCodec.CreateInstanceReadyRoute(snapshot, true)
                    )
                    : null
            )
            .AsTask();
        await WaitUntilAsync(() =>
            request.IsCompleted || target.Status().PendingApplicationMessages != 0
        );
        if (accepted)
        {
            Assert.False(request.IsCompleted);
            using var ready = new MeshReadyBatch();
            target.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
            Assert.Equal(1, ready.Count);
            using var claim = ready.TakeClaim(0);
            using var received = new MeshReceiveBatch();
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            Assert.Equal(spotId, ready[0].SpotId);
            Assert.Equal(1UL, received[0].AuthorityOwnerGeneration);
            Assert.Equal(SubmitResult.Ok, received[0].Reply([payload]));
            using var reply = await request;
        }
        else
        {
            Assert.True(request.IsCompleted);
            var failure = await Assert.ThrowsAsync<ZLinkRequestTerminalException>(() => request);
            Assert.Equal(RequestResult.Conflict, failure.Result);
            Assert.Equal(
                (int)(
                    nodeGenerationOffset == 1
                        ? resolvedGeneration > currentGeneration
                            ? ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale
                            : ServiceWireConstants.FrameworkErrorCode.SpotMoving
                        : ServiceWireConstants.FrameworkErrorCode.ActorLocationStale
                ),
                failure.FailureErrno
            );
            Assert.Equal(0UL, target.Status().PendingApplicationMessages);
        }
    }
}
