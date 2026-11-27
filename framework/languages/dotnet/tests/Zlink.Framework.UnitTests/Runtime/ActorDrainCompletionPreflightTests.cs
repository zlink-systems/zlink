using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorDrainCompletionPreflightTests
{
    [Fact]
    public async Task Host_preflight_does_not_read_authority_for_ordinary_actors()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var store = new CountingStore();
        var (drain, _) = CreateDrain(services, store);

        Assert.False(await drain.HasCompletionBlockerAsync(CancellationToken.None));
        Assert.Empty(store.Reads);
    }

    [Fact]
    public async Task Host_preflight_reads_a_pending_candidate_once_and_forgets_it_after_ack()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var store = new CountingStore();
        var (drain, runtime) = CreateDrain(services, store);
        var actor = runtime.GetOrCreateActorState("ordinary-2");
        var rid = actor.NativeActorRef!.Value.NodeRid;
        var relocation = new ZLinkAuthorityRelocation(
            Guid.NewGuid(),
            ServiceWireCodec.RelocationPhase.Activated,
            new("01", 12, "source", 7),
            new(rid.ToHex(), 13, "target", 3, 1),
            new("01", 12, "source", 7, "original"),
            new string('a', 64),
            1,
            new(1, 1, 1, "completion", new string('b', 64))
        );
        store.Snapshot = new(
            "pending",
            ReadOnlyMemory<byte>.Empty,
            1,
            2,
            "target",
            3,
            new(
                ZLinkPlacementAllocationState.Active,
                ZLinkPlacementObjectKind.Actor,
                "Test.Actor",
                new("mesh", rid),
                13,
                new(1, 0, null)
            ),
            null,
            DateTimeOffset.UtcNow
        )
        {
            Relocation = relocation,
        };
        actor.TrackRelocationCompletion(relocation);
        actor.TrackRelocationCompletion(relocation);
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId);

        Assert.True(await drain.HasCompletionBlockerAsync(CancellationToken.None));
        Assert.Equal([key], store.Reads);

        store.Reads.Clear();
        store.Snapshot = store.Snapshot with
        {
            StoreVersion = "acknowledged",
            Relocation = relocation with
            {
                Completion = relocation.Completion! with { PendingDeliveries = 0 },
            },
        };
        Assert.False(await drain.HasCompletionBlockerAsync(CancellationToken.None));
        Assert.Equal([key], store.Reads);

        store.Reads.Clear();
        Assert.False(await drain.HasCompletionBlockerAsync(CancellationToken.None));
        Assert.Empty(store.Reads);
    }

    private static (ZLinkActorDrainCoordinator Drain, ZLinkFrameworkRuntime Runtime) CreateDrain(
        IServiceProvider services,
        CountingStore store
    )
    {
        var registration = new ZLinkFrameworkRegistration
        {
            ImplicitHandlerAutoRegistrationEnabled = false,
        };
        registration.Locations._repository = store;
        registration.FreezeScannedHandlerCatalog();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration
            )
        );
        var drain = new ZLinkActorDrainCoordinator(
            runtime.StandaloneActorRelocationRuntime,
            runtime._actorSessionManager,
            services,
            registration
        );
        foreach (var id in new[] { "ordinary-1", "ordinary-2", "ordinary-3" })
        {
            var actor = runtime.GetOrCreateActorState(id);
            actor.BindNativeActorRef(new(RoutingId.From("local"), id, 1));
            actor.BindActorInstance(new TestActor(id));
        }

        return (drain, runtime);
    }

    private sealed class CountingStore : ZLinkLocationStoreTestDouble
    {
        internal List<ZLinkAuthorityKey> Reads { get; } = [];
        internal ZLinkAuthoritySnapshot? Snapshot { get; set; }

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        )
        {
            Reads.Add(key);
            return ValueTask.FromResult<ZLinkAuthorityReadResult>(
                Snapshot is { } snapshot
                    ? new ZLinkAuthorityReadResult.Found(snapshot)
                    : new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UtcNow)
            );
        }
    }
}
