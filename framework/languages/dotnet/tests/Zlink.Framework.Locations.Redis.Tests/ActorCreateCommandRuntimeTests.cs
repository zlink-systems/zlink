using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Actors;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed class ActorCreateCommandRuntimeTests
{
    [Fact]
    public void SameProcessObjectServerRemainsEligibleForActorAndUserSpotPlacement()
    {
        var rid = RoutingId.From("same-process-server");
        var descriptor = new ZLinkMeshNodeDescriptor(
            "objects",
            rid,
            7,
            1,
            "inproc://same-process-server",
            new Dictionary<string, int>(),
            "test",
            "owner",
            3,
            DateTimeOffset.UtcNow)
        {
            State = ZLinkFrameworkRuntimeState.Serving,
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = "same-process-entry",
            PlacementWeight = 100,
            ObjectCapabilities =
            [
                new(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0),
                new(
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    10)
            ],
            Capacity = new(
                new ZLinkPopulationCapacity(0, 0, 10),
                new ZLinkPopulationCapacity(0, 0, 10),
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.UserSpot,
                        "room",
                        0,
                        0,
                        10)
                ])
        };

        Assert.True(ZLinkActorManagerService.IsEligibleCandidate(
            descriptor,
            "player"));
        Assert.True(ZLinkSpotRuntimeManager.IsEligibleCandidate(
            descriptor,
            "room"));
    }

    [Fact]
    public async Task TwoNodeRejectedOperationReplaysOnlySameOperation()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "actor-command-source");
        await using var target = NewNode(context, "actor-command-target");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://actor-command-source-{suffix}";
        var targetEndpoint = $"inproc://actor-command-target-{suffix}";
        source.SetBind(sourceEndpoint);
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, target.RoutingId);
        target.ConnectPeer(sourceEndpoint, source.RoutingId);
        var operationTarget = new RejectingTarget();
        target.SetActorCreateOperationTarget(operationTarget);
        source.Start();
        target.Start();
        await WaitUntilAsync(() =>
            source.Status().AdmittedPeerCount == 1
            && target.Status().AdmittedPeerCount == 1);

        var fence = new ObjectReservationFence(
            "reservation-actor",
            "store-actor",
            17,
            19,
            target.RoutingId,
            target.Status().LifecycleGeneration,
            "owner",
            23,
            1);
        var deadline = checked(
            (ulong)DateTimeOffset.UtcNow.AddSeconds(5).ToUnixTimeMilliseconds());
        Assert.Equal(
            SubmitResult.Ok,
            source.CreateActorRemote(
                target.RoutingId,
                "actor-1",
                "player",
                fence,
                deadline,
                out var firstOperation,
                TimeSpan.FromSeconds(5)));
        await WaitUntilAsync(() => source.Status().PendingInfrastructureMessages > 0);
        var first = DrainCompletion(source, firstOperation);
        Assert.Equal(ActorCreateResult.Rejected, first.ActorCreateCompletion!.Result);
        Assert.Equal(1, operationTarget.Count);

        Assert.Equal(
            SubmitResult.Ok,
            source.ResubmitActorCreateOperation(target.RoutingId, operationTarget.Last));
        await Task.Delay(50);
        Assert.Equal(1, operationTarget.Count);

        Assert.Equal(
            SubmitResult.Ok,
            source.CreateActorRemote(
                target.RoutingId,
                "actor-1",
                "player",
                fence,
                deadline,
                out var secondOperation,
                TimeSpan.FromSeconds(5)));
        await WaitUntilAsync(() => source.Status().PendingInfrastructureMessages > 0);
        var second = DrainCompletion(source, secondOperation);
        Assert.Equal(ActorCreateResult.Rejected, second.ActorCreateCompletion!.Result);
        Assert.NotEqual(firstOperation, secondOperation);
        Assert.Equal(2, operationTarget.Count);
    }

    [Fact]
    public async Task TwoNodeDestroyCarriesExactActorAndAuthorityFences()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "actor-destroy-source");
        await using var target = NewNode(context, "actor-destroy-target");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://actor-destroy-source-{suffix}";
        var targetEndpoint = $"inproc://actor-destroy-target-{suffix}";
        source.SetBind(sourceEndpoint);
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, target.RoutingId);
        target.ConnectPeer(sourceEndpoint, source.RoutingId);
        var operationTarget = new DestroyingTarget();
        target.SetActorDestroyOperationTarget(operationTarget);
        source.Start();
        target.Start();
        await WaitUntilAsync(() =>
            source.Status().AdmittedPeerCount == 1
            && target.Status().AdmittedPeerCount == 1);

        var actor = new ActorRef("actor-27", 17, "mesh", target.RoutingId);
        var targetGeneration = target.Status().LifecycleGeneration;
        Assert.Equal(
            SubmitResult.Ok,
            source.DestroyActorRemote(
                actor,
                targetGeneration,
                23,
                out var operationId,
                TimeSpan.FromSeconds(5)));
        await WaitUntilAsync(() =>
            source.Status().PendingInfrastructureMessages > 0);

        var completion = DrainCompletion(source, operationId);
        Assert.True(completion.ActorDestroyCompletion!.Destroyed);
        Assert.Equal(actor, operationTarget.Last.Actor);
        Assert.Equal(target.RoutingId, operationTarget.Last.TargetNodeRid);
        Assert.Equal(
            targetGeneration,
            operationTarget.Last.TargetNodeGeneration);
        Assert.Equal(23UL, operationTarget.Last.AuthorityOwnerGeneration);
    }

    private static ZLinkManagedMeshNode NewNode(IContext context, string rid)
    {
        var node = new ZLinkManagedMeshNode(context, "mesh");
        node.SetRoutingId(RoutingId.From(rid));
        return node;
    }

    private static MeshReceiveRecord DrainCompletion(
        ZLinkManagedMeshNode node,
        MeshOperationId operationId)
    {
        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            while (claim.Receive(received, RecvFlags.DontWait))
                for (var record = 0; record < received.Count; record++)
                    if (received[record].Kind == MeshRecordKind.Completion
                        && received[record].OperationId == operationId)
                        return received[record];
        }
        throw new InvalidOperationException("Actor completion was not queued.");
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        while (!condition())
            await Task.Delay(10, timeout.Token);
    }

    private sealed class RejectingTarget : IActorCreateOperationTarget
    {
        public int Count { get; private set; }

        public ZLinkServiceWireCodec.ActorCreateOperationRecord Last { get; private set; }

        public ValueTask<ActorCreateOperationTerminal> CreateAsync(
            ActorCreateOperation operation,
            CancellationToken cancellationToken)
        {
            Count++;
            Last = new ZLinkServiceWireCodec.ActorCreateOperationRecord(operation);
            return ValueTask.FromResult(new ActorCreateOperationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                new ActorCreateCompletion(ActorCreateResult.Rejected, default)));
        }
    }

    private sealed class DestroyingTarget : IActorDestroyOperationTarget
    {
        public ActorDestroyOperation Last { get; private set; }

        public ValueTask<ActorDestroyOperationTerminal> DestroyAsync(
            ActorDestroyOperation operation,
            CancellationToken cancellationToken)
        {
            Last = operation;
            return ValueTask.FromResult(new ActorDestroyOperationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                new ActorDestroyCompletion(true)));
        }
    }
}
