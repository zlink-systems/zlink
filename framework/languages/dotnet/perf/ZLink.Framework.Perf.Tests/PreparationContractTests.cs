using System.Collections.Concurrent;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink;
using Xunit;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf.Tests;

public sealed class PreparationContractTests
{
    private static RoleConfig Config(string role = "Server", string scenario = "cs-remote-session-actor-echo") => new(
        "test", "test-cell", new string('a', 64), "actor", 0, scenario, null, null, "mesh",
        null, null, "", "", false, role, null, [], ["a", "b", "c"], "Immediate",
        new(64, 4096, 4096, 2, 5, 1, 1, null, 3, 2, 1000, 1000, 5000, 30000, 5000, 1000), []);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task OrdinarySpotTargetRequiresSuccessfulPublicPreparation(bool rejected)
    {
        var config = Config(scenario: "s2s-channel-to-spot-request-echo") with { actorIds = [], spotIds = ["spot"] };
        using var measurement = new Measurement(config, false);
        var spots = new PreparingSpots(rejected);
        using var services = new ServiceCollection().AddSingleton(config).AddSingleton(measurement)
            .AddSingleton<IZLinkSpotManager>(spots).AddSingleton<IZLinkFrameworkRuntime>(new ReadyHost())
            .BuildServiceProvider();
        Assert.False(ServerApplication.Ready(services).objectsReady);
        var preparing = new PerfScenario(services, measurement).PrepareAsync(CancellationToken.None);
        Assert.Null(measurement.ObjectPreparationEvidence);
        Assert.False(ServerApplication.Ready(services).objectsReady);
        spots.Release.TrySetResult();
        await preparing;
        Assert.True(spots.Completed);
        Assert.Equal(!rejected, ServerApplication.Ready(services).objectsReady);
        Assert.False(ServerApplication.Ready(services).consumersReady);
        Assert.Equal(rejected, measurement.HasErrors);
    }

    [Theory]
    [InlineData(false, "cs-remote-session-actor-echo")]
    [InlineData(true, "cs-remote-session-actor-echo")]
    [InlineData(false, "cs-local-session-actor-echo")]
    [InlineData(true, "cs-local-session-actor-echo")]
    [InlineData(false, "s2s-channel-to-actor-request-echo")]
    [InlineData(true, "s2s-channel-to-actor-request-echo")]
    public async Task CsActorTargetRequiresCompletedPublicPreparationBeforeObjectsReady(bool rejected, string scenarioName)
    {
        var config = Config(scenario: scenarioName);
        using var measurement = new Measurement(config, false);
        var actors = new PreparingActors(rejected);
        using var services = new ServiceCollection().AddSingleton(config).AddSingleton(measurement)
            .AddSingleton<IZLinkActorManager>(actors).AddSingleton<IZLinkFrameworkRuntime>(new ReadyHost())
            .BuildServiceProvider();
        var scenario = new PerfScenario(services, measurement);

        Assert.False(ServerApplication.Ready(services).objectsReady);
        var preparing = scenario.PrepareAsync(CancellationToken.None);
        Assert.Equal(2, actors.Requested.Count); // Existing configured connectConcurrency.
        Assert.Null(measurement.ObjectPreparationEvidence);
        Assert.False(ServerApplication.Ready(services).objectsReady);
        actors.Release.TrySetResult();
        await preparing;

        Assert.Equal(config.actorIds.Order(), actors.Completed.Order());
        Assert.All(actors.Meshes, mesh => Assert.Equal(config.meshName, mesh));
        Assert.All(actors.Timeouts, timeout => Assert.Equal(TimeSpan.FromSeconds(30), timeout));
        var ready = ServerApplication.Ready(services);
        Assert.Equal(!rejected, ready.objectsReady);
        Assert.False(ready.consumersReady); // Preparation alone is not a typed consumer receipt.
        Assert.False(ready.ready);
        Assert.Equal(rejected, measurement.HasErrors);
        if (!rejected)
        {
            var preparation = measurement.ObjectPreparationEvidence;
            measurement.SetupEvidence = [new { kind = "publicActorCreatedAndSessionBound", observedValue = "a" }];
            Assert.Same(preparation, measurement.ObjectPreparationEvidence);
            Assert.Equal(2, measurement.SetupEvidence.Length);
            Assert.True(ServerApplication.Ready(services).objectsReady);
            Assert.True(ServerApplication.Ready(services).consumersReady);
            // Replacing the latest receipt retains one preparation and one receipt.
            measurement.SetupEvidence = [new { kind = "typedProbeReceipt", observedValue = "probe" }];
            Assert.Same(preparation, measurement.ObjectPreparationEvidence);
            Assert.Equal(2, measurement.SetupEvidence.Length);
        }
    }

