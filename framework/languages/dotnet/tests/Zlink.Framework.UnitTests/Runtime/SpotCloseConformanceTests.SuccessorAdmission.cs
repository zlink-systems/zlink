using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class SpotCloseConformanceTests
{
    [Fact]
    public async Task Instance_target_admits_same_owner_successor_before_publication()
    {
        var flowPath = Path.Combine(
            Path.GetTempPath(),
            "zlink-close-dotnet",
            $"successor-admission-{Guid.NewGuid():N}.flow"
        );
        using var listener = new TestHostMessageFlowListener(flowPath);
        output.WriteLine($"Message flow file: {flowPath}");
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"successor-admission-{Guid.NewGuid():N}";
        var initial = await host.RequestInstanceAsync(spotId);
        var spotNode = host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName);
        var catalog = spotNode.Catalog;
        var node = spotNode.Node;
        var store = host.Runtime.Registration.Locations.ResolveStore()!;
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
        var original = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
            .Snapshot;
        var predecessor = await catalog.TryGetInstanceActivationAsync(
            spotId,
            SpotCloseHost.InstanceType,
            original.ObjectGeneration
        );
        Assert.NotNull(predecessor);
        host.State.HoldOnClosing = true;
        var close = catalog.CloseAsync(spotId, initial.Generation, CancellationToken.None).AsTask();
        try
        {
            await host.State.OnClosingEntered.Task.WaitAsync(Wait);
            var retained = await AdmitRetainedIntentAsync(
                host,
                predecessor,
                original,
                spotId,
                "pending-retained"
            );
            host.Store.HoldNextPutFor = spotId;
            host.Store.HoldAfterPut = true;
            host.State.ReleaseOnClosing.TrySetResult();
            await host.Store.PutHeld.Task.WaitAsync(Wait);
            var ready = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
                .Snapshot;
            Assert.Equal(initial.Generation + 1, ready.ObjectGeneration);
            Assert.Equal(ZLinkPlacementAllocationState.Active, ready.Allocation.State);
            Assert.True(
                ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    ready.Payload.Span,
                    out var authority
                )
            );
            Assert.Equal(ZLinkInstanceSpotAuthorityState.Ready, authority!.State);
            Assert.Same(
                predecessor,
                await catalog.TryGetInstanceActivationAsync(
                    spotId,
                    SpotCloseHost.InstanceType,
                    original.ObjectGeneration
                )
            );
            Assert.Null(
                await catalog.TryGetInstanceActivationAsync(
                    spotId,
                    SpotCloseHost.InstanceType,
                    ready.ObjectGeneration
                )
            );
            var target = new ZLinkInstanceSpotActivationTarget(
                store,
                host.Runtime.Registration.Locations.ResolveRelocationStore()!,
                catalog,
                node,
                host.Runtime.Registration.SpotNodes[SpotCloseHost.MeshName],
                () => new ZLinkLocationOwnerToken(ready.OwnerId, ready.OwnerLeaseGeneration),
                flow: host.Runtime.Flow
            );
            var id = node.AllocateOperationId();
            var operation = new InstanceSpotActivationOperation(
                new InstanceSpotActivationTarget(
                    SpotCloseHost.MeshName,
                    node.RoutingId,
                    node.MeshStatus().LifecycleGeneration,
                    spotId,
                    SpotCloseHost.InstanceType,
                    ready.StoreVersion
                ),
                node.RoutingId,
                node.MeshStatus().LifecycleGeneration,
                string.Empty,
                id,
                true,
                id.Low,
                checked((ulong)DateTimeOffset.UtcNow.Add(Wait).ToUnixTimeMilliseconds())
            );
            var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
                ZLinkClientCallCodec.CreateEnvelope(
                    ZLinkMessageKind.Request,
                    SpotCloseHost.MeshName,
                    nameof(SpotCloseProbeRequest),
                    Wait
                ),
                new SpotCloseProbeRequest("pending-successor"),
                host.Runtime.Registration.Codecs
            );
            Task<InstanceSpotActivationTerminal> pending;
            try
            {
                pending = target
                    .ActivateAsync(
                        operation,
                        null,
                        parts.Select(static part => (ReadOnlyMemory<byte>)part.ToArray()).ToArray(),
                        CancellationToken.None
                    )
                    .AsTask();
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(parts);
            }
            using var deadline = new CancellationTokenSource(Wait);
            while (true)
            {
                if (pending.IsCompleted)
                {
                    try
                    {
                        await pending;
                    }
                    catch (ZLinkFrameworkException error)
                    {
                        output.WriteLine($"Premature Instance refusal: {error.Kind}");
                        throw;
                    }
                    Assert.Fail("The Instance message completed before successor publication.");
                }
                var count = 0;
                predecessor._serial.HasPendingAcceptedState(_ =>
                {
                    count++;
                    return false;
                });
                if (count == 2)
                    break;
                deadline.Token.ThrowIfCancellationRequested();
                await Task.Yield();
            }
            Assert.False(pending.IsCompleted);
            Assert.False(retained.IsCompleted);
            Assert.Empty(host.State.PendingHandlerGenerations);
            host.Store.ReleasePut.TrySetResult();
            Assert.True(await close.WaitAsync(Wait));
            Assert.Equal(RequestResult.Ok, (await retained.WaitAsync(Wait)).Result);
            var terminal = await pending.WaitAsync(Wait);
            Assert.Equal(RequestResult.Ok, terminal.Result);
            var reply = ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<SpotCloseInstanceReply>(
                terminal.ReplyParts.Select(Message.From).ToArray(),
                "empty",
                "failed",
                host.Runtime.Registration.Codecs
            );
            Assert.Equal("pending-successor", reply.Marker);
            Assert.Equal(ready.ObjectGeneration, reply.Generation);
            Assert.Equal(
                new[] { "initial", "pending-retained", "pending-successor" },
                host.State.InstanceHandlerMarkers
            );
            Assert.Equal(
                new[] { ready.ObjectGeneration, ready.ObjectGeneration },
                host.State.PendingHandlerGenerations
            );
        }
        finally
        {
            host.State.ReleaseOnClosing.TrySetResult();
            host.Store.ReleasePut.TrySetResult();
            await close.WaitAsync(Wait);
        }
    }
}
