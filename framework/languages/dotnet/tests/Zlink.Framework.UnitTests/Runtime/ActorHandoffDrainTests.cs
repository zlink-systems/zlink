using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorHandoffDrainTests(Xunit.Abstractions.ITestOutputHelper output)
{
    private static readonly TimeSpan AdmissionLifetime = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan ObservationTimeout = TimeSpan.FromSeconds(2);
    private const string MeshName = "handoff-drain";
    private const string ActorType = "handoff-drain-actor";
    private const string RoomType = "handoff-drain-room";

    [Fact]
    public async Task RejectedAdmission_IsDrainSafeWhileItsReplyRemainsReusable()
    {
        var clock = new ControllableTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(clock);
        var request = AdmissionRequest(clock, "rejected");
        var calls = 0;
        ValueTask<ZLinkRemoteActorAdmissionReply> Reject(CancellationToken ct)
        {
            calls++;
            return ValueTask.FromResult(Reply(request, accepted: false));
        }
        try
        {
            var reply = await admissions.AdmitAsync(request, "room", Reject, default);
            Assert.True((await admissions.SnapshotDrainAsync()).IsSafe);
            await admissions.WaitUntilDrainSafeAsync(default).WaitAsync(ObservationTimeout);
            var replay = await admissions.AdmitAsync(request, "room", Reject, default);
            Assert.Same(reply, replay);
            Assert.Same(reply, await admissions.TryGetReplyAsync(request, "room"));
            Assert.Equal(1, calls);
            Assert.True((await admissions.SnapshotDrainAsync()).IsSafe);
        }
        finally
        {
            await admissions.ResetGenerationAsync();
        }
    }

    [Fact]
    public async Task RegisteredRejection_DoesNotDisagreeWithTheDrainWaiter()
    {
        var clock = new ControllableTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(clock);
        var request = AdmissionRequest(clock, "registered-rejected");
        var reply = Reply(request, accepted: false);
        try
        {
            await admissions.RegisterAsync(request, "room", reply);
            Assert.True((await admissions.SnapshotDrainAsync()).IsSafe);
            await admissions.WaitUntilDrainSafeAsync(default).WaitAsync(ObservationTimeout);
            Assert.Same(reply, await admissions.TryGetReplyAsync(request, "room"));
        }
        finally
        {
            await admissions.ResetGenerationAsync();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task MixedReplies_OnlyAcceptedReservationDelaysDrain(bool rejectedFirst)
    {
        var clock = new ControllableTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(clock);
        var accepted = AdmissionRequest(clock, "accepted");
        var rejected = AdmissionRequest(clock, "rejected");
        var rejection = Reply(rejected, accepted: false);
        try
        {
            if (rejectedFirst)
                await admissions.AdmitAsync(
                    rejected,
                    "room",
                    _ => ValueTask.FromResult(rejection),
                    default
                );
            await admissions.AdmitAsync(
                accepted,
                "room",
                _ => ValueTask.FromResult(Reply(accepted, accepted: true)),
                default
            );
            if (!rejectedFirst)
                await admissions.AdmitAsync(
                    rejected,
                    "room",
                    _ => ValueTask.FromResult(rejection),
                    default
                );
            Assert.False((await admissions.SnapshotDrainAsync()).IsSafe);
            var wait = admissions.WaitUntilDrainSafeAsync(default);
            Assert.False(wait.IsCompleted);
            await admissions.CompleteAsync(accepted.HandoffId);
            Assert.True((await admissions.SnapshotDrainAsync()).IsSafe);
            await wait.WaitAsync(ObservationTimeout);
            Assert.Same(rejection, await admissions.TryGetReplyAsync(rejected, "room"));
        }
        finally
        {
            await admissions.ResetGenerationAsync();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RejectedReplay_RejectsChangedIdentityWithoutReexecutingCallback(
        bool changedTarget
    )
    {
        var clock = new ControllableTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(clock);
        var request = AdmissionRequest(clock, "rejected-identity");
        var calls = 0;
        ValueTask<ZLinkRemoteActorAdmissionReply> Reject(CancellationToken ct)
        {
            calls++;
            return ValueTask.FromResult(Reply(request, accepted: false));
        }
        try
        {
            var reply = await admissions.AdmitAsync(request, "room", Reject, default);
            await Assert.ThrowsAsync<InvalidOperationException>(() =>
                admissions
                    .AdmitAsync(
                        changedTarget ? request : request with { Request = [1] },
                        changedTarget ? "different-room" : "room",
                        Reject,
                        default
                    )
                    .AsTask()
            );
            Assert.Same(reply, await admissions.AdmitAsync(request, "room", Reject, default));
            Assert.Equal(1, calls);
            Assert.True((await admissions.SnapshotDrainAsync()).IsSafe);
        }
        finally
        {
            await admissions.ResetGenerationAsync();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task AcceptedAbortOrExpiry_ReleasesDrainWhileRejectedReplyRemains(bool expire)
    {
        var clock = new ControllableTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(clock);
        var accepted = AdmissionRequest(clock, "accepted-release");
        try
        {
            await admissions.AdmitAsync(
                accepted,
                "room",
                _ => ValueTask.FromResult(Reply(accepted, accepted: true)),
                default
            );
            clock.Advance(TimeSpan.FromSeconds(1));
            var rejected = AdmissionRequest(clock, "rejected-retained");
            var rejection = Reply(rejected, accepted: false);
            await admissions.AdmitAsync(
                rejected,
                "room",
                _ => ValueTask.FromResult(rejection),
                default
            );
            var wait = admissions.WaitUntilDrainSafeAsync(default);
            Assert.False((await admissions.SnapshotDrainAsync()).IsSafe);
            Assert.False(wait.IsCompleted);
            if (expire)
                clock.Advance(AdmissionLifetime - TimeSpan.FromSeconds(1));
            else
                await admissions.AbortAsync(accepted.HandoffId);
            await wait.WaitAsync(ObservationTimeout);
            Assert.True((await admissions.SnapshotDrainAsync()).IsSafe);
            Assert.Null(await admissions.TryGetReplyAsync(accepted, "room"));
            Assert.Same(rejection, await admissions.TryGetReplyAsync(rejected, "room"));
        }
        finally
        {
            await admissions.ResetGenerationAsync();
        }
    }

    [Fact]
    public async Task GenerationReset_ClearsRepliesAndReleasesThePreviousDrainWaiter()
    {
        var clock = new ControllableTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(clock);
        var accepted = AdmissionRequest(clock, "accepted-reset");
        var rejected = AdmissionRequest(clock, "rejected-reset");
        try
        {
            await admissions.AdmitAsync(
                accepted,
                "room",
                _ => ValueTask.FromResult(Reply(accepted, accepted: true)),
                default
            );
            await admissions.AdmitAsync(
                rejected,
                "room",
                _ => ValueTask.FromResult(Reply(rejected, accepted: false)),
                default
            );
            var wait = admissions.WaitUntilDrainSafeAsync(default);
            var before = await admissions.SnapshotDrainAsync();
            Assert.False(before.IsSafe);
            Assert.False(wait.IsCompleted);
            await admissions.ResetGenerationAsync();
            await wait.WaitAsync(ObservationTimeout);
            var after = await admissions.SnapshotDrainAsync();
            Assert.True(after.IsSafe);
            Assert.NotEqual(before.Epoch, after.Epoch);
            Assert.Null(await admissions.TryGetReplyAsync(accepted, "room"));
            Assert.Null(await admissions.TryGetReplyAsync(rejected, "room"));
            Assert.Equal(0, clock.ActiveTimerCount);
            await admissions.RegisterAsync(accepted, "room", Reply(accepted, accepted: true));
            Assert.False((await admissions.SnapshotDrainAsync()).IsSafe);
            var nextWait = admissions.WaitUntilDrainSafeAsync(default);
            Assert.False(nextWait.IsCompleted);
            await admissions.CompleteAsync(accepted.HandoffId);
            await nextWait.WaitAsync(ObservationTimeout);
            Assert.True((await admissions.SnapshotDrainAsync()).IsSafe);
        }
        finally
        {
            await admissions.ResetGenerationAsync();
        }
    }

    private static ZLinkRemoteActorAdmissionRequest AdmissionRequest(
        TimeProvider clock,
        string handoffId
    ) =>
        new(
            handoffId,
            "handoff-drain-actor",
            "source",
            [2],
            "application/json",
            [],
            handoffId,
            clock.GetUtcNow().Add(AdmissionLifetime).ToUnixTimeMilliseconds()
        );

    private static ZLinkRemoteActorAdmissionReply Reply(
        ZLinkRemoteActorAdmissionRequest request,
        bool accepted
    ) => new(accepted, "application/json", [42], request.DeadlineUnixTimeMilliseconds);

    [Theory]
    [InlineData(1, true)]
    [InlineData(2, true)]
    [InlineData(3, true)]
    [InlineData(0, false)]
    public async Task RejectedJoin_ShutdownDoesNotWaitForReplyCache(int run, bool remote)
    {
        var traceDirectory = Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIRECTORY");
        var traceRoot = traceDirectory is null
            ? null
            : Path.Combine(traceDirectory, $"rejected-join-drain-{run}");
        using var flow = traceRoot is null
            ? null
            : new TestHostMessageFlowListener(traceRoot + ".flow");
        var original = Console.Error;
        using var debug = traceRoot is null
            ? null
            : new StreamWriter(traceRoot + ".dispatch") { AutoFlush = true };
        if (debug is not null)
            Console.SetError(TextWriter.Synchronized(debug));
        try
        {
            var store = new ZLinkInMemoryProviderLocationStore();
            var roots = new SynchronizedRelocationStore();
            await using var source = await Host.Start(store, roots, !remote);
            var actorId = $"handoff-drain-{run}-{Guid.NewGuid():N}";
            _ = await source
                .Provider.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, ActorType)
                .InMesh(MeshName)
                .Request(ZLinkMessage.Empty)
                .Async();
            await using var other = remote ? await Host.Start(store, roots, true) : null;
            var target = other ?? source;
            var room = await target
                .Provider.GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate($"room-{Guid.NewGuid():N}", RoomType)
                .InMesh(MeshName)
                .Request(ZLinkMessage.Empty)
                .Async();
            await source
                .Provider.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new Join(room.Spot.SpotId))
                .Async<Ack>();
            var completion = await source.Probe.Completed.Task.WaitAsync(TimeSpan.FromSeconds(15));
            Assert.IsType<ZLinkActorJoinCompletion.Rejected>(completion);
            var clock = Stopwatch.StartNew();
            var shutdown = target
                .Provider.GetRequiredService<IZLinkFrameworkRuntime>()
                .ShutdownAsync()
                .AsTask();
            await shutdown;
            clock.Stop();
            var line =
                $"run={run} remote={remote} rejected=True shutdown_ms={clock.Elapsed.TotalMilliseconds:F3} outcome={shutdown.Result}";
            output.WriteLine(line);
            Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, shutdown.Result.Outcome);
            Assert.Equal(ZLinkFrameworkTerminationReason.None, shutdown.Result.Reason);
            Assert.True(clock.Elapsed < ObservationTimeout, line);
        }
        finally
        {
            Console.SetError(original);
        }
    }

    internal sealed class Probe
    {
        public TaskCompletionSource<ZLinkActorJoinCompletion> Completed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    internal sealed record Join(string SpotId);

    internal sealed record Ack;

    internal sealed class Actor(string actorId, IZLinkActorContext context, Probe probe)
        : IZLinkActor
    {
        public string ActorId => actorId;
        public IZLinkActorContext Context => context;

        public ValueTask OnJoinCompletedAsync(
            ZLinkActorJoinCompletion completion,
            CancellationToken ct
        )
        {
            probe.Completed.TrySetResult(completion);
            return ValueTask.CompletedTask;
        }
    }

    internal sealed class Factory(Probe probe) : IZLinkActorFactory<Actor>
    {
        public ValueTask<Actor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken ct = default
        ) => ValueTask.FromResult(new Actor(context.ActorId, context, probe));
    }

    internal sealed class Adapter : IZLinkActorRelocationAdapter<Actor>
    {
        public ValueTask<byte[]> CaptureAsync(Actor actor, CancellationToken ct) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreAsync(
            Actor actor,
            ReadOnlyMemory<byte> payload,
            CancellationToken ct
        ) => ValueTask.CompletedTask;
    }

    internal sealed class Entry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<Actor>
    {
        public IZLinkEntrySpotContext Context => context;

        public void Configure() => Context.Handlers.AddActorPacket<Handler, Actor>(nameof(Join));

        public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
            Actor actor,
            ZLinkMessage request,
            CancellationToken ct
        ) => ValueTask.FromResult(ZLinkActorCreateResponse.Accept());

        public ValueTask OnJoinedActorAsync(Actor actor, CancellationToken ct) =>
            ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(Actor actor, CancellationToken ct) =>
            ValueTask.CompletedTask;
    }

    internal sealed class Room(IZLinkSpotContext context) : IZLinkSpot<Actor>
    {
        public IZLinkSpotContext Context => context;

        public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken ct
        ) => ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken ct
        ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject());

        public ValueTask OnJoinedActorAsync(Actor actor, CancellationToken ct) =>
            ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(Actor actor, CancellationToken ct) =>
            ValueTask.CompletedTask;
    }

    internal sealed class Handler : IZLinkEntrySpotActorRequestHandler<Entry, Actor, Join, Ack>
    {
        public ValueTask<Ack> HandleAsync(
            Entry entry,
            Actor actor,
            IZLinkMessageContext context,
            Join request,
            CancellationToken ct
        )
        {
            actor.Context.JoinSpot(request.SpotId, request).Defer();
            return ValueTask.FromResult(new Ack());
        }
    }

    internal sealed class Host(ServiceProvider provider, IHostedService hosted, Probe probe)
        : IAsyncDisposable
    {
        public ServiceProvider Provider => provider;
        public Probe Probe => probe;

        public static async Task<Host> Start(
            IZLinkLocationStore store,
            IZLinkRelocationStore roots,
            bool room
        )
        {
            var services = new ServiceCollection();
            var probe = new Probe();
            services.AddSingleton(probe);
            services.AddTransient<Handler>();
            services.AddZLinkFramework(options =>
            {
                options.AddLocationStore(store);
                options.AddRelocationStore(roots);
                options.ConfigureLocations().PollingInterval = TimeSpan.FromMilliseconds(10);
                options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
                var objects = options
                    .AddRouteMesh(MeshName)
                    .Listen("tcp://127.0.0.1:0")
                    .SetActorLimit(100)
                    .SetSpotLimit(100)
                    .Objects()
                    .Server()
                    .AddEntrySpot<Entry>()
                    .AddActorFactory<Actor, Factory>(
                        ActorType,
                        f => f.PreserveStateWith<Adapter>()
                    );
                if (room)
                    objects.AddSpotFactory<Room>(RoomType, f => f.DisableRelocation());
            });
            var provider = services.BuildServiceProvider();
            var hosted = provider
                .GetServices<IHostedService>()
                .Single(s => s is ZLinkFrameworkHostedService);
            try
            {
                await hosted.StartAsync(default);
            }
            catch
            {
                await provider.DisposeAsync();
                throw;
            }
            return new Host(provider, hosted, probe);
        }

        public async ValueTask DisposeAsync()
        {
            await hosted.StopAsync(default);
            await provider.DisposeAsync();
        }
    }
}