    [Theory]
    [InlineData("Client", "cs-remote-session-actor-echo")]
    [InlineData("None", "channel-echo-only")]
    [InlineData("None", "fanout-publish")]
    public async Task OtherTargetPreparationIsNoOpWithoutInventingConsumerEvidence(string role, string scenario)
    {
        var config = Config(role, scenario) with { mode = scenario == "fanout-publish" ? "publish" : "request" };
        using var measurement = new Measurement(config, false);
        using var services = new ServiceCollection().BuildServiceProvider();
        await new PerfScenario(services, measurement).PrepareAsync(CancellationToken.None);
        Assert.Empty(measurement.SetupEvidence);
        Assert.False(measurement.HasErrors);
    }

    private sealed class PreparingActors(bool rejected) : IZLinkActorManager
    {
        private readonly bool _rejected = rejected;
        public readonly TaskCompletionSource Release = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public readonly ConcurrentQueue<string> Requested = new(), Completed = new(), Meshes = new();
        public readonly ConcurrentQueue<TimeSpan> Timeouts = new();
        public IZLinkActorGetOrCreateCall GetOrCreate(string actorId, string actorType)
        {
            Assert.Equal("perf-actor", actorType);
            Requested.Enqueue(actorId);
            return new Call(this, actorId);
        }
        public IZLinkActorCreateCall Create(string actorId, string actorType) => throw new NotSupportedException();
        public ValueTask<ActorRef?> FindAsync(string actorId, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public ValueTask<SpotRef?> FindSpotAsync(string actorId, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public ValueTask<bool> DestroyAsync(ActorRef actor, CancellationToken cancellationToken = default) => throw new NotSupportedException();

        private sealed class Call(PreparingActors owner, string actorId) : IZLinkActorGetOrCreateCall
        {
            public IZLinkActorGetOrCreateCall InMesh(string meshName) { owner.Meshes.Enqueue(meshName); return this; }
            public IZLinkActorGetOrCreateCall Timeout(TimeSpan timeout) { owner.Timeouts.Enqueue(timeout); return this; }
            public IZLinkActorGetOrCreateCall Request(ZLinkMessage request) => throw new NotSupportedException();
            public IZLinkActorGetOrCreateCall Request<TRequest>(TRequest request) => throw new NotSupportedException();
            public async ValueTask<ZLinkActorCreateResult> Async(CancellationToken cancellationToken = default)
            {
                await owner.Release.Task.WaitAsync(cancellationToken);
                owner.Completed.Enqueue(actorId);
                return owner._rejected && actorId == "a" ? new ZLinkActorCreateResult.Rejected(null)
                    : new ZLinkActorCreateResult.Existing(new ActorRef(actorId, 1, "mesh", RoutingId.From("target")));
            }
            public ValueTask<ZLinkActorCreateResult> Yield(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        }
    }

    private sealed class ReadyHost : IZLinkFrameworkRuntime
    {
        public ZLinkFrameworkRuntimeStatus Status => new(ZLinkFrameworkRuntimeState.Serving, true, true,
            null, null, null, 0, DateTimeOffset.UtcNow);
        public IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> ObserveAsync(CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public void ResetCapacityMetrics() => throw new NotSupportedException();
        public ValueTask<ZLinkFrameworkRelocationResult> RelocateAsync(ZLinkFrameworkRelocationOptions options, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public ValueTask<ZLinkFrameworkTerminationResult> ShutdownAsync(TimeSpan? deadline = null, CancellationToken cancellationToken = default) => throw new NotSupportedException();
    }

    private sealed class PreparingSpots(bool rejected) : IZLinkSpotManager, IZLinkSpotGetOrCreateCall
    {
        public readonly TaskCompletionSource Release = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool Completed;
        public IZLinkSpotGetOrCreateCall GetOrCreate(string spotId, string spotType)
        {
            Assert.Equal("spot", spotId);
            Assert.Equal("perf-spot", spotType);
            return this;
        }
        public IZLinkSpotGetOrCreateCall InMesh(string meshName) { Assert.Equal("mesh", meshName); return this; }
        public IZLinkSpotGetOrCreateCall Timeout(TimeSpan timeout) { Assert.Equal(TimeSpan.FromSeconds(30), timeout); return this; }
        public async ValueTask<ZLinkSpotCreateResult> Async(CancellationToken cancellationToken = default)
        {
            await Release.Task.WaitAsync(cancellationToken);
            Completed = true;
            return new(new SpotRef("spot", 1, "mesh", RoutingId.From("target")),
                rejected ? ZLinkSpotCreateState.Rejected : ZLinkSpotCreateState.Created, null);
        }
        public IZLinkSpotCreateCall Create(string spotType) => throw new NotSupportedException();
        public ValueTask<SpotRef?> FindAsync(string spotId, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public ValueTask<bool> CloseAsync(SpotRef spot, CancellationToken cancellationToken = default) => throw new NotSupportedException();
        public IZLinkSpotGetOrCreateCall Request(ZLinkMessage request) => throw new NotSupportedException();
        public IZLinkSpotGetOrCreateCall Request<TRequest>(TRequest request) => throw new NotSupportedException();
        public ValueTask<ZLinkSpotCreateResult> Yield(CancellationToken cancellationToken = default) => throw new NotSupportedException();
    }
}
